/* The Jellyfin client inside the title: http_client.c and jellyfin.c run from a thread of their own.
 * Every log line starts with JFGATE so one grep of klog shows the whole story.
 *
 * The worker never fetches everything. Each time round it asks what the screen is looking at
 * (jf_gate_want) and does the most useful single piece of work:
 *   1. the category's first page, which says how many titles it has
 *   2. the pages of titles around the wheel
 *   3. the nearest poster that is missing
 *   4. the A-Z offsets of the category (when it is in name order)
 *   5. the remaining pages, nearest first
 * so scrubbing to another part of a library re-aims it within one request.
 *
 * A category is a library, or one of the server's lists for the user (Continue Watching, Next Up,
 * Recently Added, Favourites), or the search. Sorting, filtering, searching and refreshing start a
 * category over: its generation goes up, and whatever the worker was fetching for the old one is
 * thrown away when it arrives. */
#include "jf_gate.h"
#include "jf_gate_private.h"

#include <cctype>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>

extern "C" {
#include "platform.h"
#include "jf_config.h"
#include "jf_types.h"
#include "http_client.h"
#include "jellyfin.h"
}
#include <json-c/json.h>

// The decoder's implementation is compiled once, in base/VulkanglTFModel.cpp
#include "jf_conf.hpp"
#include "stb_image.h"

namespace {
constexpr int kPage = JF_GATE_PAGE;
constexpr int kRadius = 10; // posters wanted each side of the wheel

enum Kind { kLibrary = 0, kResume = 1, kNextUp = 2, kRecent = 3, kFavourites = 4, kSearch = 5, kBrowse = 6, kQueue = 7 };

const char *kFields = "Overview,Genres,SortName,ProductionYear,CommunityRating,RunTimeTicks,UserData,DateCreated,ChildCount";
const char *kSortBy[JF_GATE_SORTS] = { "SortName", "ProductionYear,SortName", "DateCreated,SortName",
	"CommunityRating,SortName", "Runtime,SortName" };
const char *kSortOrder[JF_GATE_SORTS] = { "Ascending", "Descending", "Descending", "Descending", "Descending" };
const char *kSortLabel[JF_GATE_SORTS] = { "Title A-Z", "Newest release", "Recently added", "Top rated", "Longest first" };
const char *kFilterQuery[JF_GATE_FILTERS] = { "", "&Filters=IsUnplayed", "&Filters=IsFavorite" };
const char *kFilterLabel[JF_GATE_FILTERS] = { "Everything", "Unwatched", "Favourites" };

constexpr int kRecentMax = 60;
constexpr int kSearchMax = 60;

struct Entry {
	std::string id, imageId, name, sortName, kind, overview, genre, imageTag;
	long long runtime{ 0 }, position{ 0 };
	int year{ 0 };
	int addedDays{ -1 };
	float rating{ 0.0f };
	bool favorite{ false };
	bool played{ false };
	std::string seriesId, backdropId, parentName;
	int childCount{ 0 };
};

struct Category {
	std::string id, name;
	int kind{ kLibrary };
	int sort{ 0 }, filter{ 0 };
	std::string term;
	int generation{ 0 };
	int total{ -1 };
	std::vector<Entry> entries;               // sized once, when the total is known
	std::vector<unsigned char> pageState;     // 0 not asked, 1 in, 2 gave up
	std::vector<unsigned char> pageTries;
	std::vector<unsigned char> posterState;   // 0 none, 1 fetched or being fetched, 2 none to be had
	int lettersState{ 0 };                    // 0 unknown, 1 from the server, 2 counted here, -1 none
	int letters[27] = {};
	int pagesIn{ 0 };
};

// What a request needs to know about its category, copied under the lock
struct Params {
	int kind{ kLibrary };
	std::string id, term;
	int sort{ 0 }, filter{ 0 };
	int generation{ 0 };
	bool lettered{ false };
};

std::thread worker;
std::atomic<bool> started{ false };
std::atomic<int> finished{ 0 }; // 0 running, 1 stopped, 2 failed to sign in
std::atomic<bool> stopRequested{ false };
std::atomic<bool> paused{ false };
std::atomic<int> wantCategory{ 0 };
std::atomic<int> wantCentre{ 3 };
std::atomic<int> categoryCount{ 0 };
std::atomic<int> startCategory{ 0 };

std::mutex lock; // guards cats' contents, ready and favourite jobs
std::vector<std::unique_ptr<Category>> cats; // filled once, before categoryCount is set
std::deque<JfGatePoster> ready;
struct FavouriteJob {
	std::string id;
	bool on;
};
std::deque<FavouriteJob> favouriteJobs;

struct DetailsRecord {
	int state{ 0 }; // 0 asked, 1 here, 2 failed
	JfGateDetails details;
};
std::map<std::string, DetailsRecord> detailsCache;
std::deque<std::string> detailsOrder; // oldest first, for trimming
std::deque<std::string> detailsJobs;

// the episode after the one that played (see jf_gate_next_request), and the titles browsed into
struct NextJob {
	std::string seriesId, episodeId;
};
std::deque<NextJob> nextJobs;
std::atomic<int> nextState{ 0 };
std::atomic<int> nextIndex{ -1 };
int browseCat{ -1 }, queueCat{ -1 };
struct BrowseStep {
	std::string id, name, type;
};
std::vector<BrowseStep> browseStack;

JfSession gSession;
std::atomic<bool> sessionReady{ false };

void sleepMs(int ms)
{
	for (int waited = 0; waited < ms && !stopRequested; waited += 10) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}

void authorization(const JfSession &session, char *out, size_t size)
{
	std::snprintf(out, size,
		"Authorization: MediaBrowser Client=\"%s\", Device=\"%s\", DeviceId=\"%s\", "
		"Version=\"%s\", Token=\"%s\"\r\n",
		JF_CLIENT_NAME, JF_DEVICE_NAME, JF_DEVICE_ID, JF_CLIENT_VERSION, session.access_token);
}

// ---- JSON ----

json_object *jget(json_object *o, const char *key)
{
	json_object *v = nullptr;
	if (!o || !json_object_object_get_ex(o, key, &v)) {
		return nullptr;
	}
	return v;
}

std::string jstr(json_object *o, const char *key)
{
	json_object *v = jget(o, key);
	if (!v || !json_object_is_type(v, json_type_string)) {
		return std::string();
	}
	return json_object_get_string(v);
}

long long jint(json_object *o, const char *key)
{
	json_object *v = jget(o, key);
	return v ? static_cast<long long>(json_object_get_int64(v)) : 0;
}

double jnum(json_object *o, const char *key)
{
	json_object *v = jget(o, key);
	return v ? json_object_get_double(v) : 0.0;
}

bool jbool(json_object *o, const char *key)
{
	json_object *v = jget(o, key);
	return v && json_object_get_boolean(v);
}

// GET a path and parse the answer; null when anything went wrong
json_object *getJson(const JfSession &session, const char *path)
{
	char auth[4096];
	authorization(session, auth, sizeof(auth));
	HttpResponse response;
	std::memset(&response, 0, sizeof(response));
	if (http_get(jfconf::host(), jfconf::port(), path, auth, &response) != 0) {
		say("JFGATE: request failed: %.160s", path);
		return nullptr;
	}
	if (response.status != 200 || !response.body) {
		say("JFGATE: HTTP %d for %.160s", response.status, path);
		http_response_free(&response);
		return nullptr;
	}
	json_object *root = json_tokener_parse(response.body);
	http_response_free(&response);
	if (!root) {
		say("JFGATE: unreadable answer for %.160s", path);
	}
	return root;
}

// 0 for '#' (digits, symbols, anything that is not a letter), 1..26 for A..Z
int bucketOf(const std::string &name)
{
	if (name.empty()) {
		return 0;
	}
	const unsigned char c = static_cast<unsigned char>(name[0]);
	if (c >= 'a' && c <= 'z') {
		return c - 'a' + 1;
	}
	if (c >= 'A' && c <= 'Z') {
		return c - 'A' + 1;
	}
	return 0;
}

std::string urlEncode(const std::string &text)
{
	static const char hex[] = "0123456789ABCDEF";
	std::string out;
	for (const char ch : text) {
		const unsigned char c = static_cast<unsigned char>(ch);
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
			out += ch;
		} else {
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 15];
		}
	}
	return out;
}

// Days since the server first saw a title, from "2026-09-30T12:00:00.0000000Z"; -1 if unknown. Only
// trusted when the console's clock looks set.
int daysSince(const std::string &stamp)
{
	int y = 0, m = 0, d = 0;
	if (stamp.size() < 10 || std::sscanf(stamp.c_str(), "%d-%d-%d", &y, &m, &d) != 3) {
		return -1;
	}
	const std::time_t now = std::time(nullptr);
	if (now < 1700000000) {
		return -1;
	}
	// days from civil (Howard Hinnant)
	auto days = [](int yy, int mm, int dd) {
		yy -= mm <= 2 ? 1 : 0;
		const int era = (yy >= 0 ? yy : yy - 399) / 400;
		const int yoe = yy - era * 400;
		const int doy = (153 * (mm + (mm > 2 ? -3 : 9)) + 2) / 5 + dd - 1;
		const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
		return era * 146097 + doe - 719468;
	};
	const int then = days(y, m, d);
	const int today = static_cast<int>(now / 86400);
	return today >= then ? today - then : -1;
}

bool lettered(const Category &c)
{
	return (c.kind == kLibrary || c.kind == kFavourites) && c.sort == 0;
}

bool sortable(const Category &c)
{
	return c.kind == kLibrary || c.kind == kFavourites;
}

int capOf(const Category &c)
{
	return c.kind == kRecent ? kRecentMax : c.kind == kSearch ? kSearchMax : 0;
}

Params paramsOf(int cat)
{
	std::lock_guard<std::mutex> guard(lock);
	const Category &c = *cats[static_cast<size_t>(cat)];
	Params p;
	p.kind = c.kind;
	p.id = c.id;
	p.term = c.term;
	p.sort = c.sort;
	p.filter = c.filter;
	p.generation = c.generation;
	p.lettered = lettered(c);
	return p;
}

// The path for one request: titles from `start`, at most `limit`; with `below` set, only the titles that
// sort before that letter (for the A-Z offsets).
std::string pathFor(const JfSession &session, const Params &p, int start, int limit, char below)
{
	char head[1100];
	const int sort = std::clamp(p.sort, 0, JF_GATE_SORTS - 1);
	const int filter = std::clamp(p.filter, 0, JF_GATE_FILTERS - 1);
	switch (p.kind) {
	case kResume:
		std::snprintf(head, sizeof(head), "/Users/%s/Items/Resume?MediaTypes=Video", session.user_id);
		break;
	case kNextUp:
		std::snprintf(head, sizeof(head), "/Shows/NextUp?UserId=%s", session.user_id);
		break;
	case kRecent:
		std::snprintf(head, sizeof(head),
			"/Users/%s/Items?Recursive=true&IncludeItemTypes=Movie,Series&SortBy=DateCreated&SortOrder=Descending",
			session.user_id);
		break;
	case kFavourites:
		std::snprintf(head, sizeof(head),
			"/Users/%s/Items?Recursive=true&IncludeItemTypes=Movie,Series,BoxSet&Filters=IsFavorite&SortBy=%s&SortOrder=%s",
			session.user_id, kSortBy[sort], kSortOrder[sort]);
		break;
	case kBrowse:
		if (p.term == "BoxSet") {
			std::snprintf(head, sizeof(head), "/Users/%s/Items?ParentId=%s&SortBy=ProductionYear,SortName&SortOrder=Ascending",
				session.user_id, p.id.c_str());
		} else {
			std::snprintf(head, sizeof(head),
				"/Users/%s/Items?ParentId=%s&SortBy=ParentIndexNumber,IndexNumber,SortName&SortOrder=Ascending",
				session.user_id, p.id.c_str());
		}
		break;
	case kQueue:
		std::snprintf(head, sizeof(head), "/Users/%s/Items?Ids=%s", session.user_id, p.id.c_str());
		break;
	case kSearch:
		std::snprintf(head, sizeof(head),
			"/Users/%s/Items?Recursive=true&IncludeItemTypes=Movie,Series,BoxSet&SearchTerm=%s", session.user_id,
			urlEncode(p.term).c_str());
		break;
	default:
		std::snprintf(head, sizeof(head), "/Users/%s/Items?ParentId=%s&Recursive=false&SortBy=%s&SortOrder=%s%s",
			session.user_id, p.id.c_str(), kSortBy[sort], kSortOrder[sort], kFilterQuery[filter]);
		break;
	}
	std::string path = head;
	char tail[400];
	if (below) {
		std::snprintf(tail, sizeof(tail), "&NameLessThan=%c&Limit=1&EnableTotalRecordCount=true", below);
	} else {
		std::snprintf(tail, sizeof(tail), "&StartIndex=%d&Limit=%d&EnableTotalRecordCount=true&Fields=%s", start, limit, kFields);
	}
	return path + tail;
}

// Throws away what the category has and starts again from its first page. The lock is held.
void startOver(int cat)
{
	Category &c = *cats[static_cast<size_t>(cat)];
	c.generation++;
	c.total = -1;
	std::vector<Entry>().swap(c.entries);
	c.pageState.clear();
	c.pageTries.clear();
	c.posterState.clear();
	c.pagesIn = 0;
	c.lettersState = lettered(c) ? 0 : -1;
	std::memset(c.letters, 0, sizeof(c.letters));
	for (auto it = ready.begin(); it != ready.end();) {
		if (it->category == cat) {
			std::free(it->rgba);
			it = ready.erase(it);
		} else {
			++it;
		}
	}
}

// ---- work ----

// Fills the category's arrays once its total is known. The lock is held.
void sizeFor(Category &c, int total)
{
	const int cap = capOf(c);
	if (cap > 0 && total > cap) {
		total = cap;
	}
	c.total = total;
	c.entries.assign(static_cast<size_t>(total), Entry());
	c.pageState.assign(static_cast<size_t>((total + kPage - 1) / kPage), 0);
	c.pageTries.assign(c.pageState.size(), 0);
	c.posterState.assign(static_cast<size_t>(total), 0);
	say("JFGATE: \"%s\" has %d titles", c.name.c_str(), total);
}

// One title of a list, from the server's record of it. `kind` is the category's: Continue Watching, Next Up and the
// next-episode queue show an episode as its series (the poster and the name), a browsed season shows it as itself.
void parseEntry(Entry &e, json_object *o, int kind)
{
	e.id = jstr(o, "Id");
	e.name = jstr(o, "Name");
	e.sortName = jstr(o, "SortName");
	e.kind = jstr(o, "Type");
	e.overview = jstr(o, "Overview");
	e.genre.clear();
	json_object *genres = jget(o, "Genres");
	if (genres && json_object_is_type(genres, json_type_array) && json_object_array_length(genres) > 0) {
		json_object *first = json_object_array_get_idx(genres, 0);
		if (first && json_object_is_type(first, json_type_string)) {
			e.genre = json_object_get_string(first);
		}
	}
	e.imageId = e.id;
	e.imageTag = jstr(jget(o, "ImageTags"), "Primary");
	e.seriesId = jstr(o, "SeriesId");
	e.childCount = static_cast<int>(jint(o, "ChildCount"));
	e.backdropId = e.id;
	if (e.kind == "Episode" || e.kind == "Season") {
		// an episode or a season has no backdrop of its own: its series' is the one to show
		const std::string parent = jstr(o, "ParentBackdropItemId");
		if (!parent.empty()) {
			e.backdropId = parent;
		} else if (!e.seriesId.empty()) {
			e.backdropId = e.seriesId;
		}
	}
	if (e.kind == "Episode") {
		const std::string series = jstr(o, "SeriesName");
		const std::string seriesTag = jstr(o, "SeriesPrimaryImageTag");
		char line[240];
		if (kind == kBrowse) {
			// inside a season: the episode is itself
			std::snprintf(line, sizeof(line), "S%lld  E%lld", jint(o, "ParentIndexNumber"), jint(o, "IndexNumber"));
			e.genre = line;
		} else {
			// elsewhere an episode is shown as its series' poster, under the series' name
			std::snprintf(line, sizeof(line), "S%lldE%lld  %s", jint(o, "ParentIndexNumber"), jint(o, "IndexNumber"),
				e.name.c_str());
			e.genre = line;
			if (!series.empty()) {
				e.name = series;
			}
		}
		const std::string seriesId = e.seriesId;
		if (!seriesId.empty() && !seriesTag.empty()) {
			e.imageId = seriesId;
			e.imageTag = seriesTag;
		}
	} else if (e.kind == "Season") {
		const std::string series = jstr(o, "SeriesName");
		const std::string seriesTag = jstr(o, "SeriesPrimaryImageTag");
		e.genre = series;
		e.parentName = series;
		if (e.imageTag.empty() && !e.seriesId.empty() && !seriesTag.empty()) {
			e.imageId = e.seriesId;
			e.imageTag = seriesTag;
		}
	}
	e.runtime = jint(o, "RunTimeTicks");
	e.year = static_cast<int>(jint(o, "ProductionYear"));
	e.rating = static_cast<float>(jnum(o, "CommunityRating"));
	e.addedDays = daysSince(jstr(o, "DateCreated"));
	json_object *data = jget(o, "UserData");
	e.position = jint(data, "PlaybackPositionTicks");
	e.favorite = jbool(data, "IsFavorite");
	e.played = jbool(data, "Played");
}

bool loadPage(const JfSession &session, int cat, int page, const Params &p)
{
	const std::string path = pathFor(session, p, page * kPage, kPage, 0);
	json_object *root = getJson(session, path.c_str());
	if (!root) {
		return false;
	}
	json_object *array = jget(root, "Items");
	if (!array || !json_object_is_type(array, json_type_array)) {
		json_object_put(root);
		return false;
	}
	const int total = static_cast<int>(jint(root, "TotalRecordCount"));
	const int count = static_cast<int>(json_object_array_length(array));
	{
		std::lock_guard<std::mutex> guard(lock);
		Category &c = *cats[static_cast<size_t>(cat)];
		if (c.generation != p.generation) {
			json_object_put(root); // the category started over while this was on its way
			return true;
		}
		if (c.total < 0) {
			sizeFor(c, total);
		}
		for (int i = 0; i < count; i++) {
			const int pos = page * kPage + i;
			if (pos >= c.total) {
				break;
			}
			json_object *o = json_object_array_get_idx(array, static_cast<size_t>(i));
			Entry &e = c.entries[static_cast<size_t>(pos)];
			parseEntry(e, o, c.kind);
		}
		if (static_cast<size_t>(page) < c.pageState.size()) {
			c.pageState[static_cast<size_t>(page)] = 1;
			c.pagesIn++;
		}
	}
	json_object_put(root);
	return true;
}

// The server's count of titles whose sort name is below `letter`
int countBelow(const JfSession &session, const Params &p, char letter)
{
	const std::string path = pathFor(session, p, 0, 1, letter);
	json_object *root = getJson(session, path.c_str());
	if (!root) {
		return -1;
	}
	const int n = static_cast<int>(jint(root, "TotalRecordCount"));
	json_object_put(root);
	return n;
}

void logLetters(const char *who, const Category &c)
{
	char text[256];
	int used = 0;
	for (int k = 0; k < 27 && used < static_cast<int>(sizeof(text)) - 12; k++) {
		const int next = k < 26 ? c.letters[k + 1] : c.total;
		if (next > c.letters[k]) {
			used += std::snprintf(text + used, sizeof(text) - static_cast<size_t>(used), "%c%d ", k == 0 ? '#' : 'A' + k - 1,
				c.letters[k]);
		}
	}
	say("JFGATE: letters (%s) of \"%s\": %s", who, c.name.c_str(), text);
}

bool loadLetters(const JfSession &session, int cat, const Params &p)
{
	int total;
	{
		std::lock_guard<std::mutex> guard(lock);
		total = cats[static_cast<size_t>(cat)]->total;
	}
	int starts[27];
	starts[0] = 0;
	for (int k = 1; k <= 26 && !stopRequested; k++) {
		starts[k] = countBelow(session, p, static_cast<char>('A' + k - 1));
		if (starts[k] < 0) {
			return false;
		}
	}
	for (int k = 1; k <= 26; k++) {
		if (starts[k] < starts[k - 1] || starts[k] > total) {
			say("JFGATE: the server's letter offsets make no sense, counting them here instead");
			std::lock_guard<std::mutex> guard(lock);
			if (cats[static_cast<size_t>(cat)]->generation == p.generation) {
				cats[static_cast<size_t>(cat)]->lettersState = -1;
			}
			return true;
		}
	}
	std::lock_guard<std::mutex> guard(lock);
	Category &c = *cats[static_cast<size_t>(cat)];
	if (c.generation != p.generation) {
		return true;
	}
	std::memcpy(c.letters, starts, sizeof(starts));
	c.lettersState = 1;
	logLetters("server", c);
	return true;
}

// Every title is in: count the letters from what the server called them
void countLetters(int cat)
{
	std::lock_guard<std::mutex> guard(lock);
	Category &c = *cats[static_cast<size_t>(cat)];
	int first[27];
	for (int k = 0; k < 27; k++) {
		first[k] = -1;
	}
	for (int pos = 0; pos < c.total; pos++) {
		const Entry &e = c.entries[static_cast<size_t>(pos)];
		const int k = bucketOf(e.sortName.empty() ? e.name : e.sortName);
		if (first[k] < 0) {
			first[k] = pos;
		}
	}
	int next = c.total;
	for (int k = 26; k >= 0; k--) {
		if (first[k] >= 0 && first[k] < next) {
			next = first[k];
		}
		c.letters[k] = next;
	}
	c.letters[0] = 0;
	c.lettersState = 2;
	logLetters("counted", c);
}

bool fetchPoster(const JfSession &session, int cat, int pos, const Params &p)
{
	std::string id, tag, name;
	{
		std::lock_guard<std::mutex> guard(lock);
		const Category &c = *cats[static_cast<size_t>(cat)];
		if (c.generation != p.generation || static_cast<size_t>(pos) >= c.entries.size()) {
			return true;
		}
		const Entry &e = c.entries[static_cast<size_t>(pos)];
		id = e.imageId;
		tag = e.imageTag;
		name = e.name;
	}
	char auth[4096];
	authorization(session, auth, sizeof(auth));
	char path[512];
	std::snprintf(path, sizeof(path),
		"/Items/%s/Images/Primary?tag=%s&maxWidth=400&maxHeight=600&quality=85&format=Jpg", id.c_str(), tag.c_str());

	HttpBinaryResponse response;
	std::memset(&response, 0, sizeof(response));
	if (http_get_binary(jfconf::host(), jfconf::port(), path, auth, &response) != 0) {
		say("JFGATE: poster request failed for \"%s\"", name.c_str());
		return false;
	}
	if (response.status != 200 || !response.data || response.size == 0) {
		say("JFGATE: poster for \"%s\": HTTP %d, %zu bytes", name.c_str(), response.status, response.size);
		http_binary_response_free(&response);
		return false;
	}
	int w = 0, h = 0, channels = 0;
	unsigned char *pixels =
		stbi_load_from_memory(response.data, static_cast<int>(response.size), &w, &h, &channels, 4);
	http_binary_response_free(&response);
	if (!pixels) {
		say("JFGATE: poster for \"%s\" would not decode: %s", name.c_str(), stbi_failure_reason());
		return false;
	}
	std::lock_guard<std::mutex> guard(lock);
	if (cats[static_cast<size_t>(cat)]->generation != p.generation) {
		std::free(pixels);
		return true;
	}
	JfGatePoster poster;
	poster.category = cat;
	poster.position = pos;
	poster.generation = p.generation;
	poster.width = w;
	poster.height = h;
	poster.rgba = pixels;
	ready.push_back(poster);
	return true;
}

// The server's favourite flag, set for a title
void sendFavourite(const JfSession &session, const FavouriteJob &job)
{
	char auth[4096];
	authorization(session, auth, sizeof(auth));
	char path[400];
	std::snprintf(path, sizeof(path), "/Users/%s/Items/%s/UserData", session.user_id, job.id.c_str());
	char body[64];
	std::snprintf(body, sizeof(body), "{\"IsFavorite\":%s}", job.on ? "true" : "false");
	HttpResponse response;
	std::memset(&response, 0, sizeof(response));
	if (http_post_json(jfconf::host(), jfconf::port(), path, auth, body, &response) != 0) {
		say("JFGATE: favourite request failed");
		return;
	}
	say("JFGATE: favourite %s -> HTTP %d", job.on ? "on" : "off", response.status);
	const int status = response.status;
	http_response_free(&response);
	if (job.on && (status < 200 || status >= 300)) {
		// an older server: the favourites endpoint (it has no way to switch one off, so only this direction)
		std::snprintf(path, sizeof(path), "/Users/%s/FavoriteItems/%s", session.user_id, job.id.c_str());
		std::memset(&response, 0, sizeof(response));
		if (http_post_json(jfconf::host(), jfconf::port(), path, auth, "{}", &response) == 0) {
			say("JFGATE: favourite (old endpoint) -> HTTP %d", response.status);
			http_response_free(&response);
		}
	}
}


void copyText(char *to, size_t size, const std::string &from)
{
	std::snprintf(to, size, "%s", from.c_str());
}

bool containsWord(const std::string &text, const char *word)
{
	return text.find(word) != std::string::npos;
}

// The server's full record of one title
void fetchDetails(const JfSession &session, const std::string &id)
{
	char path[300];
	std::snprintf(path, sizeof(path), "/Users/%s/Items/%s", session.user_id, id.c_str());
	json_object *root = getJson(session, path);
	std::unique_ptr<JfGateDetails> d(new JfGateDetails());
	std::memset(d.get(), 0, sizeof(JfGateDetails));
	d->default_audio = -1;
	d->default_subtitle = -1;
	if (root) {
		copyText(d->kind, sizeof(d->kind), jstr(root, "Type"));
		json_object *ids = jget(root, "ProviderIds");
		copyText(d->imdb, sizeof(d->imdb), jstr(ids, "Imdb"));
		copyText(d->tmdb, sizeof(d->tmdb), jstr(ids, "Tmdb"));
		copyText(d->tvdb, sizeof(d->tvdb), jstr(ids, "Tvdb"));
		copyText(d->official, sizeof(d->official), jstr(root, "OfficialRating"));
		d->critic = static_cast<float>(jnum(root, "CriticRating"));
		d->community = static_cast<float>(jnum(root, "CommunityRating"));
		json_object *taglines = jget(root, "Taglines");
		if (taglines && json_object_is_type(taglines, json_type_array) && json_object_array_length(taglines) > 0) {
			json_object *first = json_object_array_get_idx(taglines, 0);
			if (first && json_object_is_type(first, json_type_string)) {
				copyText(d->tagline, sizeof(d->tagline), json_object_get_string(first));
			}
		}
		json_object *people = jget(root, "People");
		if (people && json_object_is_type(people, json_type_array)) {
			std::string directors, cast;
			int actors = 0, directorCount = 0;
			const size_t n = json_object_array_length(people);
			for (size_t i = 0; i < n; i++) {
				json_object *p = json_object_array_get_idx(people, i);
				const std::string type = jstr(p, "Type");
				const std::string name = jstr(p, "Name");
				if (type == "Director" && directorCount < 2) {
					directors += (directors.empty() ? "" : ", ") + name;
					directorCount++;
				} else if (type == "Actor" && actors < 6) {
					cast += (cast.empty() ? "" : ", ") + name;
					actors++;
				}
			}
			copyText(d->directors, sizeof(d->directors), directors);
			copyText(d->cast, sizeof(d->cast), cast);
		}
		json_object *sources = jget(root, "MediaSources");
		json_object *source = (sources && json_object_is_type(sources, json_type_array) && json_object_array_length(sources) > 0)
			? json_object_array_get_idx(sources, 0) : nullptr;
		if (source) {
			copyText(d->media_source, sizeof(d->media_source), jstr(source, "Id"));
			copyText(d->container, sizeof(d->container), jstr(source, "Container"));
			d->file_size = jint(source, "Size");
			d->total_bitrate = static_cast<int>(jint(source, "Bitrate"));
			if (jget(source, "DefaultAudioStreamIndex")) {
				d->default_audio = static_cast<int>(jint(source, "DefaultAudioStreamIndex"));
			}
			if (jget(source, "DefaultSubtitleStreamIndex")) {
				d->default_subtitle = static_cast<int>(jint(source, "DefaultSubtitleStreamIndex"));
			}
			json_object *streams = jget(source, "MediaStreams");
			const size_t n = streams && json_object_is_type(streams, json_type_array) ? json_object_array_length(streams) : 0;
			bool haveVideo = false;
			for (size_t i = 0; i < n; i++) {
				json_object *s = json_object_array_get_idx(streams, i);
				const std::string type = jstr(s, "Type");
				if (type == "Video" && !haveVideo) {
					haveVideo = true;
					d->width = static_cast<int>(jint(s, "Width"));
					d->height = static_cast<int>(jint(s, "Height"));
					copyText(d->video_codec, sizeof(d->video_codec), jstr(s, "Codec"));
					d->bit_depth = static_cast<int>(jint(s, "BitDepth"));
					d->video_bitrate = static_cast<int>(jint(s, "BitRate"));
					d->fps = static_cast<float>(jnum(s, "RealFrameRate") > 0.0 ? jnum(s, "RealFrameRate") : jnum(s, "AverageFrameRate"));
					const std::string rangeType = jstr(s, "VideoRangeType");
					const std::string range = jstr(s, "VideoRange");
					copyText(d->range_type, sizeof(d->range_type), rangeType);
					d->dv_profile = static_cast<int>(jint(s, "DvProfile"));
					d->dv_level = static_cast<int>(jint(s, "DvLevel"));
					d->dv_el = containsWord(rangeType, "EL") ? 1 : 0;
					const char *label = "SDR";
					if (containsWord(rangeType, "DOVI") && !containsWord(rangeType, "DOVIWithSDR")) {
						label = "Dolby Vision";
					} else if (containsWord(rangeType, "HDR10Plus")) {
						label = "HDR10+";
					} else if (rangeType == "HDR10") {
						label = "HDR10";
					} else if (containsWord(rangeType, "HLG")) {
						label = "HLG";
					} else if (range == "HDR") {
						label = "HDR10";
					}
					copyText(d->range, sizeof(d->range), label);
				} else if ((type == "Audio" || type == "Subtitle") && d->track_count < JF_GATE_TRACKS) {
					JfGateTrack &t = d->tracks[d->track_count++];
					t.index = static_cast<int>(jint(s, "Index"));
					t.kind = type == "Audio" ? 1 : 2;
					std::string title = jstr(s, "DisplayTitle");
					if (title.empty()) {
						title = jstr(s, "Title");
					}
					if (title.empty()) {
						title = jstr(s, "Language");
					}
					if (title.empty()) {
						title = type == "Audio" ? "Sound track" : "Subtitles";
					}
					copyText(t.title, sizeof(t.title), title);
					copyText(t.lang, sizeof(t.lang), jstr(s, "Language"));
					copyText(t.codec, sizeof(t.codec), jstr(s, "Codec"));
					t.channels = static_cast<int>(jint(s, "Channels"));
					t.is_default = jbool(s, "IsDefault") ? 1 : 0;
					t.forced = jbool(s, "IsForced") ? 1 : 0;
					t.external = jbool(s, "IsExternal") ? 1 : 0;
					t.text = jbool(s, "IsTextSubtitleStream") ? 1 : 0;
					if (!t.external && type == "Subtitle") {
						// PGS (Blu-ray), DVD and DVB pictures are decoded from the file itself
						std::string c = jstr(s, "Codec");
						for (char &ch : c) {
							ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
						}
						if (c == "pgssub" || c == "hdmv_pgs_subtitle" || c == "pgs" || c == "dvdsub" || c == "dvd_subtitle" ||
							c == "dvbsub" || c == "dvb_subtitle" || c == "xsub") {
							t.text = 1;
						}
					}
					t.atmos = (containsWord(title, "Atmos") || containsWord(jstr(s, "Profile"), "Atmos")) ? 1 : 0;
				}
			}
		}
		json_object_put(root);
	}
	std::lock_guard<std::mutex> guard(lock);
	DetailsRecord &record = detailsCache[id];
	if (root) {
		record.details = *d;
		record.state = 1;
		say("JFGATE: details of %s: %s %dx%d %s, %d tracks, imdb %s", id.c_str(), d->video_codec, d->width, d->height, d->range,
			d->track_count, d->imdb[0] ? d->imdb : "-");
	} else {
		record.state = 2;
	}
}


// One piece of work for the category the screen is on. Returns false when there is nothing to do.
bool step(const JfSession &session, int cat, int centre)
{
	const Params p = paramsOf(cat);
	int total;
	{
		std::lock_guard<std::mutex> guard(lock);
		total = cats[static_cast<size_t>(cat)]->total;
	}
	// 1. how many titles there are
	if (total < 0) {
		if (!loadPage(session, cat, 0, p)) {
			say("JFGATE: the first page of category %d would not load", cat);
			sleepMs(1000);
		}
		return true;
	}
	if (total == 0) {
		return false;
	}
	centre = std::clamp(centre, 0, total - 1);

	// 2. the pages around the wheel
	int page = -1;
	// 3. the nearest poster missing
	int poster = -1;
	bool needLetters = false;
	{
		std::lock_guard<std::mutex> guard(lock);
		Category &c = *cats[static_cast<size_t>(cat)];
		if (c.generation != p.generation || c.total != total) {
			return true;
		}
		for (int d = 0; d <= kRadius && page < 0; d++) {
			for (int sign = -1; sign <= 1; sign += 2) {
				const int pos = centre + sign * d;
				if (pos < 0 || pos >= c.total) {
					continue;
				}
				const size_t pg = static_cast<size_t>(pos / kPage);
				if (c.pageState[pg] == 0) {
					page = static_cast<int>(pg);
					break;
				}
			}
		}
		if (page < 0) {
			for (int d = 0; d <= kRadius && poster < 0; d++) {
				for (int sign = -1; sign <= 1; sign += 2) {
					const int pos = centre + sign * d;
					if (pos < 0 || pos >= c.total || (d == 0 && sign == 1)) {
						continue;
					}
					if (c.pageState[static_cast<size_t>(pos / kPage)] != 1 || c.posterState[static_cast<size_t>(pos)] != 0) {
						continue;
					}
					if (c.entries[static_cast<size_t>(pos)].imageTag.empty()) {
						c.posterState[static_cast<size_t>(pos)] = 2;
						continue;
					}
					c.posterState[static_cast<size_t>(pos)] = 1;
					poster = pos;
					break;
				}
			}
			needLetters = c.lettersState == 0;
		}
	}
	if (page >= 0) {
		if (!loadPage(session, cat, page, p)) {
			std::lock_guard<std::mutex> guard(lock);
			Category &c = *cats[static_cast<size_t>(cat)];
			if (c.generation == p.generation && static_cast<size_t>(page) < c.pageTries.size() &&
				++c.pageTries[static_cast<size_t>(page)] >= 3) {
				c.pageState[static_cast<size_t>(page)] = 2;
				say("JFGATE: giving up on page %d of category %d", page, cat);
			}
		}
		return true;
	}
	if (poster >= 0) {
		if (!fetchPoster(session, cat, poster, p)) {
			std::lock_guard<std::mutex> guard(lock);
			Category &c = *cats[static_cast<size_t>(cat)];
			if (c.generation == p.generation && static_cast<size_t>(poster) < c.posterState.size()) {
				c.posterState[static_cast<size_t>(poster)] = 2;
			}
		}
		return true;
	}
	// 4. the A-Z offsets
	if (needLetters) {
		if (!loadLetters(session, cat, p)) {
			std::lock_guard<std::mutex> guard(lock);
			Category &c = *cats[static_cast<size_t>(cat)];
			if (c.generation == p.generation) {
				c.lettersState = -1;
			}
		}
		return true;
	}
	// 5. the rest of the category, nearest page first
	int next = -1;
	bool allIn = false;
	int exactNeeded = 0;
	{
		std::lock_guard<std::mutex> guard(lock);
		Category &c = *cats[static_cast<size_t>(cat)];
		if (c.generation != p.generation) {
			return true;
		}
		const int centrePage = centre / kPage;
		const int pages = static_cast<int>(c.pageState.size());
		for (int d = 0; d < pages && next < 0; d++) {
			for (int sign = -1; sign <= 1; sign += 2) {
				const int pg = centrePage + sign * d;
				if (pg >= 0 && pg < pages && c.pageState[static_cast<size_t>(pg)] == 0) {
					next = pg;
					break;
				}
			}
		}
		allIn = next < 0;
		bool complete = true;
		for (size_t pg = 0; pg < c.pageState.size(); pg++) {
			if (c.pageState[pg] != 1) {
				complete = false;
			}
		}
		exactNeeded = complete && lettered(c) && c.lettersState != 2 ? 1 : 0;
	}
	if (exactNeeded) {
		countLetters(cat);
		return true;
	}
	if (!allIn) {
		if (!loadPage(session, cat, next, p)) {
			std::lock_guard<std::mutex> guard(lock);
			Category &c = *cats[static_cast<size_t>(cat)];
			if (c.generation == p.generation && static_cast<size_t>(next) < c.pageTries.size() &&
				++c.pageTries[static_cast<size_t>(next)] >= 3) {
				c.pageState[static_cast<size_t>(next)] = 2;
			}
		}
		return true;
	}
	return false;
}

int probeTotal(const JfSession &session, int kind)
{
	Params p;
	p.kind = kind;
	const std::string path = pathFor(session, p, 0, 1, 0);
	json_object *root = getJson(session, path.c_str());
	if (!root) {
		return 0;
	}
	const int n = static_cast<int>(jint(root, "TotalRecordCount"));
	json_object_put(root);
	return n;
}

// The episode after `job.episodeId`, from the series' own list of episodes: it becomes the one title of the queue category
void fetchNext(const JfSession &session, const NextJob &job)
{
	char path[700];
	std::snprintf(path, sizeof(path), "/Shows/%s/Episodes?UserId=%s&Fields=%s", job.seriesId.c_str(), session.user_id, kFields);
	json_object *root = getJson(session, path);
	json_object *array = root ? jget(root, "Items") : nullptr;
	int found = -1;
	int count = 0;
	if (array && json_object_is_type(array, json_type_array)) {
		count = static_cast<int>(json_object_array_length(array));
		for (int i = 0; i < count; i++) {
			if (jstr(json_object_array_get_idx(array, static_cast<size_t>(i)), "Id") == job.episodeId) {
				found = i;
				break;
			}
		}
	}
	if (found >= 0 && found + 1 < count && queueCat >= 0) {
		json_object *o = json_object_array_get_idx(array, static_cast<size_t>(found + 1));
		{
			std::lock_guard<std::mutex> guard(lock);
			startOver(queueCat);
			Category &q = *cats[static_cast<size_t>(queueCat)];
			q.total = 1;
			q.entries.assign(1, Entry());
			parseEntry(q.entries[0], o, kQueue);
			q.pageState.assign(1, 1);
			q.pageTries.assign(1, 0);
			q.posterState.assign(1, 2);
			q.pagesIn = 1;
			say("JFGATE: the episode after this one: \"%s\"", q.entries[0].genre.c_str());
		}
		nextIndex.store(queueCat * JF_GATE_STRIDE);
		nextState.store(2);
	} else {
		say("JFGATE: no episode after this one (%d of %d)", found + 1, count);
		nextState.store(-1);
	}
	if (root) {
		json_object_put(root);
	}
}

// Points the browse category at the title on top of the stack and starts it over. The lock is held.
void browseConfigure()
{
	Category &c = *cats[static_cast<size_t>(browseCat)];
	const BrowseStep &s = browseStack.back();
	c.id = s.id;
	c.term = s.type;
	c.name = s.name;
	startOver(browseCat);
	say("JFGATE: browsing \"%s\" (%s)", s.name.c_str(), s.type.c_str());
}

void addCategory(int kind, const char *name, const char *id)
{
	std::unique_ptr<Category> c(new Category());
	c->kind = kind;
	c->name = name;
	c->id = id ? id : "";
	c->lettersState = lettered(*c) ? 0 : -1;
	cats.push_back(std::move(c));
}

// ---- signing in: the server, then the person (password, Quick Connect, or the token kept from last time) ----
// The screen asks through the jf_gate_auth_* calls; this thread does the talking, so nothing blocks a frame.

std::atomic<int> authState{ 0 }; // 0 working, 1 needs a server, 2 needs a sign-in, 3 signed in
std::mutex authLock;             // guards the strings below
std::string authMessage, serverName, profileName, quickCode;
std::vector<std::string> publicUsers;
struct FoundServer {
	std::string name, host;
	int port{ 8096 };
};
std::vector<FoundServer> foundServers;
std::atomic<int> discoverState{ 0 }; // 0 idle, 1 searching, 2 done
std::atomic<int> quickState{ 0 };    // 0 idle, 1 waiting for approval, 2 approved, -1 failed or switched off
std::atomic<int> quickAvailable{ -1 }; // -1 unknown, 0 off on this server, 1 on
std::atomic<int> probing{ 0 };
std::string quickSecret;
struct AuthJob {
	int kind{ 0 }; // 1 server, 2 password, 3 quick connect, 4 cancel quick connect, 5 discover
	std::string a, b;
	int port{ 8096 };
};
std::deque<AuthJob> authJobs;

void setMessage(const std::string &m)
{
	std::lock_guard<std::mutex> g(authLock);
	authMessage = m;
}

bool fileExists(const char *path)
{
	struct stat st;
	return ::stat(path, &st) == 0;
}

// The console cannot resolve names: an address is four numbers
bool parseV4(const char *host, in_addr *out)
{
	return host && *host && ::inet_pton(AF_INET, host, out) == 1;
}

// True when something answers on host:port within the time (a closed or silent address must not hold the screen)
bool reachable(const char *host, int port, int ms)
{
	in_addr ip;
	if (!parseV4(host, &ip)) {
		return false;
	}
	const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		return false;
	}
	const int flags = ::fcntl(fd, F_GETFL, 0);
	::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	sockaddr_in sa;
	std::memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(static_cast<uint16_t>(port));
	sa.sin_addr = ip;
	bool ok = false;
	const int r = ::connect(fd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa));
	if (r == 0) {
		ok = true;
	} else if (errno == EINPROGRESS) {
		pollfd p;
		p.fd = fd;
		p.events = POLLOUT;
		p.revents = 0;
		if (::poll(&p, 1, ms) == 1) {
			int err = 0;
			socklen_t len = sizeof(err);
			::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
			ok = err == 0;
		}
	}
	::close(fd);
	return ok;
}

// The broadcast every Jellyfin app sends: servers on the network answer with their name and address
void discover()
{
	{
		std::lock_guard<std::mutex> g(authLock);
		foundServers.clear();
	}
	discoverState = 1;
	const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		say("JFGATE: discovery: no socket");
		discoverState = 2;
		return;
	}
	int yes = 1;
	::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
	sockaddr_in to;
	std::memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = htons(7359);
	to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	const char *ask = "who is JellyfinServer?";
	for (int round = 0; round < 2; round++) {
		::sendto(fd, ask, std::strlen(ask), 0, reinterpret_cast<sockaddr *>(&to), sizeof(to));
		for (int waited = 0; waited < 1500 && !stopRequested; ) {
			pollfd p;
			p.fd = fd;
			p.events = POLLIN;
			p.revents = 0;
			if (::poll(&p, 1, 250) != 1) {
				waited += 250;
				continue;
			}
			char buf[1024];
			sockaddr_in from;
			socklen_t fl = sizeof(from);
			const ssize_t n = ::recvfrom(fd, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr *>(&from), &fl);
			if (n <= 0) {
				continue;
			}
			buf[n] = 0;
			json_object *o = json_tokener_parse(buf);
			if (!o) {
				continue;
			}
			FoundServer s;
			s.name = jstr(o, "Name");
			// "Address" is a URL: http://192.168.1.10:8096 . Names cannot be resolved, so the sender's address is used.
			char ipText[64] = "";
			::inet_ntop(AF_INET, &from.sin_addr, ipText, sizeof(ipText));
			s.host = ipText;
			const std::string address = jstr(o, "Address");
			const size_t colon = address.rfind(':');
			if (colon != std::string::npos && address.find("//") != std::string::npos && colon > address.find("//") + 1) {
				const int p2 = std::atoi(address.c_str() + colon + 1);
				if (p2 > 0 && p2 < 65536) {
					s.port = p2;
				}
			}
			json_object_put(o);
			std::lock_guard<std::mutex> g(authLock);
			bool dup = false;
			for (const auto &f : foundServers) {
				dup = dup || (f.host == s.host && f.port == s.port);
			}
			if (!dup) {
				say("JFGATE: found server %s at %s:%d", s.name.c_str(), s.host.c_str(), s.port);
				foundServers.push_back(s);
			}
			waited += 50;
		}
	}
	::close(fd);
	discoverState = 2;
}

// The header before there is a token
void anonymousAuth(char *out, size_t size)
{
	std::snprintf(out, size,
		"Authorization: MediaBrowser Client=\"%s\", Device=\"%s\", DeviceId=\"%s\", Version=\"%s\"\r\n",
		JF_CLIENT_NAME, JF_DEVICE_NAME, JF_DEVICE_ID, JF_CLIENT_VERSION);
}

// What the server answers to a sign-in: token, user, name
bool readSignIn(const HttpResponse &r, JfSession *out, std::string *name, std::string *why)
{
	if (r.status < 200 || r.status >= 300) {
		*why = r.status == 401 ? "Wrong user name or password" : "The server said no (HTTP " + std::to_string(r.status) + ")";
		return false;
	}
	json_object *root = r.body ? json_tokener_parse(r.body) : nullptr;
	if (!root) {
		*why = "The server's answer was unreadable";
		return false;
	}
	std::memset(out, 0, sizeof(*out));
	std::snprintf(out->access_token, sizeof(out->access_token), "%s", jstr(root, "AccessToken").c_str());
	std::snprintf(out->server_id, sizeof(out->server_id), "%s", jstr(root, "ServerId").c_str());
	json_object *user = jget(root, "User");
	if (user) {
		std::snprintf(out->user_id, sizeof(out->user_id), "%s", jstr(user, "Id").c_str());
		*name = jstr(user, "Name");
	}
	json_object_put(root);
	if (!out->access_token[0] || !out->user_id[0]) {
		*why = "The server sent no session";
		return false;
	}
	return true;
}

bool signInPassword(const std::string &user, const std::string &pass, JfSession *out, std::string *name, std::string *why)
{
	json_object *body = json_object_new_object();
	json_object_object_add(body, "Username", json_object_new_string(user.c_str()));
	json_object_object_add(body, "Pw", json_object_new_string(pass.c_str()));
	const std::string text = json_object_to_json_string(body);
	json_object_put(body);
	char auth[1024];
	anonymousAuth(auth, sizeof(auth));
	HttpResponse r;
	std::memset(&r, 0, sizeof(r));
	if (http_post_json(jfconf::host(), jfconf::port(), "/Users/AuthenticateByName", auth, text.c_str(), &r) != 0) {
		*why = "Could not reach the server";
		return false;
	}
	const bool ok = readSignIn(r, out, name, why);
	http_response_free(&r);
	return ok;
}

bool signInQuick(const std::string &secret, JfSession *out, std::string *name, std::string *why)
{
	json_object *body = json_object_new_object();
	json_object_object_add(body, "Secret", json_object_new_string(secret.c_str()));
	const std::string text = json_object_to_json_string(body);
	json_object_put(body);
	char auth[1024];
	anonymousAuth(auth, sizeof(auth));
	HttpResponse r;
	std::memset(&r, 0, sizeof(r));
	if (http_post_json(jfconf::host(), jfconf::port(), "/Users/AuthenticateWithQuickConnect", auth, text.c_str(), &r) != 0) {
		*why = "Could not reach the server";
		return false;
	}
	const bool ok = readSignIn(r, out, name, why);
	http_response_free(&r);
	return ok;
}

// The token kept from last time: still good?
bool signInToken(const std::string &token, JfSession *out, std::string *name)
{
	std::memset(out, 0, sizeof(*out));
	std::snprintf(out->access_token, sizeof(out->access_token), "%s", token.c_str());
	json_object *me = getJson(*out, "/Users/Me");
	if (!me) {
		return false;
	}
	std::snprintf(out->user_id, sizeof(out->user_id), "%s", jstr(me, "Id").c_str());
	std::snprintf(out->server_id, sizeof(out->server_id), "%s", jstr(me, "ServerId").c_str());
	*name = jstr(me, "Name");
	json_object_put(me);
	return out->user_id[0] != 0;
}

void rememberSession(const JfSession &s, const std::string &name)
{
	jfconf::writeFile("/data/jf-server", jfconf::serverText());
	jfconf::writeFile("/data/jf-token", s.access_token);
	jfconf::writeFile("/data/jf-userid", s.user_id);
	jfconf::writeFile("/data/jf-user", name);
	std::remove("/data/jf-setup");
	std::lock_guard<std::mutex> g(authLock);
	profileName = name;
}

// Is a server there, and what does it offer? Fills the server's name, its public profiles and Quick Connect.
bool probeServer(const std::string &host, int port, std::string *why)
{
	// host may be an address, a name, or https://name: a name or https goes through the local tunnel (jf_tls.cpp)
	std::string where;
	int wherePort = 0;
	jfconf::route(host, port, &where, &wherePort);
	if (!reachable(where.c_str(), wherePort, 3000)) {
		*why = "Nothing answered at " + jfconf::serverTextOf(host, port);
		return false;
	}
	jfconf::setServer(host, port);
	JfSession none;
	std::memset(&none, 0, sizeof(none));
	json_object *info = getJson(none, "/System/Info/Public");
	if (!info) {
		{
			char problem[300];
			jf_tls_error(problem, sizeof(problem));
			*why = problem[0] ? std::string(problem) : std::string("That is not a Jellyfin server");
		}
		return false;
	}
	std::string name = jstr(info, "ServerName");
	json_object_put(info);
	std::vector<std::string> users;
	if (json_object *list = getJson(none, "/Users/Public")) {
		const int n = json_object_is_type(list, json_type_array) ? static_cast<int>(json_object_array_length(list)) : 0;
		for (int i = 0; i < n && i < 24; i++) {
			const std::string u = jstr(json_object_array_get_idx(list, i), "Name");
			if (!u.empty()) {
				users.push_back(u);
			}
		}
		json_object_put(list);
	}
	int quick = 0;
	{
		char auth[1024];
		anonymousAuth(auth, sizeof(auth));
		HttpResponse r;
		std::memset(&r, 0, sizeof(r));
		if (http_get(jfconf::host(), jfconf::port(), "/QuickConnect/Enabled", auth, &r) == 0) {
			quick = r.status == 200 && r.body && std::strstr(r.body, "true") ? 1 : 0;
			http_response_free(&r);
		}
	}
	std::lock_guard<std::mutex> g(authLock);
	serverName = name;
	publicUsers = users;
	quickAvailable = quick;
	return true;
}

// Waits for the person to be signed in: first from what was kept, then from the screen.
bool acquireSession(JfSession *out)
{
	std::string name, why;
	const bool forced = fileExists("/data/jf-setup");
	if (!forced && jfconf::configured()) {
		const std::string token = jfconf::readFile("/data/jf-token");
		if (!token.empty() && reachable(jfconf::host(), jfconf::port(), 3000) && signInToken(token, out, &name)) {
			say("JFGATE: kept session accepted for %s", name.c_str());
			rememberSession(*out, name);
			return true;
		}
		if (jfconf::user()[0] && jfconf::pass()[0] && reachable(jfconf::host(), jfconf::port(), 3000)) {
			if (signInPassword(jfconf::user(), jfconf::pass(), out, &name, &why)) {
				rememberSession(*out, name);
				return true;
			}
			setMessage(why);
		}
	}
	// Needs the screen
	bool haveServer = false;
	if (jfconf::configured()) {
		haveServer = probeServer(jfconf::host(), jfconf::port(), &why);
		if (!haveServer) {
			setMessage(why);
		}
	}
	authState = haveServer ? 2 : 1;
	say("JFGATE: waiting for the person to sign in (%s)", haveServer ? "server known" : "no server");
	int quickWaited = 0;
	while (!stopRequested) {
		AuthJob job;
		bool have = false;
		{
			std::lock_guard<std::mutex> g(authLock);
			if (!authJobs.empty()) {
				job = authJobs.front();
				authJobs.pop_front();
				have = true;
			}
		}
		if (have) {
			switch (job.kind) {
			case 1: {
				probing = 1;
				setMessage("");
				if (probeServer(job.a, job.port, &why)) {
					haveServer = true;
					authState = 2;
				} else {
					setMessage(why);
				}
				probing = 0;
				break;
			}
			case 2: {
				probing = 1;
				setMessage("");
				if (signInPassword(job.a, job.b, out, &name, &why)) {
					rememberSession(*out, name);
					probing = 0;
					return true;
				}
				setMessage(why);
				probing = 0;
				break;
			}
			case 3: {
				quickState = 0;
				{
					std::lock_guard<std::mutex> g(authLock);
					quickCode.clear();
				}
				char auth[1024];
				anonymousAuth(auth, sizeof(auth));
				HttpResponse r;
				std::memset(&r, 0, sizeof(r));
				json_object *o = nullptr;
				if (http_post_json(jfconf::host(), jfconf::port(), "/QuickConnect/Initiate", auth, "{}", &r) == 0) {
					if (r.status == 200 && r.body) {
						o = json_tokener_parse(r.body);
					}
					http_response_free(&r);
				}
				if (o && !jstr(o, "Code").empty()) {
					quickSecret = jstr(o, "Secret");
					{
						std::lock_guard<std::mutex> g(authLock);
						quickCode = jstr(o, "Code");
					}
					quickState = 1;
					quickWaited = 0;
					say("JFGATE: Quick Connect code %s", jstr(o, "Code").c_str());
				} else {
					quickState = -1;
					setMessage("Quick Connect is switched off on this server");
				}
				if (o) {
					json_object_put(o);
				}
				break;
			}
			case 4:
				quickState = 0;
				break;
			case 5:
				discover();
				break;
			}
			continue;
		}
		if (quickState == 1) {
			sleepMs(100);
			quickWaited += 100;
			if (quickWaited % 2000 == 0) {
				char auth[1024];
				anonymousAuth(auth, sizeof(auth));
				const std::string path = "/QuickConnect/Connect?secret=" + quickSecret;
				HttpResponse r;
				std::memset(&r, 0, sizeof(r));
				if (http_get(jfconf::host(), jfconf::port(), path.c_str(), auth, &r) == 0) {
					json_object *o = r.status == 200 && r.body ? json_tokener_parse(r.body) : nullptr;
					http_response_free(&r);
					if (o && jbool(o, "Authenticated")) {
						json_object_put(o);
						if (signInQuick(quickSecret, out, &name, &why)) {
							rememberSession(*out, name);
							quickState = 2;
							return true;
						}
						setMessage(why);
						quickState = -1;
					} else if (o) {
						json_object_put(o);
					} else {
						quickState = -1;
						setMessage("The code expired");
					}
				}
			}
			if (quickWaited > 300000) {
				quickState = -1;
				setMessage("The code expired");
			}
			continue;
		}
		sleepMs(50);
	}
	return false;
}

// The profile picture (Jellyfin's own "Primary" image of the user); none is fine, the screen draws an initial
JfGatePoster avatarPicture;
bool avatarWaiting = false;

void fetchAvatar(const JfSession &session)
{
	char auth[4096];
	authorization(session, auth, sizeof(auth));
	char path[400];
	std::snprintf(path, sizeof(path), "/Users/%s/Images/Primary?maxHeight=160&maxWidth=160&quality=90&format=Jpg", session.user_id);
	HttpBinaryResponse response;
	std::memset(&response, 0, sizeof(response));
	if (http_get_binary(jfconf::host(), jfconf::port(), path, auth, &response) != 0) {
		return;
	}
	if (response.status != 200 || !response.data || response.size == 0) {
		say("JFGATE: no profile picture (HTTP %d)", response.status);
		http_binary_response_free(&response);
		return;
	}
	int w = 0, h = 0, channels = 0;
	unsigned char *pixels = stbi_load_from_memory(response.data, static_cast<int>(response.size), &w, &h, &channels, 4);
	http_binary_response_free(&response);
	if (!pixels) {
		return;
	}
	std::lock_guard<std::mutex> guard(lock);
	if (avatarWaiting) {
		std::free(avatarPicture.rgba);
	}
	std::memset(&avatarPicture, 0, sizeof(avatarPicture));
	avatarPicture.width = w;
	avatarPicture.height = h;
	avatarPicture.rgba = pixels;
	avatarWaiting = true;
	say("JFGATE: profile picture %dx%d", w, h);
}

void run()
{
	say("JFGATE: starting");
	JfSession session;
	std::memset(&session, 0, sizeof(session));
	if (!acquireSession(&session)) {
		finished = 1;
		return;
	}
	jf_init(jfconf::host(), jfconf::port());
	authState = 3;
	fetchAvatar(session);
	say("JFGATE: logged in as user %s at %s", session.user_id, jfconf::serverText().c_str());
	gSession = session;
	sessionReady.store(true, std::memory_order_release);

	JfItem *libraries = static_cast<JfItem *>(std::calloc(64, sizeof(JfItem)));
	int count = 0;
	if (!libraries || jf_get_libraries(&session, libraries, 64, &count) != 0) {
		say("JFGATE: library list FAILED: %s", jf_last_error());
		std::free(libraries);
		finished = 2;
		return;
	}
	say("JFGATE: %d libraries", count);
	if (count == 0) {
		std::free(libraries);
		finished = 2;
		return;
	}
	// Does the user have something to continue? Then the wheel opens there.
	const int resumeTotal = probeTotal(session, kResume);
	const int nextTotal = probeTotal(session, kNextUp);
	say("JFGATE: continue watching %d, next up %d", resumeTotal, nextTotal);
	int total;
	{
		std::lock_guard<std::mutex> guard(lock);
		addCategory(kResume, "Continue Watching", nullptr);
		addCategory(kNextUp, "Next Up", nullptr);
		for (int i = 0; i < count; i++) {
			say("JFGATE: library %d: %s (%s)", i, libraries[i].name, libraries[i].type);
			addCategory(kLibrary, libraries[i].name, libraries[i].id);
		}
		addCategory(kRecent, "Recently Added", nullptr);
		addCategory(kFavourites, "Favourites", nullptr);
		// two lists with no tab: what a series, season or collection holds, and the episode that plays next
		addCategory(kBrowse, "Browse", nullptr);
		cats.back()->total = 0;
		browseCat = static_cast<int>(cats.size()) - 1;
		addCategory(kQueue, "Up next", nullptr);
		cats.back()->total = 0;
		queueCat = static_cast<int>(cats.size()) - 1;
		addCategory(kSearch, "Search", nullptr);
		cats.back()->total = 0; // nothing searched for yet
		total = static_cast<int>(cats.size());
	}
	std::free(libraries);
	startCategory.store(resumeTotal > 0 ? 0 : nextTotal > 0 ? 1 : 2);
	wantCategory.store(startCategory.load());
	categoryCount.store(total, std::memory_order_release);

	while (!stopRequested) {
		// what the player and the screen are waiting for comes first, even while a film plays
		FavouriteJob job;
		bool haveJob = false;
		std::string detailsId;
		NextJob nextJob;
		bool haveNext = false;
		{
			std::lock_guard<std::mutex> guard(lock);
			if (!favouriteJobs.empty()) {
				job = favouriteJobs.front();
				favouriteJobs.pop_front();
				haveJob = true;
			} else if (!detailsJobs.empty()) {
				detailsId = detailsJobs.front();
				detailsJobs.pop_front();
			} else if (!nextJobs.empty()) {
				nextJob = nextJobs.front();
				nextJobs.pop_front();
				haveNext = true;
			}
		}
		if (haveJob) {
			sendFavourite(session, job);
			continue;
		}
		if (!detailsId.empty()) {
			fetchDetails(session, detailsId);
			continue;
		}
		if (haveNext) {
			fetchNext(session, nextJob);
			continue;
		}
		if (paused) {
			sleepMs(100);
			continue;
		}
		const int cat = std::clamp(wantCategory.load(), 0, total - 1);
		if (!step(session, cat, wantCentre.load())) {
			sleepMs(30);
		}
	}
	say("JFGATE: done");
	finished = 1;
}

bool locate(int gateIndex, int *cat, int *pos)
{
	*cat = gateIndex / JF_GATE_STRIDE;
	*pos = gateIndex % JF_GATE_STRIDE;
	return gateIndex >= 0 && *cat < categoryCount.load(std::memory_order_acquire);
}

bool validCategory(int category)
{
	return category >= 0 && category < categoryCount.load(std::memory_order_acquire);
}
} // namespace

extern "C" void jf_gate_start(void)
{
	if (started.exchange(true)) {
		return;
	}
	worker = std::thread(run);
}

extern "C" void jf_gate_stop(void)
{
	if (!started) {
		return;
	}
	stopRequested = true;
	// The client sets no socket timeouts, so a server that does not answer can hold the thread for a
	// long time: wait a moment, and never block the title's exit for long.
	for (int i = 0; i < 60 && !finished; i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	if (finished) {
		worker.join();
	} else {
		say("JFGATE: still busy at exit, detaching");
		worker.detach();
	}
	started = false;
}

extern "C" int jf_gate_category_count(void)
{
	return categoryCount.load(std::memory_order_acquire);
}

extern "C" const char *jf_gate_category_name(int category)
{
	if (!validCategory(category)) {
		return "";
	}
	return cats[static_cast<size_t>(category)]->name.c_str();
}

extern "C" int jf_gate_category_role(int category)
{
	if (!validCategory(category)) {
		return JF_GATE_LIBRARY;
	}
	const int kind = cats[static_cast<size_t>(category)]->kind;
	return kind == kLibrary ? JF_GATE_LIBRARY : kind == kSearch ? JF_GATE_SEARCH : JF_GATE_HOME;
}

extern "C" int jf_gate_category_kind(int category)
{
	return validCategory(category) ? cats[static_cast<size_t>(category)]->kind : 0;
}

extern "C" int jf_gate_start_category(void)
{
	return startCategory.load();
}

extern "C" int jf_gate_category_total(int category)
{
	if (!validCategory(category)) {
		return -1;
	}
	std::lock_guard<std::mutex> guard(lock);
	return cats[static_cast<size_t>(category)]->total;
}

extern "C" int jf_gate_generation(int category)
{
	if (!validCategory(category)) {
		return 0;
	}
	std::lock_guard<std::mutex> guard(lock);
	return cats[static_cast<size_t>(category)]->generation;
}

extern "C" int jf_gate_category_lettered(int category)
{
	if (!validCategory(category)) {
		return 0;
	}
	std::lock_guard<std::mutex> guard(lock);
	return lettered(*cats[static_cast<size_t>(category)]) ? 1 : 0;
}

extern "C" int jf_gate_category_sortable(int category)
{
	if (!validCategory(category)) {
		return 0;
	}
	std::lock_guard<std::mutex> guard(lock);
	return sortable(*cats[static_cast<size_t>(category)]) ? 1 : 0;
}

extern "C" int jf_gate_sort(int category)
{
	if (!validCategory(category)) {
		return 0;
	}
	std::lock_guard<std::mutex> guard(lock);
	return cats[static_cast<size_t>(category)]->sort;
}

extern "C" int jf_gate_filter(int category)
{
	if (!validCategory(category)) {
		return 0;
	}
	std::lock_guard<std::mutex> guard(lock);
	return cats[static_cast<size_t>(category)]->filter;
}

extern "C" const char *jf_gate_sort_name(int sort)
{
	return kSortLabel[std::clamp(sort, 0, JF_GATE_SORTS - 1)];
}

extern "C" const char *jf_gate_filter_name(int filter)
{
	return kFilterLabel[std::clamp(filter, 0, JF_GATE_FILTERS - 1)];
}

extern "C" void jf_gate_set_view(int category, int sort, int filter)
{
	if (!validCategory(category)) {
		return;
	}
	std::lock_guard<std::mutex> guard(lock);
	Category &c = *cats[static_cast<size_t>(category)];
	if (!sortable(c)) {
		return;
	}
	sort = std::clamp(sort, 0, JF_GATE_SORTS - 1);
	filter = c.kind == kFavourites ? 0 : std::clamp(filter, 0, JF_GATE_FILTERS - 1);
	if (sort == c.sort && filter == c.filter) {
		return;
	}
	c.sort = sort;
	c.filter = filter;
	say("JFGATE: \"%s\" sorted by %s, showing %s", c.name.c_str(), kSortLabel[sort], kFilterLabel[filter]);
	startOver(category);
}

extern "C" void jf_gate_refresh(int category)
{
	if (!validCategory(category)) {
		return;
	}
	std::lock_guard<std::mutex> guard(lock);
	Category &c = *cats[static_cast<size_t>(category)];
	if (c.kind == kSearch) {
		return;
	}
	startOver(category);
}

extern "C" void jf_gate_search(const char *term)
{
	const int count = categoryCount.load(std::memory_order_acquire);
	if (count == 0) {
		return;
	}
	const int category = count - 1;
	std::lock_guard<std::mutex> guard(lock);
	Category &c = *cats[static_cast<size_t>(category)];
	const std::string text = term ? term : "";
	if (text == c.term && c.generation > 0) {
		return;
	}
	c.term = text;
	startOver(category);
	if (text.empty()) {
		c.total = 0; // nothing to look for
		c.entries.clear();
		c.pageState.clear();
	}
	say("JFGATE: search \"%s\"", text.c_str());
}

extern "C" void jf_gate_set_favorite(int gateIndex, int on)
{
	int cat, pos;
	if (!locate(gateIndex, &cat, &pos)) {
		return;
	}
	std::lock_guard<std::mutex> guard(lock);
	Category &c = *cats[static_cast<size_t>(cat)];
	if (c.total < 0 || static_cast<size_t>(pos) >= c.entries.size() || c.entries[static_cast<size_t>(pos)].id.empty()) {
		return;
	}
	Entry &e = c.entries[static_cast<size_t>(pos)];
	e.favorite = on != 0;
	favouriteJobs.push_back(FavouriteJob{ e.id, on != 0 });
}

extern "C" void jf_gate_want(int category, int centre)
{
	wantCategory.store(category);
	wantCentre.store(centre);
}

extern "C" void jf_gate_pause(int value)
{
	paused.store(value != 0);
}

extern "C" int jf_gate_page_loaded(int category, int page)
{
	if (!validCategory(category) || page < 0) {
		return 0;
	}
	std::lock_guard<std::mutex> guard(lock);
	const Category &c = *cats[static_cast<size_t>(category)];
	return static_cast<size_t>(page) < c.pageState.size() && c.pageState[static_cast<size_t>(page)] == 1 ? 1 : 0;
}

extern "C" int jf_gate_item_meta(int gateIndex, JfGateMeta *out)
{
	int cat, pos;
	if (!locate(gateIndex, &cat, &pos)) {
		return 0;
	}
	std::lock_guard<std::mutex> guard(lock);
	const Category &c = *cats[static_cast<size_t>(cat)];
	if (c.total < 0 || pos >= c.total || static_cast<size_t>(pos / kPage) >= c.pageState.size() ||
		c.pageState[static_cast<size_t>(pos / kPage)] != 1) {
		return 0;
	}
	const Entry &e = c.entries[static_cast<size_t>(pos)];
	out->name = e.name.c_str();
	out->kind = e.kind.c_str();
	out->overview = e.overview.c_str();
	out->runtime_ticks = e.runtime;
	out->position_ticks = e.position;
	out->genre = e.genre.c_str();
	out->sort_name = e.sortName.c_str();
	out->year = e.year;
	out->rating = e.rating;
	out->favorite = e.favorite ? 1 : 0;
	out->has_poster = e.imageTag.empty() ? 0 : 1;
	out->watched = e.played ? 1 : 0;
	out->added_days = e.addedDays;
	out->child_count = e.childCount;
	return 1;
}

extern "C" int jf_gate_details_request(int gateIndex)
{
	int cat, pos;
	if (!locate(gateIndex, &cat, &pos)) {
		return -1;
	}
	std::lock_guard<std::mutex> guard(lock);
	const Category &c = *cats[static_cast<size_t>(cat)];
	if (c.total < 0 || static_cast<size_t>(pos) >= c.entries.size() || c.entries[static_cast<size_t>(pos)].id.empty()) {
		return 0;
	}
	const std::string &id = c.entries[static_cast<size_t>(pos)].id;
	auto found = detailsCache.find(id);
	if (found != detailsCache.end()) {
		return found->second.state == 1 ? 1 : found->second.state == 2 ? -1 : 0;
	}
	detailsCache[id] = DetailsRecord();
	detailsOrder.push_back(id);
	detailsJobs.push_back(id);
	// keep the newest 120: the oldest finished ones go
	while (detailsOrder.size() > 120) {
		const std::string old = detailsOrder.front();
		detailsOrder.pop_front();
		auto it = detailsCache.find(old);
		if (it != detailsCache.end() && it->second.state != 0) {
			detailsCache.erase(it);
		}
	}
	return 0;
}

extern "C" int jf_gate_details_get(int gateIndex, JfGateDetails *out)
{
	int cat, pos;
	if (!locate(gateIndex, &cat, &pos)) {
		return -1;
	}
	std::lock_guard<std::mutex> guard(lock);
	const Category &c = *cats[static_cast<size_t>(cat)];
	if (c.total < 0 || static_cast<size_t>(pos) >= c.entries.size()) {
		return 0;
	}
	auto found = detailsCache.find(c.entries[static_cast<size_t>(pos)].id);
	if (found == detailsCache.end()) {
		return 0;
	}
	if (found->second.state == 1) {
		*out = found->second.details;
		return 1;
	}
	return found->second.state == 2 ? -1 : 0;
}

extern "C" int jf_gate_letters(int category, int *starts)
{
	if (!validCategory(category)) {
		return 0;
	}
	std::lock_guard<std::mutex> guard(lock);
	const Category &c = *cats[static_cast<size_t>(category)];
	if (c.lettersState != 1 && c.lettersState != 2) {
		return 0;
	}
	std::memcpy(starts, c.letters, sizeof(c.letters));
	return c.lettersState;
}

extern "C" int jf_gate_take_poster(JfGatePoster *out)
{
	std::lock_guard<std::mutex> guard(lock);
	if (ready.empty()) {
		return 0;
	}
	*out = ready.front();
	ready.pop_front();
	return 1;
}

extern "C" void jf_gate_poster_released(int category, int position)
{
	if (!validCategory(category)) {
		return;
	}
	std::lock_guard<std::mutex> guard(lock);
	Category &c = *cats[static_cast<size_t>(category)];
	if (position >= 0 && static_cast<size_t>(position) < c.posterState.size() && c.posterState[static_cast<size_t>(position)] == 1) {
		c.posterState[static_cast<size_t>(position)] = 0;
	}
}

// ---- signing in, for the screen ----

namespace {
void queueAuth(const AuthJob &job)
{
	std::lock_guard<std::mutex> g(authLock);
	authJobs.push_back(job);
}
void copyOut(const std::string &s, char *out, int size)
{
	if (!out || size <= 0) {
		return;
	}
	std::snprintf(out, static_cast<size_t>(size), "%s", s.c_str());
}
} // namespace

// ---- sign out / switch server ----
// Stops the worker, forgets the session (and the server too when asked), empties everything the worker
// built, and starts a worker again: it waits for a sign-in as on a first run. 1 when done, 0 if the worker
// was too busy to stop (nothing changed; ask again).
extern "C" int jf_gate_signout(int forget_server)
{
	if (!started) {
		return 0;
	}
	stopRequested = true;
	for (int i = 0; i < 160 && !finished; i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	if (!finished) {
		stopRequested = false;
		return 0;
	}
	worker.join();

	std::remove("/data/jf-token");
	std::remove("/data/jf-userid");
	jfconf::writeFile("/data/jf-user", "");
	jfconf::writeFile("/data/jf-setup", "1"); // the build's own login (config.h) must not sign straight back in
	if (forget_server) {
		std::remove("/data/jf-server");
		jfconf::setServer("", 8096);
	}
	{
		std::lock_guard<std::mutex> guard(lock);
		for (JfGatePoster &p : ready) {
			std::free(p.rgba);
		}
		ready.clear();
		cats.clear();
		favouriteJobs.clear();
		detailsCache.clear();
		detailsOrder.clear();
		detailsJobs.clear();
		nextJobs.clear();
		browseStack.clear();
		if (avatarWaiting) {
			std::free(avatarPicture.rgba);
			avatarWaiting = false;
		}
	}
	{
		std::lock_guard<std::mutex> guard(authLock);
		authJobs.clear();
		foundServers.clear();
		publicUsers.clear();
		authMessage.clear();
		profileName.clear();
		quickCode.clear();
		quickSecret.clear();
		if (forget_server) {
			serverName.clear();
		}
	}
	categoryCount = 0;
	startCategory = 0;
	wantCategory = 0;
	wantCentre = 3;
	paused = false;
	nextState = 0;
	nextIndex = -1;
	browseCat = -1;
	queueCat = -1;
	sessionReady = false;
	std::memset(&gSession, 0, sizeof(gSession));
	authState = 0;
	discoverState = 0;
	quickState = 0;
	quickAvailable = -1;
	probing = 0;
	finished = 0;
	stopRequested = false;
	say("JFGATE: signed out%s", forget_server ? " and forgot the server" : "");
	worker = std::thread(run);
	return 1;
}

extern "C" int jf_gate_take_avatar(JfGatePoster *out)
{
	std::lock_guard<std::mutex> guard(lock);
	if (!avatarWaiting) {
		return 0;
	}
	*out = avatarPicture;
	avatarWaiting = false;
	std::memset(&avatarPicture, 0, sizeof(avatarPicture));
	return 1;
}

extern "C" int jf_gate_decode_image(const unsigned char *data, int size, int *w, int *h, unsigned char **rgba)
{
	int channels = 0;
	unsigned char *pixels = stbi_load_from_memory(data, size, w, h, &channels, 4);
	if (!pixels) {
		return 0;
	}
	*rgba = pixels;
	return 1;
}

extern "C" int jf_gate_auth_state(void)
{
	return authState.load();
}

extern "C" int jf_gate_auth_busy(void)
{
	return probing.load();
}

extern "C" void jf_gate_auth_text(int which, char *out, int size)
{
	std::lock_guard<std::mutex> g(authLock);
	copyOut(which == 0 ? authMessage : which == 1 ? serverName : which == 2 ? profileName : quickCode, out, size);
}

extern "C" int jf_gate_auth_users(int index, char *out, int size)
{
	std::lock_guard<std::mutex> g(authLock);
	if (index >= 0 && index < static_cast<int>(publicUsers.size())) {
		copyOut(publicUsers[static_cast<size_t>(index)], out, size);
	}
	return static_cast<int>(publicUsers.size());
}

extern "C" int jf_gate_auth_found(int index, char *name, int nameSize, char *host, int hostSize, int *port)
{
	std::lock_guard<std::mutex> g(authLock);
	if (index >= 0 && index < static_cast<int>(foundServers.size())) {
		const FoundServer &f = foundServers[static_cast<size_t>(index)];
		copyOut(f.name, name, nameSize);
		copyOut(f.host, host, hostSize);
		if (port) {
			*port = f.port;
		}
	}
	return static_cast<int>(foundServers.size());
}

extern "C" void jf_gate_auth_discover(void)
{
	if (discoverState.load() == 1) {
		return;
	}
	discoverState = 1;
	AuthJob j;
	j.kind = 5;
	queueAuth(j);
}

extern "C" int jf_gate_auth_discover_state(void)
{
	return discoverState.load();
}

extern "C" void jf_gate_auth_server(const char *host, int port)
{
	AuthJob j;
	j.kind = 1;
	j.a = host ? host : "";
	j.port = port;
	probing = 1;
	queueAuth(j);
}

extern "C" void jf_gate_auth_password(const char *user, const char *pass)
{
	AuthJob j;
	j.kind = 2;
	j.a = user ? user : "";
	j.b = pass ? pass : "";
	probing = 1;
	queueAuth(j);
}

extern "C" void jf_gate_auth_quick_start(void)
{
	AuthJob j;
	j.kind = 3;
	quickState = 0;
	queueAuth(j);
}

extern "C" void jf_gate_auth_quick_cancel(void)
{
	AuthJob j;
	j.kind = 4;
	queueAuth(j);
}

extern "C" int jf_gate_auth_quick_state(void)
{
	return quickState.load();
}

extern "C" int jf_gate_auth_quick_available(void)
{
	return quickAvailable.load();
}

extern "C" int jf_gate_finished(void)
{
	return finished.load();
}

// ---- for the player ----

bool jf_gate_session(JfSession *out)
{
	if (!sessionReady.load(std::memory_order_acquire)) {
		return false;
	}
	*out = gSession;
	return true;
}

const char *jf_gate_item_id(int index)
{
	int cat, pos;
	if (!locate(index, &cat, &pos)) {
		return "";
	}
	std::lock_guard<std::mutex> guard(lock);
	const Category &c = *cats[static_cast<size_t>(cat)];
	if (c.total < 0 || pos >= c.total || static_cast<size_t>(pos) >= c.entries.size()) {
		return "";
	}
	return c.entries[static_cast<size_t>(pos)].id.c_str();
}

std::string jf_gate_backdrop_id(int index)
{
	int cat, pos;
	if (!locate(index, &cat, &pos)) {
		return "";
	}
	std::lock_guard<std::mutex> guard(lock);
	const Category &c = *cats[static_cast<size_t>(cat)];
	if (c.total < 0 || pos >= c.total || static_cast<size_t>(pos) >= c.entries.size()) {
		return "";
	}
	const Entry &e = c.entries[static_cast<size_t>(pos)];
	return e.backdropId.empty() ? e.id : e.backdropId;
}

// ---- inside a title, and what plays next ----

extern "C" int jf_gate_browse_open(int gateIndex)
{
	int cat, pos;
	if (!locate(gateIndex, &cat, &pos) || browseCat < 0) {
		return -1;
	}
	std::lock_guard<std::mutex> guard(lock);
	const Category &src = *cats[static_cast<size_t>(cat)];
	if (src.total < 0 || static_cast<size_t>(pos) >= src.entries.size() || src.entries[static_cast<size_t>(pos)].id.empty()) {
		return -1;
	}
	const Entry &e = src.entries[static_cast<size_t>(pos)];
	BrowseStep step;
	step.id = e.id;
	step.type = e.kind;
	step.name = e.parentName.empty() ? e.name : e.parentName + ": " + e.name;
	browseStack.push_back(step);
	browseConfigure();
	return browseCat;
}

extern "C" int jf_gate_browse_back(void)
{
	std::lock_guard<std::mutex> guard(lock);
	if (browseCat < 0 || browseStack.empty()) {
		return 0;
	}
	browseStack.pop_back();
	if (!browseStack.empty()) {
		browseConfigure();
		return 1;
	}
	// back out of the last level: the list is emptied, the screen returns to the wheel it came from
	Category &c = *cats[static_cast<size_t>(browseCat)];
	startOver(browseCat);
	c.total = 0;
	c.entries.clear();
	c.pageState.clear();
	c.name = "Browse";
	return 0;
}

extern "C" void jf_gate_next_request(int gateIndex)
{
	int cat, pos;
	nextIndex.store(-1);
	if (!locate(gateIndex, &cat, &pos)) {
		nextState.store(-1);
		return;
	}
	std::lock_guard<std::mutex> guard(lock);
	const Category &c = *cats[static_cast<size_t>(cat)];
	if (c.total < 0 || static_cast<size_t>(pos) >= c.entries.size()) {
		nextState.store(-1);
		return;
	}
	const Entry &e = c.entries[static_cast<size_t>(pos)];
	if (e.kind != "Episode" || e.seriesId.empty()) {
		nextState.store(-1);
		return;
	}
	nextJobs.clear();
	nextJobs.push_back(NextJob{ e.seriesId, e.id });
	nextState.store(1);
}

extern "C" int jf_gate_next_state(void)
{
	return nextState.load();
}

extern "C" int jf_gate_next_index(void)
{
	return nextIndex.load();
}

extern "C" void jf_gate_next_cancel(void)
{
	std::lock_guard<std::mutex> guard(lock);
	nextJobs.clear();
	nextState.store(0);
	nextIndex.store(-1);
}
