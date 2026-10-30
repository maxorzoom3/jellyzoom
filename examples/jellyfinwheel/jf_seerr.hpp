// Seerr (Jellyseerr / Overseerr): the Discover tab, a flat page of poster rows (Trending, My Requests, Popular and Upcoming
// movies and series), search, and one press to request a title. Plain HTTP on the local network, to Seerr's own port
// (or an IP with the site name after a slash for a reverse proxy: 192.168.1.10:80/seerr.mydomain.com).
// Needs kit.hpp and jf_gate.h before it, and json-c. Posters come through Seerr's /imageproxy.
#pragma once
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <json-c/json.h>

namespace jfseerr {

// ---- a small HTTP client that keeps the headers (Seerr's sign-in answers with a cookie) ----

struct Http {
	int status{ 0 };
	std::string headers, body;
};

inline std::string lower(std::string s)
{
	for (char &c : s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

inline std::string unchunk(const std::string &in)
{
	std::string out;
	size_t at = 0;
	while (at < in.size()) {
		const size_t eol = in.find("\r\n", at);
		if (eol == std::string::npos) {
			break;
		}
		const long size = std::strtol(in.substr(at, eol - at).c_str(), nullptr, 16);
		if (size <= 0) {
			break;
		}
		at = eol + 2;
		if (at + static_cast<size_t>(size) > in.size()) {
			out.append(in, at, std::string::npos);
			break;
		}
		out.append(in, at, static_cast<size_t>(size));
		at += static_cast<size_t>(size) + 2;
	}
	return out;
}

// "ip[:port][/name]" -> the machine to connect to (with "/name" when a site name goes in the Host header) and the port
inline std::string splitAddress(const std::string &text, int *port)
{
	if (text.rfind("https://", 0) == 0 || text.rfind("http://", 0) == 0) {
		// a name, with or without a port: the tunnel looks it up and, for https, does the TLS
		const bool tls = text[4] == 's';
		std::string rest = text.substr(tls ? 8 : 7);
		while (!rest.empty() && rest.back() == '/') {
			rest.pop_back();
		}
		*port = tls ? 443 : 80;
		const size_t colon = rest.rfind(':');
		if (colon != std::string::npos) {
			const int p = std::atoi(rest.c_str() + colon + 1);
			rest = rest.substr(0, colon);
			if (p > 0 && p < 65536) {
				*port = p;
			}
		}
		return (tls ? "https://" : "http://") + rest;
	}
	const size_t slash = text.find('/');
	std::string machine = text.substr(0, slash);
	const std::string name = slash == std::string::npos ? std::string() : text.substr(slash + 1);
	*port = 5055;
	const size_t colon = machine.rfind(':');
	if (colon != std::string::npos) {
		const int p = std::atoi(machine.c_str() + colon + 1);
		machine = machine.substr(0, colon);
		if (p > 0 && p < 65536) {
			*port = p;
		}
	}
	return name.empty() ? machine : machine + "/" + name;
}

inline std::string showAddress(const std::string &hostSpec, int port)
{
	if (hostSpec.rfind("https://", 0) == 0 || hostSpec.rfind("http://", 0) == 0) {
		const bool tls = hostSpec[4] == 's';
		return (tls && port == 443) || (!tls && port == 80) ? hostSpec : hostSpec + ":" + std::to_string(port);
	}
	const size_t slash = hostSpec.find('/');
	const std::string machine = hostSpec.substr(0, slash);
	return machine + ":" + std::to_string(port) + (slash == std::string::npos ? std::string() : hostSpec.substr(slash));
}

inline std::string statusText(int status)
{
	if (status == 599) {
		return "That address sends the console to HTTPS, which it cannot use yet. Use Seerr's own port, or a plain-HTTP route";
	}
	return "Seerr said no (HTTP " + std::to_string(status) + ")";
}

// What to say when nothing answered: the tunnel's own reason (name not found, certificate refused ...) when it has one
inline std::string unreachable(const std::string &host, int port)
{
	char problem[300] = "";
	jf_tls_error(problem, sizeof(problem));
	std::string text = problem[0] ? std::string(problem) : "Seerr did not answer at " + showAddress(host, port);
	if (text.size() > 120) {
		text = text.substr(0, 117) + "...";
	}
	return text;
}

// False when nothing could be reached; otherwise out holds whatever the server answered
inline bool request(const std::string &hostSpec, int port, const char *method, const std::string &path, const std::string &extraHeaders,
	const std::string &body, Http *out, int timeoutMs = 8000)
{
	std::string host, vhost;
	int connectPort = port;
	if (hostSpec.rfind("https://", 0) == 0 || hostSpec.rfind("http://", 0) == 0) {
		// a name: the tunnel (jf_tls.cpp) looks it up and, for https, does the TLS; here it is plain HTTP to the tunnel
		const bool tls = hostSpec[4] == 's';
		const std::string name = hostSpec.substr(tls ? 8 : 7);
		int local = 0;
		if (jf_tls_open(name.c_str(), port, tls ? 1 : 0, &local) != 0) {
			return false;
		}
		host = "127.0.0.1";
		connectPort = local;
		vhost = name + (((tls && port != 443) || (!tls && port != 80)) ? ":" + std::to_string(port) : std::string());
	} else {
		// "ip/name": connect to the ip, ask for the site called name (a reverse proxy picks the service by that name)
		const size_t slash = hostSpec.find('/');
		host = hostSpec.substr(0, slash);
		vhost = slash == std::string::npos ? host + ":" + std::to_string(port) : hostSpec.substr(slash + 1);
	}
	in_addr ip;
	if (inet_pton(AF_INET, host.c_str(), &ip) != 1) {
		return false;
	}
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		return false;
	}
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
	sockaddr_in sa;
	std::memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(static_cast<uint16_t>(connectPort));
	sa.sin_addr = ip;
	bool connected = connect(fd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) == 0;
	if (!connected && errno == EINPROGRESS) {
		pollfd p;
		p.fd = fd;
		p.events = POLLOUT;
		p.revents = 0;
		if (poll(&p, 1, 3000) == 1) {
			int err = 0;
			socklen_t len = sizeof(err);
			getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
			connected = err == 0;
		}
	}
	if (!connected) {
		close(fd);
		return false;
	}
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);

	std::string req = std::string(method) + " " + path + " HTTP/1.1\r\nHost: " + vhost +
		"\r\nAccept: */*\r\nUser-Agent: Jellyzoom\r\nConnection: close\r\n" + extraHeaders;
	if (!body.empty() || std::strcmp(method, "POST") == 0 || std::strcmp(method, "DELETE") == 0) {
		req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
	}
	req += "\r\n" + body;
	size_t sent = 0;
	while (sent < req.size()) {
		const ssize_t n = send(fd, req.data() + sent, req.size() - sent, 0);
		if (n <= 0) {
			close(fd);
			return false;
		}
		sent += static_cast<size_t>(n);
	}
	std::string raw;
	char buf[16384];
	for (;;) {
		pollfd waiting;
		waiting.fd = fd;
		waiting.events = POLLIN;
		waiting.revents = 0;
		if (poll(&waiting, 1, timeoutMs) != 1) {
			break; // nothing more came in time
		}
		const ssize_t n = recv(fd, buf, sizeof(buf), 0);
		if (n <= 0) {
			break;
		}
		raw.append(buf, static_cast<size_t>(n));
		if (raw.size() > 8u * 1024u * 1024u) {
			break;
		}
	}
	close(fd);
	const size_t split = raw.find("\r\n\r\n");
	if (split == std::string::npos || raw.compare(0, 5, "HTTP/") != 0) {
		return false;
	}
	out->headers = raw.substr(0, split);
	out->status = std::atoi(out->headers.c_str() + std::min<size_t>(9, out->headers.size()));
	std::string body2 = raw.substr(split + 4);
	if (lower(out->headers).find("transfer-encoding: chunked") != std::string::npos) {
		body2 = unchunk(body2);
	}
	out->body = body2;
	if (out->status >= 300 && out->status < 400) {
		out->status = 599; // a redirect: to HTTPS, in the case that matters here
	}
	return true;
}

inline std::string urlEncode(const std::string &s)
{
	static const char *hex = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : s) {
		if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
			out += static_cast<char>(c);
		} else {
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 15];
		}
	}
	return out;
}

inline std::string readText(const char *path)
{
	std::string out;
	if (FILE *f = std::fopen(path, "rb")) {
		char buf[512];
		const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
		out.assign(buf, n);
		std::fclose(f);
	}
	while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) {
		out.pop_back();
	}
	return out;
}

inline void writeText(const char *path, const std::string &text)
{
	if (FILE *f = std::fopen(path, "wb")) {
		std::fwrite(text.data(), 1, text.size(), f);
		std::fclose(f);
	}
}

inline json_object *jget(json_object *o, const char *key)
{
	json_object *v = nullptr;
	return (o && json_object_object_get_ex(o, key, &v)) ? v : nullptr;
}
inline std::string jstr(json_object *o, const char *key)
{
	json_object *v = jget(o, key);
	return (v && json_object_is_type(v, json_type_string)) ? json_object_get_string(v) : std::string();
}
inline long long jint(json_object *o, const char *key)
{
	json_object *v = jget(o, key);
	return v ? static_cast<long long>(json_object_get_int64(v)) : 0;
}

// ---- what the worker threads share with the screen (all under State::m) ----

struct Card {
	long long id{ 0 };
	std::string type; // "movie" or "tv"
	std::string title, year, overview, posterPath;
	std::string character; // a cast card: the role; a card on an actor's page: the role they played
	int status{ 1 };      // Seerr's media status: 1 unknown (not requested), 2 pending, 3 processing, 4 partly available, 5 available
	int requestState{ 0 }; // My Requests only: the request's own status (1 awaiting approval, 2 approved, 3 declined)
	bool asking{ false };
	int texState{ 0 };    // 0 none yet, 1 being fetched, 2 on the screen's hands, 3 none to be had
	std::uint32_t tex{ 0 };
	float ar{ 0.35f }, ag{ 0.45f }, ab{ 0.9f }; // the poster's main colour, for the backdrop and the glow
};

struct Row {
	int uid{ 0 };
	std::string name;
	std::string path;     // the endpoint, without ?page=
	std::string defaultType;
	bool mine{ false };
	bool search{ false };
	std::vector<Card> cards;
	int page{ 0 }, pages{ 1 };
	int loading{ 0 };     // a fetch is under way
	bool failed{ false };
	std::string note;     // what to say when it has nothing
};

// A page left behind while a cast page or an actor page is on show
struct Level {
	std::vector<Row> rows;
	int selRow{ 0 };
	float rowScroll{ 0.0f };
	std::vector<int> selCol;
	std::vector<float> colScroll;
};

struct Picture {
	int rowUid{ 0 };
	long long id{ 0 };
	std::string type;
	int w{ 0 }, h{ 0 };
	unsigned char *rgba{ nullptr };
};

struct State {
	std::mutex m;
	std::string host;
	int port{ 5055 };
	std::string cookie;
	std::atomic<int> busy{ 0 };      // sign-in and requests (the screen waits for these)
	std::string message;
	std::string notice;
	std::atomic<int> noticeSerial{ 0 };
	std::vector<Row> rows;
	std::vector<Level> stack; // Discover, then a cast page, ... : what Back returns to
	int nextUid{ 1 };
	std::vector<std::pair<int, long long>> wanted; // (row uid, card id) the screen is looking at, nearest first
	std::deque<Picture> pictures;
	std::atomic<int> runId{ 0 };     // the poster worker stops when this changes
	std::atomic<int> loginState{ 0 };
	std::atomic<int> needLogin{ 0 };
	long long myUserId{ 0 };
	bool loaded{ false };
	std::deque<std::pair<std::string, std::string>> cache; // downloaded posters (path, JPEG bytes), oldest first: one scrolled back to is not fetched again
};

inline State &state()
{
	static State *s = new State(); // never destroyed: a worker may still be running when the title ends
	return *s;
}

inline void loadConfig()
{
	State &s = state();
	std::lock_guard<std::mutex> g(s.m);
	if (s.loaded) {
		return;
	}
	s.loaded = true;
	const std::string server = readText("/data/jellyzoom-seerr");
	const size_t colon = server.rfind(':');
	if (colon != std::string::npos) {
		s.host = server.substr(0, colon);
		s.port = std::max(1, std::atoi(server.c_str() + colon + 1));
	}
	s.cookie = readText("/data/jellyzoom-seerr-cookie");
}

inline std::string cookieHeader(const State &s)
{
	return s.cookie.empty() ? std::string() : "Cookie: " + s.cookie + "\r\n";
}

inline void setMessage(const std::string &text)
{
	State &s = state();
	std::lock_guard<std::mutex> g(s.m);
	s.message = text;
}

// Signs in with the Jellyfin account; keeps the cookie Seerr answers with
inline void startLogin(const std::string &host, int port, const std::string &user, const std::string &pass)
{
	State &s = state();
	s.busy++;
	s.loginState = 0;
	setMessage("");
	std::thread([host, port, user, pass]() {
		State &st = state();
		json_object *body = json_object_new_object();
		json_object_object_add(body, "username", json_object_new_string(user.c_str()));
		json_object_object_add(body, "password", json_object_new_string(pass.c_str()));
		const std::string text = json_object_to_json_string(body);
		json_object_put(body);
		Http r;
		if (!request(host, port, "POST", "/api/v1/auth/jellyfin", "", text, &r)) {
			setMessage(unreachable(host, port));
			st.loginState = -1;
		} else if (r.status < 200 || r.status >= 300) {
			setMessage(r.status == 401 || r.status == 403 ? "Seerr did not accept that user name and password" : statusText(r.status));
			st.loginState = -1;
		} else {
			std::string cookie;
			const std::string low = lower(r.headers);
			size_t at = 0;
			while ((at = low.find("set-cookie:", at)) != std::string::npos) {
				const size_t eol = low.find("\r\n", at);
				const std::string line = r.headers.substr(at + 11, eol == std::string::npos ? std::string::npos : eol - at - 11);
				const size_t sid = line.find("connect.sid=");
				if (sid != std::string::npos) {
					const size_t end = line.find(';', sid);
					cookie = line.substr(sid, end == std::string::npos ? std::string::npos : end - sid);
				}
				at += 11;
			}
			if (cookie.empty()) {
				setMessage("Seerr signed you in but sent no session");
				st.loginState = -1;
			} else {
				json_object *me = json_tokener_parse(r.body.c_str());
				{
					std::lock_guard<std::mutex> g(st.m);
					st.host = host;
					st.port = port;
					st.cookie = cookie;
					st.myUserId = me ? jint(me, "id") : 0;
				}
				if (me) {
					json_object_put(me);
				}
				writeText("/data/jellyzoom-seerr", host + ":" + std::to_string(port));
				writeText("/data/jellyzoom-seerr-cookie", cookie);
				st.loginState = 1;
			}
		}
		st.busy--;
	}).detach();
}

inline void rowsReset()
{
	State &s = state();
	std::lock_guard<std::mutex> g(s.m);
	s.rows.clear();
	s.stack.clear();
	auto add = [&](const char *name, const char *path, const char *type, bool mine) {
		Row r;
		r.uid = s.nextUid++;
		r.name = name;
		r.path = path;
		r.defaultType = type;
		r.mine = mine;
		s.rows.push_back(r);
	};
	add("Trending", "/api/v1/discover/trending", "", false);
	add("My Requests", "/api/v1/request", "", true);
	add("Popular Movies", "/api/v1/discover/movies", "movie", false);
	add("Popular Series", "/api/v1/discover/tv", "tv", false);
	add("Upcoming Movies", "/api/v1/discover/movies/upcoming", "movie", false);
	add("Upcoming Series", "/api/v1/discover/tv/upcoming", "tv", false);
}

// Cards from a "results" array (discover, trending, search); people are skipped
inline void cardsFrom(json_object *list, const std::string &defaultType, std::vector<Card> *out)
{
	const int n = (list && json_object_is_type(list, json_type_array)) ? static_cast<int>(json_object_array_length(list)) : 0;
	for (int i = 0; i < n; i++) {
		json_object *o = json_object_array_get_idx(list, i);
		std::string type = jstr(o, "mediaType");
		if (type.empty()) {
			type = defaultType;
		}
		if (type != "movie" && type != "tv") {
			continue;
		}
		Card c;
		c.id = jint(o, "id");
		c.type = type;
		c.title = type == "movie" ? jstr(o, "title") : jstr(o, "name");
		const std::string date = type == "movie" ? jstr(o, "releaseDate") : jstr(o, "firstAirDate");
		c.year = date.size() >= 4 ? date.substr(0, 4) : std::string();
		c.overview = jstr(o, "overview");
		c.posterPath = jstr(o, "posterPath");
		if (json_object *info = jget(o, "mediaInfo")) {
			c.status = std::max(1, static_cast<int>(jint(info, "status")));
		}
		if (!c.title.empty() && c.id > 0) {
			out->push_back(c);
		}
	}
}

// Fetches the next page of a row (My Requests fetches its twenty newest, with their titles looked up)
inline void startRow(int uid)
{
	State &s = state();
	std::string host, cookie, path, defType;
	int port, page;
	bool mine, search;
	long long me;
	{
		std::lock_guard<std::mutex> g(s.m);
		Row *row = nullptr;
		for (Row &r : s.rows) {
			if (r.uid == uid) {
				row = &r;
			}
		}
		if (!row || row->loading || (row->page >= row->pages && row->page > 0)) {
			return;
		}
		row->loading = 1;
		row->failed = false;
		host = s.host;
		port = s.port;
		cookie = cookieHeader(s);
		path = row->path;
		defType = row->defaultType;
		page = row->page + 1;
		mine = row->mine;
		search = row->search;
		me = s.myUserId;
	}
	std::thread([uid, host, port, cookie, path, defType, page, mine, search, me]() {
		State &st = state();
		std::vector<Card> found;
		int pages = 1;
		std::string problem;
		bool unauthorised = false;
		long long userId = me;
		if (mine) {
			if (userId <= 0) {
				Http who;
				if (request(host, port, "GET", "/api/v1/auth/me", cookie, "", &who) && who.status == 200) {
					if (json_object *o = json_tokener_parse(who.body.c_str())) {
						userId = jint(o, "id");
						json_object_put(o);
						std::lock_guard<std::mutex> g(st.m);
						st.myUserId = userId;
					}
				} else if (who.status == 401 || who.status == 403) {
					unauthorised = true;
				}
			}
			Http r;
			if (!unauthorised && request(host, port, "GET",
					"/api/v1/request?take=20&skip=0&sort=added&requestedBy=" + std::to_string(userId), cookie, "", &r) &&
				r.status == 200) {
				if (json_object *root = json_tokener_parse(r.body.c_str())) {
					json_object *list = jget(root, "results");
					const int n = (list && json_object_is_type(list, json_type_array)) ? static_cast<int>(json_object_array_length(list)) : 0;
					for (int i = 0; i < n; i++) {
						json_object *o = json_object_array_get_idx(list, i);
						json_object *media = jget(o, "media");
						Card c;
						c.type = jstr(o, "type");
						c.id = jint(media, "tmdbId");
						c.requestState = static_cast<int>(jint(o, "status"));
						c.status = std::max(1, static_cast<int>(jint(media, "status")));
						if ((c.type != "movie" && c.type != "tv") || c.id <= 0) {
							continue;
						}
						// the request only holds ids: the title and poster come from the title's own page
						Http d;
						if (request(host, port, "GET", "/api/v1/" + c.type + "/" + std::to_string(c.id), cookie, "", &d) && d.status == 200) {
							if (json_object *t = json_tokener_parse(d.body.c_str())) {
								c.title = c.type == "movie" ? jstr(t, "title") : jstr(t, "name");
								const std::string date = c.type == "movie" ? jstr(t, "releaseDate") : jstr(t, "firstAirDate");
								c.year = date.size() >= 4 ? date.substr(0, 4) : std::string();
								c.overview = jstr(t, "overview");
								c.posterPath = jstr(t, "posterPath");
								json_object_put(t);
							}
						}
						if (c.title.empty()) {
							c.title = "(title " + std::to_string(c.id) + ")";
						}
						found.push_back(c);
					}
					json_object_put(root);
				}
			} else if (!unauthorised) {
				problem = "Could not load your requests";
			}
		} else {
			Http r;
			const std::string url = path + (path.find('?') == std::string::npos ? "?" : "&") + "page=" + std::to_string(page);
			if (!request(host, port, "GET", url, cookie, "", &r)) {
				problem = unreachable(host, port);
			} else if (r.status == 401 || r.status == 403) {
				unauthorised = true;
			} else if (r.status != 200) {
				problem = statusText(r.status);
			} else if (json_object *root = json_tokener_parse(r.body.c_str())) {
				cardsFrom(jget(root, "results"), defType, &found);
				pages = std::max(1, static_cast<int>(jint(root, "totalPages")));
				json_object_put(root);
			} else {
				problem = "Seerr's answer was unreadable";
			}
		}
		std::lock_guard<std::mutex> g(st.m);
		for (Row &row : st.rows) {
			if (row.uid != uid) {
				continue;
			}
			row.loading = 0;
			if (unauthorised) {
				st.message = "Sign in to Seerr again";
				st.needLogin = 1;
				row.failed = true;
			} else if (!problem.empty()) {
				row.failed = true;
				st.message = problem;
			} else {
				row.page = mine ? 1 : page;
				row.pages = mine ? 1 : pages;
				for (Card &c : found) {
					bool have = false;
					for (const Card &old : row.cards) {
						have = have || (old.id == c.id && old.type == c.type);
					}
					if (!have) {
						row.cards.push_back(c);
					}
				}
				if (row.cards.empty()) {
					row.note = mine ? "You have not requested anything yet" : search ? "Nothing found" : "Nothing to show";
				}
			}
		}
	}).detach();
}

// A search becomes the first row
inline void startSearch(const std::string &query)
{
	State &s = state();
	int uid;
	{
		std::lock_guard<std::mutex> g(s.m);
		if (!s.rows.empty() && s.rows.front().search) {
			for (Card &c : s.rows.front().cards) {
				(void)c; // textures are released by the screen when it notices the row is gone
			}
			s.rows.erase(s.rows.begin());
		}
		Row r;
		r.uid = s.nextUid++;
		r.name = "Results for \"" + query + "\"";
		r.path = "/api/v1/search?query=" + urlEncode(query);
		r.search = true;
		uid = r.uid;
		s.rows.insert(s.rows.begin(), r);
	}
	startRow(uid);
}

// Asks for the card (row uid, id); a series is asked for with all its seasons
inline void startRequest(int uid, long long id)
{
	State &s = state();
	std::string host, cookie, type, title;
	int port;
	{
		std::lock_guard<std::mutex> g(s.m);
		Card *card = nullptr;
		for (Row &row : s.rows) {
			if (row.uid == uid) {
				for (Card &c : row.cards) {
					if (c.id == id) {
						card = &c;
					}
				}
			}
		}
		if (!card || card->asking) {
			return;
		}
		card->asking = true;
		host = s.host;
		port = s.port;
		cookie = cookieHeader(s);
		type = card->type;
		title = card->title;
		s.message.clear();
	}
	s.busy++;
	std::thread([host, port, cookie, type, title, id]() {
		State &st = state();
		json_object *body = json_object_new_object();
		json_object_object_add(body, "mediaType", json_object_new_string(type.c_str()));
		json_object_object_add(body, "mediaId", json_object_new_int64(id));
		if (type == "tv") {
			json_object_object_add(body, "seasons", json_object_new_string("all"));
		}
		const std::string text = json_object_to_json_string(body);
		json_object_put(body);
		Http r;
		std::string note, problem;
		bool done = false;
		if (!request(host, port, "POST", "/api/v1/request", cookie, text, &r)) {
			problem = unreachable(host, port);
		} else if (r.status == 401) {
			st.needLogin = 1;
			problem = "Sign in to Seerr again";
		} else if (r.status == 201 || r.status == 200) {
			note = "Requested " + title;
			done = true;
		} else if (r.status == 409) {
			note = title + " is already requested";
			done = true;
		} else if (r.status == 403) {
			problem = "Seerr will not let this user request (limit reached, or no permission)";
		} else {
			problem = statusText(r.status);
		}
		{
			std::lock_guard<std::mutex> g(st.m);
			for (Row &row : st.rows) {
				for (Card &c : row.cards) {
					if (c.id == id && c.type == type) {
						c.asking = false;
						if (done && c.status < 2) {
							c.status = 2;
						}
					}
				}
			}
			for (Level &level : st.stack) {
				for (Row &row : level.rows) {
					for (Card &c : row.cards) {
						if (c.id == id && c.type == type && done && c.status < 2) {
							c.status = 2;
						}
					}
				}
			}
			if (done) {
				st.notice = note;
			} else {
				st.message = problem;
			}
		}
		if (done) {
			st.noticeSerial++;
		}
		st.busy--;
	}).detach();
}

// Cancels the user's own request(s) for a title: looks up its requests, deletes the ones this user made
inline void startCancel(long long id, const std::string &type, const std::string &title, bool mine)
{
	State &s = state();
	std::string host, cookie;
	int port;
	long long me;
	{
		std::lock_guard<std::mutex> g(s.m);
		host = s.host;
		port = s.port;
		cookie = cookieHeader(s);
		me = s.myUserId;
		s.message.clear();
	}
	s.busy++;
	std::thread([host, port, cookie, id, type, title, mine, me]() {
		State &st = state();
		std::string note, problem;
		bool done = false;
		long long userId = me;
		Http who;
		long long perms = 0;
		if (request(host, port, "GET", "/api/v1/auth/me", cookie, "", &who) && who.status == 200) {
			if (json_object *o = json_tokener_parse(who.body.c_str())) {
				userId = jint(o, "id");
				perms = jint(o, "permissions");
				json_object_put(o);
			}
		}
		// an administrator (2) or someone who manages requests (16) may cancel anybody's request
		const bool manager = (perms & (2 | 16)) != 0;
		std::vector<long long> mineIds;
		int others = 0;
		Http d;
		if (!request(host, port, "GET", "/api/v1/" + type + "/" + std::to_string(id), cookie, "", &d)) {
			problem = unreachable(host, port);
		} else if (d.status == 401) {
			st.needLogin = 1;
			problem = "Sign in to Seerr again";
		} else if (d.status != 200) {
			problem = statusText(d.status);
		} else if (json_object *t = json_tokener_parse(d.body.c_str())) {
			json_object *requests = jget(jget(t, "mediaInfo"), "requests");
			const int n = (requests && json_object_is_type(requests, json_type_array)) ? static_cast<int>(json_object_array_length(requests)) : 0;
			for (int i = 0; i < n; i++) {
				json_object *r = json_object_array_get_idx(requests, i);
				if (jint(r, "status") == 3) {
					continue; // already declined: nothing to cancel
				}
				if (manager || (userId > 0 && jint(jget(r, "requestedBy"), "id") == userId)) {
					mineIds.push_back(jint(r, "id"));
				} else {
					others++;
				}
			}
			json_object_put(t);
		}
		if (problem.empty()) {
			if (mineIds.empty()) {
				problem = others > 0 ? "Only the person who asked can cancel this request" : "No request of yours to cancel";
			} else {
				int failed = 0;
				std::string why;
				for (long long rid : mineIds) {
					Http r;
					const std::string path = "/api/v1/request/" + std::to_string(rid);
					bool ok = request(host, port, "DELETE", path, cookie, "", &r) && (r.status == 200 || r.status == 204);
					if (!ok && r.status != 0 && r.status != 401 && manager) {
						// some versions want a request declined before it can go
						Http dec;
						request(host, port, "POST", path + "/decline", cookie, "{}", &dec);
						Http again;
						ok = request(host, port, "DELETE", path, cookie, "", &again) && (again.status == 200 || again.status == 204);
						if (!ok) {
							r = again;
						}
					}
					if (!ok) {
						failed++;
						if (why.empty()) {
							// what Seerr itself said, so the reason can be read on screen
							std::string said;
							if (json_object *o = json_tokener_parse(r.body.c_str())) {
								said = jstr(o, "message");
								json_object_put(o);
							}
							why = r.status == 0 ? std::string("no answer") : "HTTP " + std::to_string(r.status);
							if (!said.empty()) {
								why += ": " + said.substr(0, 60);
							}
						}
					}
				}
				if (failed == 0) {
					note = "Cancelled the request for " + title;
					done = true;
				} else {
					problem = "Seerr would not cancel the request (" + why + ")";
				}
			}
		}
		{
			std::lock_guard<std::mutex> g(st.m);
			const auto apply = [&](std::vector<Row> &rows) {
				for (Row &row : rows) {
					for (size_t i = 0; i < row.cards.size(); i++) {
						Card &c = row.cards[i];
						if (c.id == id && c.type == type) {
							c.asking = false;
							if (done && others == 0) {
								c.status = 1;
								c.requestState = 0;
							}
						}
					}
					if (done && others == 0 && row.mine) {
						row.cards.erase(std::remove_if(row.cards.begin(), row.cards.end(),
							[&](const Card &c) { return c.id == id && c.type == type; }), row.cards.end());
					}
				}
			};
			apply(st.rows);
			for (Level &level : st.stack) {
				apply(level.rows);
			}
			if (done) {
				st.notice = note;
			} else {
				st.message = problem;
			}
		}
		if (done) {
			st.noticeSerial++;
		}
		st.busy--;
	}).detach();
}

// ---- a title's cast, an actor's films and series ----

// Text cut to at most max bytes (not in the middle of a letter), with ".." after it when it was cut
inline std::string clipText(const std::string &text, size_t max)
{
	if (text.size() <= max) {
		return text;
	}
	size_t n = max;
	while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) {
		n--;
	}
	return text.substr(0, n) + "..";
}

// "Self", "Himself" ...: an actor's talk-show appearances are not their work
inline bool playsThemselves(const std::string &role)
{
	const std::string r = lower(role);
	return r.rfind("self", 0) == 0 || r.rfind("himself", 0) == 0 || r.rfind("herself", 0) == 0 ||
		r.find("archive footage") != std::string::npos;
}

struct Credit {
	Card card;
	long long votes{ 0 };
};

// The cards of a credits list (an actor's "cast" or "crew"): the best known first, each title once, with a poster
inline void creditCards(json_object *list, const char *job, std::vector<Card> *movies, std::vector<Card> *shows, size_t limit = 40)
{
	const int n = (list && json_object_is_type(list, json_type_array)) ? static_cast<int>(json_object_array_length(list)) : 0;
	std::vector<Credit> m, t;
	for (int i = 0; i < n; i++) {
		json_object *o = json_object_array_get_idx(list, i);
		const std::string type = jstr(o, "mediaType");
		if (type != "movie" && type != "tv") {
			continue;
		}
		if (job && jstr(o, "job") != job) {
			continue;
		}
		Credit k;
		Card &c = k.card;
		c.id = jint(o, "id");
		c.type = type;
		c.title = type == "movie" ? jstr(o, "title") : jstr(o, "name");
		const std::string date = type == "movie" ? jstr(o, "releaseDate") : jstr(o, "firstAirDate");
		c.year = date.size() >= 4 ? date.substr(0, 4) : std::string();
		c.overview = jstr(o, "overview");
		c.posterPath = jstr(o, "posterPath");
		c.character = job ? std::string(job) : jstr(o, "character");
		if (c.title.empty() || c.id <= 0 || c.posterPath.size() < 2 || (!job && playsThemselves(c.character))) {
			continue;
		}
		if (json_object *info = jget(o, "mediaInfo")) {
			c.status = std::max(1, static_cast<int>(jint(info, "status")));
		}
		k.votes = jint(o, "voteCount");
		(type == "movie" ? m : t).push_back(k);
	}
	auto take = [limit](std::vector<Credit> &from, std::vector<Card> *to) {
		if (!to) {
			return;
		}
		std::stable_sort(from.begin(), from.end(), [](const Credit &a, const Credit &b) { return a.votes > b.votes; });
		for (const Credit &k : from) {
			bool have = false;
			for (const Card &c : *to) {
				have = have || c.id == k.card.id;
			}
			if (!have && to->size() < limit) {
				to->push_back(k.card);
			}
		}
	};
	take(m, movies);
	take(t, shows);
}

// A page's row is ready (or failed): what it holds now, if the person is still on that page
inline void pageReady(int uid, std::vector<Card> cards, const std::string &problem, bool unauthorised)
{
	State &s = state();
	std::lock_guard<std::mutex> g(s.m);
	for (Row &row : s.rows) {
		if (row.uid != uid) {
			continue;
		}
		row.loading = 0;
		if (unauthorised) {
			s.message = "Sign in to Seerr again";
			s.needLogin = 1;
			row.failed = true;
		} else if (!problem.empty()) {
			row.failed = true;
			s.message = problem;
		} else {
			row.cards = std::move(cards);
		}
		return;
	}
}

// The cast of a movie or series: Seerr's page for the title lists it
inline void startCast(int uid, long long id, const std::string &type)
{
	State &s = state();
	std::string host, cookie;
	int port;
	{
		std::lock_guard<std::mutex> g(s.m);
		host = s.host;
		port = s.port;
		cookie = cookieHeader(s);
	}
	std::thread([uid, id, type, host, port, cookie]() {
		std::vector<Card> found;
		std::string problem;
		bool unauthorised = false;
		Http r;
		if (!request(host, port, "GET", "/api/v1/" + type + "/" + std::to_string(id), cookie, "", &r)) {
			problem = unreachable(host, port);
		} else if (r.status == 401 || r.status == 403) {
			unauthorised = true;
		} else if (r.status != 200) {
			problem = statusText(r.status);
		} else if (json_object *root = json_tokener_parse(r.body.c_str())) {
			json_object *list = jget(jget(root, "credits"), "cast");
			const int n = (list && json_object_is_type(list, json_type_array)) ? static_cast<int>(json_object_array_length(list)) : 0;
			for (int i = 0; i < n && found.size() < 40; i++) {
				json_object *o = json_object_array_get_idx(list, i);
				Card c;
				c.id = jint(o, "id");
				c.type = "person";
				c.title = jstr(o, "name");
				c.character = jstr(o, "character");
				c.posterPath = jstr(o, "profilePath");
				if (!c.title.empty() && c.id > 0) {
					found.push_back(c);
				}
			}
			json_object_put(root);
		} else {
			problem = "Seerr's answer was unreadable";
		}
		pageReady(uid, found, problem, unauthorised);
	}).detach();
}

// An actor: when and where they were born (a line on the first row), their films, their series, and what they directed
inline void startPerson(int movieUid, int showUid, long long personId, const std::string &name)
{
	State &s = state();
	std::string host, cookie;
	int port;
	{
		std::lock_guard<std::mutex> g(s.m);
		host = s.host;
		port = s.port;
		cookie = cookieHeader(s);
	}
	std::thread([movieUid, showUid, personId, name, host, port, cookie]() {
		State &st = state();
		std::string problem, facts;
		bool unauthorised = false;
		std::vector<Card> movies, shows, directed;
		const std::string base = "/api/v1/person/" + std::to_string(personId);
		Http d;
		if (request(host, port, "GET", base, cookie, "", &d) && d.status == 200) {
			if (json_object *p = json_tokener_parse(d.body.c_str())) {
				const std::string born = jstr(p, "birthday"), died = jstr(p, "deathday");
				if (born.size() >= 4) {
					facts = died.size() >= 4 ? born.substr(0, 4) + " - " + died.substr(0, 4) : "born " + born.substr(0, 4);
				}
				const std::string place = jstr(p, "placeOfBirth");
				if (!place.empty()) {
					facts += (facts.empty() ? "" : ", ") + clipText(place, 40);
				}
				json_object_put(p);
			}
		}
		Http r;
		if (!request(host, port, "GET", base + "/combined_credits", cookie, "", &r)) {
			problem = unreachable(host, port);
		} else if (r.status == 401 || r.status == 403) {
			unauthorised = true;
		} else if (r.status != 200) {
			problem = statusText(r.status);
		} else if (json_object *root = json_tokener_parse(r.body.c_str())) {
			creditCards(jget(root, "cast"), nullptr, &movies, &shows);
			std::vector<Card> directedMovies, directedShows;
			creditCards(jget(root, "crew"), "Director", &directedMovies, &directedShows);
			directed = directedMovies;
			directed.insert(directed.end(), directedShows.begin(), directedShows.end());
			json_object_put(root);
		} else {
			problem = "Seerr's answer was unreadable";
		}
		std::lock_guard<std::mutex> g(st.m);
		Row *mv = nullptr, *sh = nullptr;
		for (Row &row : st.rows) {
			if (row.uid == movieUid) {
				mv = &row;
			}
			if (row.uid == showUid) {
				sh = &row;
			}
		}
		if (!mv || !sh) {
			return; // the person has left the page
		}
		mv->loading = 0;
		sh->loading = 0;
		if (unauthorised) {
			st.message = "Sign in to Seerr again";
			st.needLogin = 1;
			mv->failed = sh->failed = true;
			return;
		}
		if (!problem.empty()) {
			st.message = problem;
			mv->failed = sh->failed = true;
			return;
		}
		mv->cards = movies;
		sh->cards = shows;
		if (!facts.empty()) {
			mv->name += "  \xC2\xB7  " + facts;
		}
		if (!directed.empty()) {
			Row dir;
			dir.uid = st.nextUid++;
			dir.name = name + ": Directed";
			dir.defaultType = "movie";
			dir.page = dir.pages = 1;
			dir.cards = directed;
			st.rows.push_back(dir); // mv and sh are not used after this
		}
	}).detach();
}

// One thread fetches the posters the screen is looking at, nearest first, through Seerr's image proxy
inline void startPosterWorker()
{
	State &s = state();
	const int run = ++s.runId;
	auto work = [run]() {
		State &st = state();
		while (st.runId.load() == run) {
			std::string host, path, type, key, bytes;
			int port = 0, uid = 0;
			long long id = 0;
			bool have = false;
			{
				std::lock_guard<std::mutex> g(st.m);
				for (const auto &want : st.wanted) {
					for (Row &row : st.rows) {
						if (row.uid != want.first) {
							continue;
						}
						for (Card &c : row.cards) {
							if (c.id == want.second && c.texState == 0) {
								if (c.posterPath.size() < 2) {
									c.texState = 3;
									continue;
								}
								c.texState = 1;
								host = st.host;
								port = st.port;
								key = c.posterPath;
								path = "/imageproxy/tmdb/t/p/w185" + c.posterPath;
								{
									for (const auto &entry : st.cache) {
										if (entry.first == key) {
											bytes = entry.second;
											break;
										}
									}
								}
								uid = row.uid;
								id = c.id;
								type = c.type;
								have = true;
								break;
							}
						}
						if (have) {
							break;
						}
					}
					if (have) {
						break;
					}
				}
			}
			if (!have) {
				std::this_thread::sleep_for(std::chrono::milliseconds(40));
				continue;
			}
			Http r;
			unsigned char *pixels = nullptr;
			int w = 0, h = 0;
			bool fromNet = false;
			if (bytes.empty() && request(host, port, "GET", path, "", "", &r, 6000) && r.status == 200 && !r.body.empty()) {
				bytes.swap(r.body);
				fromNet = true;
			}
			if (!bytes.empty()) {
				if (!jf_gate_decode_image(reinterpret_cast<const unsigned char *>(bytes.data()), static_cast<int>(bytes.size()), &w, &h, &pixels)) {
					pixels = nullptr;
				} else if (fromNet) {
					std::lock_guard<std::mutex> g(st.m);
					st.cache.emplace_back(key, bytes);
					while (st.cache.size() > 600) {
						st.cache.pop_front();
					}
				}
			}
			std::lock_guard<std::mutex> g(st.m);
			if (pixels) {
				Picture p;
				p.rowUid = uid;
				p.id = id;
				p.type = type;
				p.w = w;
				p.h = h;
				p.rgba = pixels;
				st.pictures.push_back(p);
			} else {
				for (Row &row : st.rows) {
					for (Card &c : row.cards) {
						if (row.uid == uid && c.id == id && c.type == type) {
							c.texState = 3;
						}
					}
				}
			}
		}
	};
	// several at once: Seerr fetches each poster from TMDB the first time, which is slow one by one
	for (int i = 0; i < 4; i++) {
		std::thread(work).detach();
	}
}

// ---- the screen ----

class Screen
{
  public:
	using CreateTexture = std::function<std::uint32_t(int, int, const unsigned char *)>;
	using DestroyTexture = std::function<void(std::uint32_t)>;

	// The program's textures: the screen asks for one per poster and gives it back
	void bind(CreateTexture create, DestroyTexture destroy)
	{
		create_ = std::move(create);
		destroy_ = std::move(destroy);
	}

	bool is_open() const { return open_; }

	// The Discover tab was selected: start from the saved session, or wait for the person to sign in
	void activate()
	{
		loadConfig();
		open_ = true;
		kb_ = false;
		connecting_ = false;
		needSignIn_ = false;
		autoAsk_ = false;
		wantTabs_ = false;
		selRow_ = 0;
		rowScroll_ = 0.0f;
		char buf[128];
		jf_gate_auth_text(2, buf, sizeof(buf));
		user_ = buf;
		State &s = state();
		{
			// Stage 36: Settings asked for the address or the password again
			const int go = ::jf::discoverGo();
			if (go == 1 || go == 2) {
				::jf::discoverGo() = 0;
				forgetSession();
				std::string known;
				{
					std::lock_guard<std::mutex> g(s.m);
					known = s.host;
				}
				if (go == 2 && !known.empty()) {
					connecting_ = true;
					needSignIn_ = false;
					openKeyboard(Ask::password, "");
				} else {
					beginConnect();
				}
				return;
			}
		}
		bool have;
		std::string host;
		int port;
		{
			std::lock_guard<std::mutex> g(s.m);
			have = !s.host.empty() && !s.cookie.empty();
			host = s.host;
			port = s.port;
			s.message.clear();
		}
		s.loginState = 0;
		s.needLogin = 0;
		if (have) {
			startDiscover();
			return;
		}
		// no session: the Jellyfin password, when it was saved, signs in without asking
		const std::string saved = readText("/data/jellyzoom-pass");
		if (!host.empty() && !saved.empty() && !user_.empty()) {
			connecting_ = true;
			startLogin(host, port, user_, saved);
		} else {
			needSignIn_ = true;
			autoAsk_ = true;
		}
	}

	// The tab was left (or Seerr switched off): the posters go back and the poster worker stops
	void deactivate()
	{
		::jf::castRequest().pending = false;
		fromLibrary_ = false;
		if (open_) {
			closeScreen();
		}
	}

	// True once when the page wants the tabs to have the pad back
	bool takeTabsRequest()
	{
		const bool w = wantTabs_;
		wantTabs_ = false;
		return w;
	}

	// Sign out of Jellyfin: the Seerr session belongs to that account
	static void forgetSession()
	{
		writeText("/data/jellyzoom-seerr-cookie", "");
		State &s = state();
		std::lock_guard<std::mutex> g(s.m);
		s.cookie.clear();
		s.myUserId = 0;
	}

	template <class Feedback>
	void update(const hui::InputFrame &in, float dt, Feedback &fb, bool pad)
	{
		if (!open_) {
			return;
		}
		clock_ += dt;
		pad_ = pad;
		State &s = state();
		takePictures();
		{
			std::lock_guard<std::mutex> g(s.m);
			message_ = s.message;
			if (s.noticeSerial.load() != noticeSeen_) {
				noticeSeen_ = s.noticeSerial.load();
				notice_ = s.notice;
				noticeClock_ = 4.0f;
			}
			rowCount_ = static_cast<int>(s.rows.size());
		}
		noticeClock_ = std::max(0.0f, noticeClock_ - dt);
		{
			// the backdrop takes the colours of the poster under the cursor, as the wheel's takes the cover's
			static const float night[3] = { 0.012f, 0.016f, 0.031f };
			const float k = std::min(1.0f, dt * 4.0f);
			const float dark = 0.22f, sky = 0.34f, mid = 0.62f;
			for (int i = 0; i < 3; i++) {
				const float to[3] = { want_[i] * dark * 0.4f + night[i] * 0.6f, want_[i] * sky * 0.65f + night[i] * 0.35f, want_[i] * mid };
				for (int j = 0; j < 3; j++) {
					tint_[j][i] += (to[j] - tint_[j][i]) * k;
				}
			}
		}
		busy_ = s.busy.load() > 0;
		if (s.loginState.load() == 1 && connecting_) {
			s.loginState = 0;
			connecting_ = false;
			needSignIn_ = false;
			startDiscover();
			fb.play(hui::audio::Cue::open);
			return;
		}
		if (s.loginState.load() == -1 && connecting_) {
			s.loginState = 0;
			connecting_ = false;
			fb.play(hui::audio::Cue::error);
			if (pad) {
				beginConnect(); // the message stays on screen
			} else {
				needSignIn_ = true;
				autoAsk_ = false;
			}
			return;
		}
		if (s.needLogin.load() && !connecting_ && !needSignIn_) {
			s.needLogin = 0;
			rowsReset();
			writeText("/data/jellyzoom-seerr-cookie", "");
			{
				std::lock_guard<std::mutex> g(s.m);
				s.cookie.clear();
			}
			needSignIn_ = true;
			autoAsk_ = false;
			return;
		}
		if (!pad && !kb_ && !needSignIn_ && !connecting_) {
			wantPosters(); // the posters load while the tabs have the pad too
		}
		if (::jf::castRequest().pending && open_ && !needSignIn_ && !connecting_ && !kb_) {
			::jf::CastRequest &cr = ::jf::castRequest();
			cr.pending = false;
			leaveAll();
			fromLibrary_ = true;
			openCast(cr.tmdb, cr.type, cr.title);
			return;
		}
		if (busy_ || !pad) {
			return;
		}
		if (kb_) {
			keyboard(in, fb);
			return;
		}
		if (needSignIn_) {
			if (autoAsk_ || in.is_pressed(hui::Action::confirm)) {
				autoAsk_ = false;
				beginConnect();
			} else if (in.is_pressed(hui::Action::back) || in.nav == hui::Direction::up) {
				fb.play(hui::audio::Cue::back);
				wantTabs_ = true;
			}
			return;
		}
		if (!connecting_) {
			browse(in, dt, fb);
		}
	}

	// Drawn by the design, in its scene, under the tabs: the page sets the backdrop as the wheel does
	void draw(hui::app::Frame &frame, const hui::ui::Fonts &fonts) const
	{
		using hui::gfx::Align;
		using hui::gfx::Color;
		using hui::gfx::Rect;
		if (!open_) {
			return;
		}
		hui::gfx::DrawList &list = frame.scene;
		frame.backdrop.mode = hui::gfx::BackdropMode::gradient;
		frame.backdrop.colors[0] = Color{ tint_[0][0], tint_[0][1], tint_[0][2], 1.0f };
		frame.backdrop.colors[1] = Color{ tint_[1][0], tint_[1][1], tint_[1][2], 1.0f };
		frame.backdrop.colors[2] = Color{ tint_[2][0], tint_[2][1], tint_[2][2], 1.0f };
		frame.backdrop.params[0] = 0.5f;
		frame.backdrop.params[1] = 0.34f;
		frame.backdrop.params[2] = 0.5f + 0.06f * std::sin(clock_ * 1.2f);
		frame.backdrop.time = clock_;
		const Color white{ 1, 1, 1, 1 };
		// a floor under the caption, as the wheel has
		list.gradient_rect(Rect{ 0, 700, 1920, 380 }, 0.0f, Color{ 0.012f, 0.016f, 0.031f, 0.0f }, Color{ 0.012f, 0.016f, 0.031f, 0.82f });
		if (busy_ || connecting_) {
			const float a = 0.5f + 0.4f * std::sin(clock_ * 3.0f);
			hui::ui::text(list, fonts.semibold, "ONE MOMENT ...", 1830.0f, 124.0f, 18, white.with_alpha(a), Align::right, 4.0f);
		} else if (!message_.empty()) {
			hui::ui::text(list, fonts.regular, message_.c_str(), 1830.0f, 126.0f, 24, Color{ 1.0f, 0.62f, 0.55f, 1.0f }, Align::right);
		} else if (noticeClock_ > 0.0f) {
			hui::ui::text(list, fonts.regular, notice_.c_str(), 1830.0f, 126.0f, 24, Color{ 0.6f, 0.92f, 0.65f, 1.0f }, Align::right);
		}
		if (kb_) {
			drawKeyboard(list, fonts);
			return;
		}
		if (needSignIn_) {
			hui::ui::text(list, fonts.display, "Discover", 960.0f, 440.0f, 72, white, Align::center);
			hui::ui::text(list, fonts.regular, "Sign in to Seerr with your Jellyfin account to see what is trending and to request titles.",
				960.0f, 510.0f, 26, white.with_alpha(0.7f), Align::center);
			return;
		}
		if (connecting_) {
			return;
		}
		drawRows(list, fonts);
		if (pad_) {
			drawLegend(list, fonts);
		}
	}

  private:
	struct Cell {
		std::string label;
		char ch{ 0 };
		int control{ 0 }; // 1 shift, 2 symbols, 3 space, 4 delete, 5 done
		int col{ 0 }, span{ 1 };
	};
	enum class Ask { address, password, search };

	static constexpr float kCardW = 180.0f, kCardH = 270.0f, kPitch = 204.0f, kRowH = 330.0f;
	static constexpr int kMaxTextures = 150;

	CreateTexture create_;
	DestroyTexture destroy_;
	bool open_{ false }, busy_{ false }, kb_{ false }, showText_{ true }, connecting_{ false };
	bool needSignIn_{ false }, autoAsk_{ false }, wantTabs_{ false };
	long long cancelId_{ 0 }; // Circle was pressed once on this requested title: a second press within a few seconds cancels
	std::string cancelType_;
	float cancelClock_{ 0.0f };
	bool fromLibrary_{ false }; // the cast page on show was asked for by a film page: Back leaves the tab
	bool pad_{ false }; // the page (not the tabs) has the pad: it draws its own button legend
	Ask ask_{ Ask::search };
	float clock_{ 0.0f }, noticeClock_{ 0.0f }, evictClock_{ 0.0f };
	int selRow_{ 0 }, rowCount_{ 0 }, noticeSeen_{ 0 };
	float rowScroll_{ 0.0f };
	float tint_[3][3]{ { 0.05f, 0.07f, 0.16f }, { 0.08f, 0.11f, 0.26f }, { 0.20f, 0.26f, 0.55f } }; // backdrop top, bottom, spotlight
	float want_[3]{ 0.35f, 0.45f, 0.9f };
	std::vector<int> selCol_;           // the card each row has the cursor on
	mutable std::vector<float> colScroll_; // how far each row has slid
	std::string user_, message_, notice_, text_, address_;
	int kbPage_{ 0 }, kbRow_{ 1 }, kbCol_{ 0 };
	std::vector<std::vector<Cell>> rows_;

	void startDiscover()
	{
		rowsReset();
		selRow_ = 0;
		selCol_.assign(8, 0);
		colScroll_.assign(8, 0.0f);
		rowScroll_ = 0.0f;
		startPosterWorker();
		State &s = state();
		int a = 0, b = 0;
		{
			std::lock_guard<std::mutex> g(s.m);
			a = s.rows[0].uid;
			b = s.rows[1].uid;
		}
		startRow(a);
		startRow(b);
	}

	// A cast page or an actor page: the rows on show are put aside (their pictures given back) and these take their place
	void enterPage(std::vector<Row> rows)
	{
		State &s = state();
		std::lock_guard<std::mutex> g(s.m);
		giveBack(s.rows);
		Level l;
		l.rows = std::move(s.rows);
		l.selRow = selRow_;
		l.rowScroll = rowScroll_;
		l.selCol = selCol_;
		l.colScroll = colScroll_;
		s.stack.push_back(std::move(l));
		s.rows = std::move(rows);
		s.wanted.clear();
		selRow_ = 0;
		rowScroll_ = 0.0f;
		selCol_.assign(8, 0);
		colScroll_.assign(8, 0.0f);
		rowCount_ = static_cast<int>(s.rows.size());
	}

	// Back from a cast or actor page to the page before it; false when there is none
	bool leavePage()
	{
		State &s = state();
		std::lock_guard<std::mutex> g(s.m);
		if (s.stack.empty()) {
			return false;
		}
		giveBack(s.rows);
		Level l = std::move(s.stack.back());
		s.stack.pop_back();
		s.rows = std::move(l.rows);
		s.wanted.clear();
		selRow_ = l.selRow;
		rowScroll_ = l.rowScroll;
		selCol_ = l.selCol;
		colScroll_ = l.colScroll;
		rowCount_ = static_cast<int>(s.rows.size());
		return true;
	}

	void leaveAll()
	{
		fromLibrary_ = false;
		while (leavePage()) {
		}
	}

	// The pictures of these rows go back to the program; the posters are fetched again (from the cache) if the rows return
	void giveBack(std::vector<Row> &rows)
	{
		for (Row &row : rows) {
			for (Card &c : row.cards) {
				if (c.tex && destroy_) {
					destroy_(c.tex);
				}
				c.tex = 0;
				if (c.texState == 1 || c.texState == 2) {
					c.texState = 0;
				}
			}
		}
	}

	void openCast(long long id, const std::string &type, const std::string &title)
	{
		State &s = state();
		Row r;
		{
			std::lock_guard<std::mutex> g(s.m);
			r.uid = s.nextUid++;
		}
		r.name = "Cast of " + title;
		r.defaultType = "person";
		r.loading = 1;
		r.page = r.pages = 1; // nothing more to fetch
		r.note = "No cast is listed";
		const int uid = r.uid;
		std::vector<Row> rows;
		rows.push_back(r);
		enterPage(std::move(rows));
		startCast(uid, id, type);
	}

	void openPerson(long long id, const std::string &name)
	{
		State &s = state();
		std::vector<Row> rows(2);
		{
			std::lock_guard<std::mutex> g(s.m);
			rows[0].uid = s.nextUid++;
			rows[1].uid = s.nextUid++;
		}
		rows[0].name = name + ": Movies";
		rows[0].defaultType = "movie";
		rows[0].note = "No films are listed";
		rows[1].name = name + ": Series";
		rows[1].defaultType = "tv";
		rows[1].note = "No series are listed";
		for (Row &r : rows) {
			r.loading = 1;
			r.page = r.pages = 1;
		}
		const int a = rows[0].uid, b = rows[1].uid;
		enterPage(std::move(rows));
		startPerson(a, b, id, name);
	}

	// Gives every texture back and stops the poster worker
	void closeScreen()
	{
		State &s = state();
		s.runId++;
		std::lock_guard<std::mutex> g(s.m);
		for (Row &row : s.rows) {
			for (Card &c : row.cards) {
				if (c.tex && destroy_) {
					destroy_(c.tex);
				}
				c.tex = 0;
				if (c.texState == 2) {
					c.texState = 0;
				}
			}
		}
		for (Picture &p : s.pictures) {
			std::free(p.rgba);
		}
		s.pictures.clear();
		s.stack.clear();
		open_ = false;
	}

	// The poster's main colour: the average of a sample of its pixels, lifted so its strongest channel is full
	static void accentOf(const Picture &p, Card *c)
	{
		double r = 0, g = 0, b = 0;
		long n = 0;
		const long total = static_cast<long>(p.w) * p.h;
		for (long i = 0; i < total; i += 7) {
			const unsigned char *px = p.rgba + i * 4;
			r += px[0];
			g += px[1];
			b += px[2];
			n++;
		}
		if (n == 0) {
			return;
		}
		const float peak = static_cast<float>(std::max(r, std::max(g, b)) / static_cast<double>(n)) / 255.0f;
		const float lift = 1.0f / std::max(0.2f, peak);
		c->ar = std::min(1.0f, static_cast<float>(r / n) / 255.0f * lift);
		c->ag = std::min(1.0f, static_cast<float>(g / n) / 255.0f * lift);
		c->ab = std::min(1.0f, static_cast<float>(b / n) / 255.0f * lift);
	}

	// Tells the poster worker which posters to fetch: around the cursor first, a screen ahead of it and a little behind,
	// then the rows near it
	void wantPosters()
	{
		State &s = state();
		std::lock_guard<std::mutex> g(s.m);
		s.wanted.clear();
		const int rows = static_cast<int>(s.rows.size());
		const int order[4] = { selRow_, selRow_ + 1, selRow_ - 1, selRow_ + 2 };
		for (int n = 0; n < 4; n++) {
			const int r = order[n];
			if (r < 0 || r >= rows) {
				continue;
			}
			const Row &row = s.rows[static_cast<size_t>(r)];
			const int cards = static_cast<int>(row.cards.size());
			const int centre = r < static_cast<int>(selCol_.size()) ? selCol_[static_cast<size_t>(r)] : 0;
			const int ahead = n == 0 ? 20 : 12, behind = n == 0 ? 6 : 3;
			for (int d = 0; d <= ahead; d++) {
				if (centre + d < cards) {
					s.wanted.emplace_back(row.uid, row.cards[static_cast<size_t>(centre + d)].id);
				}
				if (d >= 1 && d <= behind && centre - d >= 0) {
					s.wanted.emplace_back(row.uid, row.cards[static_cast<size_t>(centre - d)].id);
				}
			}
		}
	}

	void takePictures()
	{
		State &s = state();
		std::lock_guard<std::mutex> g(s.m);
		for (int taken = 0; taken < 4 && !s.pictures.empty(); taken++) {
			Picture p = s.pictures.front();
			s.pictures.pop_front();
			bool placed = false;
			for (Row &row : s.rows) {
				if (row.uid != p.rowUid) {
					continue;
				}
				for (Card &c : row.cards) {
					if (c.id == p.id && c.type == p.type && c.texState == 1 && create_) {
						c.tex = create_(p.w, p.h, p.rgba);
						accentOf(p, &c);
						c.texState = c.tex ? 2 : 3;
						placed = true;
					}
				}
			}
			(void)placed;
			std::free(p.rgba);
		}
	}

	bool haveSession() const
	{
		State &s = state();
		std::lock_guard<std::mutex> g(s.m);
		return !s.cookie.empty();
	}

	void beginConnect(bool expired = false)
	{
		connecting_ = true;
		needSignIn_ = false;
		State &s = state();
		std::string host;
		int port;
		{
			std::lock_guard<std::mutex> g(s.m);
			host = s.host;
			port = s.port;
			if (expired) {
				s.cookie.clear();
			}
		}
		if (host.empty()) {
			const std::string jf = readText("/data/jellyzoom-server"); // "192.168.1.10:8096": Seerr is often on the same machine
			if (jf.rfind("http", 0) == 0) {
				address_ = "https://"; // Jellyfin is behind a name: so is Seerr, probably
			} else {
				const size_t colon = jf.rfind(':');
				host = colon == std::string::npos ? jf : jf.substr(0, colon);
				port = 5055;
				address_ = showAddress(host, port);
			}
		} else {
			address_ = showAddress(host, port);
		}
		openKeyboard(Ask::address, address_);
	}

	void openKeyboard(Ask ask, const std::string &initial)
	{
		ask_ = ask;
		text_ = initial;
		kb_ = true;
		showText_ = ask != Ask::password;
		kbPage_ = 0;
		kbRow_ = 1;
		kbCol_ = 0;
		buildKeys();
	}

	void buildKeys()
	{
		static const char *const lowerSet[4] = { "1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm._:" };
		static const char *const upperSet[4] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM._:" };
		static const char *const symSet[4] = { "!@#$%^&*()", "-_=+[]{};:", "'\"<>,.?/\\|", "~`@#.:/_-+" };
		const char *const *set = kbPage_ == 0 ? lowerSet : kbPage_ == 1 ? upperSet : symSet;
		rows_.assign(5, {});
		for (int r = 0; r < 4; r++) {
			for (int c = 0; c < 10; c++) {
				Cell cell;
				cell.ch = set[r][c];
				cell.label = std::string(1, cell.ch);
				cell.col = c;
				rows_[static_cast<size_t>(r)].push_back(cell);
			}
		}
		auto control = [&](const char *label, int kind, int col, int span) {
			Cell cell;
			cell.label = label;
			cell.control = kind;
			cell.col = col;
			cell.span = span;
			rows_[4].push_back(cell);
		};
		control(kbPage_ == 1 ? "abc" : "ABC", 1, 0, 2);
		control(kbPage_ == 2 ? "abc" : "#+=", 2, 2, 2);
		control("space", 3, 4, 3);
		control("delete", 4, 7, 2);
		control("done", 5, 9, 1);
	}

	int keyAt(int row, int col) const
	{
		const auto &r = rows_[static_cast<size_t>(row)];
		int best = 0, bestDistance = 99;
		for (int i = 0; i < static_cast<int>(r.size()); i++) {
			const Cell &c = r[static_cast<size_t>(i)];
			if (col >= c.col && col < c.col + c.span) {
				return i;
			}
			const int d = std::abs(c.col - col);
			if (d < bestDistance) {
				bestDistance = d;
				best = i;
			}
		}
		return best;
	}

	template <class Feedback>
	void finishKeyboard(Feedback &fb)
	{
		fb.play(hui::audio::Cue::open);
		kb_ = false;
		State &s = state();
		if (ask_ == Ask::address) {
			int port = 5055;
			const std::string host = splitAddress(text_, &port);
			address_ = showAddress(host, port);
			{
				std::lock_guard<std::mutex> g(s.m);
				s.host = host;
				s.port = port;
			}
			openKeyboard(Ask::password, "");
		} else if (ask_ == Ask::password) {
			std::string host;
			int port;
			{
				std::lock_guard<std::mutex> g(s.m);
				host = s.host;
				port = s.port;
			}
			startLogin(host, port, user_, text_);
			text_.clear();
		} else if (!text_.empty()) {
			startSearch(text_);
			selRow_ = 0;
			selCol_.insert(selCol_.begin(), 0);
			colScroll_.insert(colScroll_.begin(), 0.0f);
		}
	}

	template <class Feedback>
	void keyboard(const hui::InputFrame &in, Feedback &fb)
	{
		if (in.is_pressed(hui::Action::back)) {
			fb.play(hui::audio::Cue::back);
			kb_ = false;
			text_.clear();
			if (connecting_ || !haveSession()) {
				connecting_ = false;
				needSignIn_ = true; // no Seerr connection yet: back to the sign-in page
				autoAsk_ = false;
			}
			return;
		}
		if (in.is_pressed(hui::Action::west) && !text_.empty()) {
			text_.pop_back();
			fb.play(hui::audio::Cue::tick);
		}
		if (in.is_pressed(hui::Action::north) && text_.size() < 120) {
			text_ += ' ';
			fb.play(hui::audio::Cue::tick);
		}
		if (in.is_pressed(hui::Action::page_next)) {
			kbPage_ = kbPage_ == 1 ? 0 : 1;
			buildKeys();
			fb.play(hui::audio::Cue::tab);
		}
		if (in.is_pressed(hui::Action::page_prev)) {
			kbPage_ = kbPage_ == 2 ? 0 : 2;
			buildKeys();
			fb.play(hui::audio::Cue::tab);
		}
		if (in.is_pressed(hui::Action::l3) && ask_ == Ask::password) {
			showText_ = !showText_;
		}
		if (in.nav != hui::Direction::none) {
			const auto &row = rows_[static_cast<size_t>(kbRow_)];
			const int idx = keyAt(kbRow_, kbCol_);
			if (in.nav == hui::Direction::left) {
				kbCol_ = row[static_cast<size_t>(idx > 0 ? idx - 1 : static_cast<int>(row.size()) - 1)].col;
			} else if (in.nav == hui::Direction::right) {
				kbCol_ = row[static_cast<size_t>(idx + 1 < static_cast<int>(row.size()) ? idx + 1 : 0)].col;
			} else if (in.nav == hui::Direction::up) {
				kbRow_ = kbRow_ > 0 ? kbRow_ - 1 : 4;
			} else if (in.nav == hui::Direction::down) {
				kbRow_ = kbRow_ < 4 ? kbRow_ + 1 : 0;
			}
			fb.play(hui::audio::Cue::focus);
		}
		if (!in.is_pressed(hui::Action::confirm)) {
			return;
		}
		const Cell &cell = rows_[static_cast<size_t>(kbRow_)][static_cast<size_t>(keyAt(kbRow_, kbCol_))];
		switch (cell.control) {
		case 0:
			if (text_.size() < 120) {
				text_ += cell.ch;
			}
			fb.play(hui::audio::Cue::tick);
			break;
		case 1:
			kbPage_ = kbPage_ == 1 ? 0 : 1;
			buildKeys();
			break;
		case 2:
			kbPage_ = kbPage_ == 2 ? 0 : 2;
			buildKeys();
			break;
		case 3:
			text_ += ' ';
			fb.play(hui::audio::Cue::tick);
			break;
		case 4:
			if (!text_.empty()) {
				text_.pop_back();
			}
			fb.play(hui::audio::Cue::tick);
			break;
		case 5:
			finishKeyboard(fb);
			break;
		}
	}

	// Moves around the rows, fetches what the cursor is near, and tells the poster worker what to fetch
	template <class Feedback>
	void browse(const hui::InputFrame &in, float dt, Feedback &fb)
	{
		State &s = state();
		if (cancelClock_ > 0.0f) {
			cancelClock_ = std::max(0.0f, cancelClock_ - dt);
			if (cancelClock_ <= 0.0f || in.nav != hui::Direction::none || in.is_pressed(hui::Action::confirm) ||
				in.is_pressed(hui::Action::menu)) {
				cancelClock_ = 0.0f; // the moment passed, or the cursor moved on: nothing is cancelled
				std::lock_guard<std::mutex> g(s.m);
				s.message.clear();
			}
		}
		if (in.is_pressed(hui::Action::back) && cancellableSelection()) {
			// CIRCLE on a title that was requested: cancel the request (asked twice, so a slip does no harm)
			long long id = 0;
			std::string type, title;
			bool mine = false;
			{
				std::lock_guard<std::mutex> g(s.m);
				const Row &row = s.rows[static_cast<size_t>(selRow_)];
				const Card &c = row.cards[static_cast<size_t>(selCol_[static_cast<size_t>(selRow_)])];
				id = c.id;
				type = c.type;
				title = c.title;
				mine = row.mine;
			}
			if (cancelClock_ > 0.0f && cancelId_ == id && cancelType_ == type) {
				cancelClock_ = 0.0f;
				fb.play(hui::audio::Cue::open);
				{
					std::lock_guard<std::mutex> g(s.m);
					for (Row &row : s.rows) {
						for (Card &c : row.cards) {
							if (c.id == id && c.type == type) {
								c.asking = true;
							}
						}
					}
				}
				startCancel(id, type, title, mine);
			} else {
				cancelId_ = id;
				cancelType_ = type;
				cancelClock_ = 5.0f;
				fb.play(hui::audio::Cue::focus);
				std::lock_guard<std::mutex> g(s.m);
				s.message = "Cancel the request for " + clipText(title, 40) + "?  Press Circle again";
			}
			return;
		}
		if (in.is_pressed(hui::Action::back)) {
			fb.play(hui::audio::Cue::back);
			bool atBase = false;
			if (fromLibrary_) {
				std::lock_guard<std::mutex> g(s.m);
				atBase = s.stack.size() <= 1;
			}
			if (atBase) { // the cast page a film page asked for: Back goes to the film
				leaveAll();
				fromLibrary_ = false;
				::jf::discoverLeave() = true;
				return;
			}
			if (!leavePage()) { // from a cast or actor page, one page back; from Discover, the tabs
				wantTabs_ = true;
			}
			return;
		}
		if (selCol_.size() < static_cast<size_t>(rowCount_)) {
			selCol_.resize(static_cast<size_t>(rowCount_), 0);
		}
		if (colScroll_.size() < static_cast<size_t>(rowCount_)) {
			colScroll_.resize(static_cast<size_t>(rowCount_), 0.0f);
		}
		selRow_ = std::max(0, std::min(selRow_, rowCount_ - 1));
		if (in.nav == hui::Direction::down && selRow_ + 1 < rowCount_) {
			selRow_++;
			fb.play(hui::audio::Cue::focus);
		} else if (in.nav == hui::Direction::up && selRow_ > 0) {
			selRow_--;
			fb.play(hui::audio::Cue::focus);
		} else if (in.nav == hui::Direction::up && !in.nav_repeat) {
			wantTabs_ = true; // above the first row: the tabs
			fb.play(hui::audio::Cue::focus);
		}
		int cards = 0, uid = 0, pages = 1, page = 0, loading = 0;
		long long selId = 0;
		bool canAsk = false;
		std::string selType, selTitle;
		{
			std::lock_guard<std::mutex> g(s.m);
			if (selRow_ < static_cast<int>(s.rows.size())) {
				Row &row = s.rows[static_cast<size_t>(selRow_)];
				cards = static_cast<int>(row.cards.size());
				uid = row.uid;
				pages = row.pages;
				page = row.page;
				loading = row.loading;
			}
		}
		int &col = selCol_[static_cast<size_t>(selRow_)];
		const int step = in.is_pressed(hui::Action::page_next) ? 7 : in.is_pressed(hui::Action::page_prev) ? -7 : 0;
		if (in.nav == hui::Direction::right && col + 1 < cards) {
			col++;
			fb.play(hui::audio::Cue::focus);
		} else if (in.nav == hui::Direction::left && col > 0) {
			col--;
			fb.play(hui::audio::Cue::focus);
		} else if (step != 0 && cards > 0) {
			col = std::max(0, std::min(cards - 1, col + step));
			fb.play(hui::audio::Cue::tab);
		}
		col = std::max(0, std::min(col, std::max(0, cards - 1)));
		// near the end of a row: the next page
		if (cards > 0 && col + 6 >= cards && page < pages && !loading) {
			startRow(uid);
		}
		// the rows around the cursor get loaded
		{
			std::vector<int> uids;
			{
				std::lock_guard<std::mutex> g(s.m);
				for (int r = selRow_ - 1; r <= selRow_ + 2; r++) {
					if (r >= 0 && r < static_cast<int>(s.rows.size()) && s.rows[static_cast<size_t>(r)].page == 0 &&
						!s.rows[static_cast<size_t>(r)].loading && !s.rows[static_cast<size_t>(r)].failed) {
						uids.push_back(s.rows[static_cast<size_t>(r)].uid);
					}
				}
			}
			for (int u : uids) {
				startRow(u);
			}
		}
		// smooth scrolling
		const float k = std::min(1.0f, dt * 12.0f);
		rowScroll_ += (static_cast<float>(selRow_) - rowScroll_) * k;
		for (int r = 0; r < rowCount_; r++) {
			const int c = selCol_[static_cast<size_t>(r)];
			const float target = std::max(0.0f, static_cast<float>(c) - 6.0f);
			colScroll_[static_cast<size_t>(r)] += (target - colScroll_[static_cast<size_t>(r)]) * k;
		}
		// which posters to fetch, and which to give back
		evictClock_ += dt;
		wantPosters();
		{
			std::lock_guard<std::mutex> g(s.m);
			if (selRow_ < static_cast<int>(s.rows.size()) && col < static_cast<int>(s.rows[static_cast<size_t>(selRow_)].cards.size())) {
				const Card &c = s.rows[static_cast<size_t>(selRow_)].cards[static_cast<size_t>(col)];
				selId = c.id;
				want_[0] = c.ar;
				want_[1] = c.ag;
				want_[2] = c.ab;
				canAsk = c.status < 2 && !c.asking && !s.rows[static_cast<size_t>(selRow_)].mine && c.type != "person";
				selType = c.type;
				selTitle = c.title;
			}
			if (evictClock_ > 0.5f) {
				evictClock_ = 0.0f;
				int live = 0;
				for (const Row &row : s.rows) {
					for (const Card &c : row.cards) {
						live += c.tex ? 1 : 0;
					}
				}
				if (live > kMaxTextures) {
					// give back the posters of rows nowhere near the cursor first, then those far along in this one
					for (int r = 0; r < static_cast<int>(s.rows.size()) && live > kMaxTextures - 40; r++) {
						Row &row = s.rows[static_cast<size_t>(r)];
						for (int i = 0; i < static_cast<int>(row.cards.size()) && live > kMaxTextures - 40; i++) {
							const bool near = std::abs(r - selRow_) <= 2 && r < static_cast<int>(selCol_.size()) &&
								std::abs(i - selCol_[static_cast<size_t>(r)]) <= 24;
							Card &c = row.cards[static_cast<size_t>(i)];
							if (c.tex && !near) {
								if (destroy_) {
									destroy_(c.tex);
								}
								c.tex = 0;
								c.texState = 0;
								live--;
							}
						}
					}
				}
			}
		}
		if (in.is_pressed(hui::Action::west)) {
			fb.play(hui::audio::Cue::open);
			leaveAll(); // a search is a row of Discover
			openKeyboard(Ask::search, "");
			return;
		}
		if (in.is_pressed(hui::Action::north)) {
			// disconnect: forget the cookie and ask again
			leaveAll();
			writeText("/data/jellyzoom-seerr-cookie", "");
			{
				std::lock_guard<std::mutex> g(s.m);
				s.cookie.clear();
			}
			fb.play(hui::audio::Cue::back);
			beginConnect();
			return;
		}
		if (in.is_pressed(hui::Action::menu) && cards > 0 && (selType == "movie" || selType == "tv")) {
			fb.play(hui::audio::Cue::open);
			openCast(selId, selType, selTitle); // OPTIONS: the cast of the title under the cursor
			return;
		}
		if (in.is_pressed(hui::Action::confirm) && cards > 0 && selType == "person") {
			fb.play(hui::audio::Cue::open);
			openPerson(selId, selTitle); // an actor: their films and series
			return;
		}
		if (in.is_pressed(hui::Action::confirm) && cards > 0) {
			if (canAsk) {
				fb.play(hui::audio::Cue::open);
				startRequest(uid, selId);
			} else {
				fb.play(hui::audio::Cue::error);
			}
		}
	}

	static const char *statusWord(const Card &c)
	{
		if (c.asking) {
			return "Asking ...";
		}
		if (c.status >= 5) {
			return "In your library";
		}
		if (c.requestState == 3) {
			return "Declined";
		}
		if (c.status == 4) {
			return "Partly available";
		}
		if (c.status == 3) {
			return "Processing";
		}
		if (c.status == 2) {
			return c.requestState == 1 ? "Awaiting approval" : "Requested";
		}
		return nullptr;
	}

	void drawRows(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts) const
	{
		using hui::gfx::Align;
		using hui::gfx::Color;
		using hui::gfx::Rect;
		State &s = state();
		std::lock_guard<std::mutex> g(s.m);
		const Color white{ 1, 1, 1, 1 };
		const Color ink{ 0.043f, 0.051f, 0.086f, 1.0f };
		const Color gold{ 1.0f, 0.82f, 0.4f, 1.0f };
		const int rows = static_cast<int>(s.rows.size());
		list.push_clip(Rect{ 0.0f, 108.0f, 1920.0f, 664.0f });
		for (int r = 0; r < rows; r++) {
			const float top = 128.0f + (static_cast<float>(r) - rowScroll_) * kRowH;
			if (top > 780.0f || top < -kRowH) {
				continue;
			}
			const Row &row = s.rows[static_cast<size_t>(r)];
			const bool active = r == selRow_;
			const float near = std::max(0.0f, 1.0f - std::fabs(static_cast<float>(r) - rowScroll_));
			const float dim = 0.5f + 0.5f * near;
			std::string label = row.name;
			for (char &c : label) {
				c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
			}
			hui::ui::text(list, fonts.semibold, label.c_str(), 90.0f, top + 26.0f, 18, white.with_alpha(0.6f * dim), Align::left, 4.0f);
			if (row.cards.empty()) {
				const char *line = row.loading ? "Loading ..." : row.failed ? "Could not load this row" : row.page == 0 ? "" : row.note.c_str();
				hui::ui::text(list, fonts.regular, line, 90.0f, top + 170.0f, 26, white.with_alpha(0.5f * dim));
				continue;
			}
			const float slide = r < static_cast<int>(colScroll_.size()) ? colScroll_[static_cast<size_t>(r)] : 0.0f;
			for (int i = 0; i < static_cast<int>(row.cards.size()); i++) {
				const float x = 90.0f + (static_cast<float>(i) - slide) * kPitch;
				if (x > 1930.0f || x < -kCardW) {
					continue;
				}
				const Card &c = row.cards[static_cast<size_t>(i)];
				const bool on = active && r < static_cast<int>(selCol_.size()) && i == selCol_[static_cast<size_t>(r)];
				const float grow = on ? 10.0f : 0.0f;
				const Rect box{ x - grow, top + 52.0f - grow - (on ? 6.0f : 0.0f), kCardW + 2.0f * grow, kCardH + 2.0f * grow };
				const float radius = 18.0f;
				const float light = on ? 1.0f : (0.45f + 0.4f * near);
				const Color accent{ c.ar, c.ag, c.ab, 1.0f };
				list.shadow(Rect{ box.x, box.y + 14.0f, box.w, box.h }, radius, on ? 40.0f : 26.0f, Color{ 0, 0, 0, 0.55f });
				if (on) {
					list.glow(box.inset(18.0f), radius, 70.0f, accent.with_alpha(0.28f + 0.1f * std::sin(clock_ * 2.0f)));
				}
				if (c.tex) {
					list.image(c.tex, box, hui::gfx::kFullUv, Color{ light, light, light, 1.0f }, radius);
				} else {
					list.rounded_rect(box, radius, Color{ 0.12f * light + 0.03f, 0.14f * light + 0.03f, 0.22f * light + 0.05f, 1.0f });
					std::string t = c.title.size() > 20 ? c.title.substr(0, 18) + ".." : c.title;
					hui::ui::text(list, fonts.regular, t.c_str(), box.cx(), box.y + box.h * 0.5f, 20, white.with_alpha(0.6f), Align::center);
				}
				list.bordered_rect(box, radius, Color{ 0, 0, 0, 0 }, on ? 2.5f : 1.5f, white.with_alpha(on ? 0.85f : 0.16f * light));
				if (c.status >= 5 && !c.asking && c.requestState != 3 && c.type != "person") {
					// a green tick on the corner: it is in your library
					const float tx = box.x + 32.0f, ty = box.y + 32.0f;
					list.circle(tx, ty, 21.0f, ink.with_alpha(0.55f * light));
					list.circle(tx, ty, 18.0f, Color{ 0.14f, 0.66f, 0.36f, 0.4f + 0.6f * light });
					for (int k = 0; k <= 5; k++) {
						list.circle(tx - 8.0f + static_cast<float>(k), ty + 0.5f + 1.1f * static_cast<float>(k), 2.4f, white);
					}
					for (int k = 1; k <= 10; k++) {
						list.circle(tx - 3.0f + 1.1f * static_cast<float>(k), ty + 6.0f - 1.2f * static_cast<float>(k), 2.4f, white);
					}
				} else if (const char *word = statusWord(c)) {
					// a pill on the corner, like the wheel's NEW and WATCHED tags
					const bool lib = c.status >= 5;
					const bool declined = c.requestState == 3;
					const int size = 15;
					std::string w = word;
					for (char &ch : w) {
						ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
					}
					const float width = fonts.semibold.measure(w.c_str(), size) + 24.0f;
					const Rect pill{ box.x + 12.0f, box.y + 12.0f, width, 30.0f };
					list.rounded_rect(pill, 15.0f, ink.with_alpha(0.8f * light));
					hui::ui::text(list, fonts.semibold, w.c_str(), pill.x + 12.0f, pill.cy() + size * 0.36f, size,
						declined ? Color{ 1.0f, 0.55f, 0.5f, 1.0f } : lib ? white : gold);
				}
			}
		}
		list.pop_clip();
		// the caption under the rows: the title large, then the facts, then a little of the story
		if (selRow_ >= 0 && selRow_ < rows && selRow_ < static_cast<int>(selCol_.size())) {
			const Row &row = s.rows[static_cast<size_t>(selRow_)];
			const int col = selCol_[static_cast<size_t>(selRow_)];
			if (col >= 0 && col < static_cast<int>(row.cards.size())) {
				const Card &c = row.cards[static_cast<size_t>(col)];
				hui::ui::text(list, fonts.display, fonts.display.font->fit(c.title.c_str(), 60, 1300.0f), 960.0f, 836.0f, 60, white,
					Align::center);
				const bool person = c.type == "person";
				std::string facts = person ? "Cast" : c.type == "tv" ? "Series" : "Movie";
				if (!c.year.empty()) {
					facts += "  \xC2\xB7  " + c.year;
				}
				std::string role;
				if (!c.character.empty()) {
					role = "as " + c.character;
				}
				if (person) {
					facts += "  \xC2\xB7  Select to see their TV/Film credits";
				} else {
					if (const char *word = statusWord(c)) {
						facts += std::string("  \xC2\xB7  ") + word;
					} else if (!row.mine) {
						facts += "  \xC2\xB7  Press Cross to request";
					}
					facts += "  \xC2\xB7  Options: cast";
				}
				if (role.empty()) {
					hui::ui::text(list, fonts.regular, facts.c_str(), 960.0f, 884.0f, 26, white.with_alpha(0.74f), Align::center);
				} else {
					// the role on a line of its own, larger and brighter than the facts under it
					hui::ui::text(list, fonts.semibold, fonts.semibold.font->fit(role.c_str(), 34, 1500.0f), 960.0f, 880.0f, 34,
						white.with_alpha(0.92f), Align::center);
					hui::ui::text(list, fonts.regular, facts.c_str(), 960.0f, 914.0f, 24, white.with_alpha(0.74f), Align::center);
				}
				const std::string &o = c.overview;
				const size_t cut = 118;
				size_t at = o.size() > cut ? o.rfind(' ', cut) : o.size();
				if (at == std::string::npos || at == 0) {
					at = std::min(o.size(), cut);
				}
				const std::string first = o.substr(0, at);
				std::string second = at < o.size() ? o.substr(at + (o[at] == ' ' ? 1 : 0)) : std::string();
				if (second.size() > cut) {
					second = second.substr(0, cut - 3) + "...";
				}
				const bool roleLine = !c.character.empty();
				hui::ui::text(list, fonts.regular, first.c_str(), 960.0f, roleLine ? 952.0f : 930.0f, 22, white.with_alpha(0.55f),
					Align::center);
				if (!roleLine) {
					hui::ui::text(list, fonts.regular, second.c_str(), 960.0f, 962.0f, 22, white.with_alpha(0.55f), Align::center);
				}
			}
		}
	}

	// The title under the cursor was requested and its request can be cancelled (not available yet, not declined)
	bool cancellableSelection() const
	{
		State &s = state();
		std::lock_guard<std::mutex> g(s.m);
		if (selRow_ < 0 || selRow_ >= static_cast<int>(s.rows.size()) || selRow_ >= static_cast<int>(selCol_.size())) {
			return false;
		}
		const Row &row = s.rows[static_cast<size_t>(selRow_)];
		const int col = selCol_[static_cast<size_t>(selRow_)];
		if (col < 0 || col >= static_cast<int>(row.cards.size())) {
			return false;
		}
		const Card &c = row.cards[static_cast<size_t>(col)];
		if ((c.type != "movie" && c.type != "tv") || c.asking) {
			return false;
		}
		return row.mine ? (c.requestState == 1 || c.requestState == 2) && c.status < 5 : (c.status == 2 || c.status == 3);
	}

	// The button legend along the bottom: it follows what is under the cursor (an actor is Selected, a title is Requested)
	void drawLegend(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts) const
	{
		State &s = state();
		std::string type;
		bool canRequest = false, inPage = false;
		{
			std::lock_guard<std::mutex> g(s.m);
			inPage = !s.stack.empty();
			if (selRow_ >= 0 && selRow_ < static_cast<int>(s.rows.size()) && selRow_ < static_cast<int>(selCol_.size())) {
				const Row &row = s.rows[static_cast<size_t>(selRow_)];
				const int col = selCol_[static_cast<size_t>(selRow_)];
				if (col >= 0 && col < static_cast<int>(row.cards.size())) {
					const Card &c = row.cards[static_cast<size_t>(col)];
					type = c.type;
					canRequest = !row.mine && !statusWord(c);
				}
			}
		}
		hui::ui::Hint hints[6];
		int n = 0;
		hints[n++] = { hui::ui::Button::dpad, "Choose" };
		if (type == "person") {
			hints[n++] = { hui::ui::Button::cross, "Select" };
		} else if (canRequest) {
			hints[n++] = { hui::ui::Button::cross, "Request" };
		}
		if (type == "movie" || type == "tv") {
			hints[n++] = { hui::ui::Button::options, "Cast" };
		}
		if (!inPage) {
			hints[n++] = { hui::ui::Button::square, "Search" };
			hints[n++] = { hui::ui::Button::triangle, "Disconnect" };
		}
		hints[n++] = { hui::ui::Button::circle, cancellableSelection() ? "Cancel request" : inPage ? "Back" : "Tabs" };
		hui::ui::draw_hints(list, fonts, hui::ui::GlyphStyle::dark(), hints, n, 1824, true);
	}

	void drawKeyboard(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts) const
	{
		using hui::gfx::Align;
		using hui::gfx::Color;
		using hui::gfx::Rect;
		const std::string label = ask_ == Ask::address ? "Address, like 192.168.1.10:5055 or https://seerr.mydomain.com"
			: ask_ == Ask::password ? "Password for " + user_ + " (your Jellyfin password)" : "Search for a title";
		hui::ui::text(list, fonts.regular, label.c_str(), 960.0f, 300.0f, 28, Color{ 1, 1, 1, 0.75f }, Align::center);
		const Rect field{ 440.0f, 350.0f, 1040.0f, 84.0f };
		list.bordered_rect(field, 18.0f, Color{ 1, 1, 1, 0.07f }, 1.5f, Color{ 1, 1, 1, 0.3f });
		std::string shown = text_;
		if (!showText_) {
			shown.assign(text_.size(), '*');
		}
		if (shown.size() > 34) {
			shown = shown.substr(shown.size() - 34);
		}
		hui::ui::text(list, fonts.semibold, shown.c_str(), field.x + 32.0f, field.y + 58.0f, 42, Color{ 1, 1, 1, 1 });
		const float width = shown.empty() ? 0.0f : fonts.semibold.measure(shown.c_str(), 42);
		const float blink = 0.5f + 0.5f * std::sin(clock_ * 7.0f);
		list.rounded_rect(Rect{ field.x + 36.0f + width, field.y + 20.0f, 3.0f, 44.0f }, 1.5f, Color{ 1, 1, 1, 0.35f + 0.55f * blink });
		const float kw = 96.0f, kh = 84.0f, gap = 8.0f;
		const float x0 = 960.0f - (10.0f * (kw + gap) - gap) * 0.5f;
		const float y0 = 470.0f;
		for (int r = 0; r < 5; r++) {
			for (const Cell &c : rows_[static_cast<size_t>(r)]) {
				const Rect box{ x0 + c.col * (kw + gap), y0 + r * (kh + gap), c.span * kw + (c.span - 1) * gap, kh };
				const bool on = r == kbRow_ && &c == &rows_[static_cast<size_t>(r)][static_cast<size_t>(keyAt(kbRow_, kbCol_))];
				const bool lit = c.control == 5;
				list.bordered_rect(box, 16.0f,
					on ? Color{ 0.30f, 0.34f, 0.62f, 0.98f } : lit ? Color{ 0.67f, 0.36f, 0.76f, 0.55f } : Color{ 1, 1, 1, 0.09f },
					on ? 2.5f : 1.0f, Color{ 1, 1, 1, on ? 0.9f : 0.14f });
				hui::ui::text(list, fonts.semibold, c.label.c_str(), box.cx(), box.y + 56.0f, c.control ? 28 : 36,
					Color{ 1, 1, 1, on ? 1.0f : 0.85f }, Align::center);
			}
		}
		const hui::ui::Hint hints[] = { { hui::ui::Button::cross, "Type" }, { hui::ui::Button::square, "Delete" },
			{ hui::ui::Button::triangle, "Space" }, { hui::ui::Button::l1, "Symbols" }, { hui::ui::Button::r1, "Capitals" },
			{ hui::ui::Button::circle, "Back" } };
		hui::ui::draw_hints(list, fonts, hui::ui::GlyphStyle::dark(), hints, 6, 1824, true);
	}
};

} // namespace jfseerr
