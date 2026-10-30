#include "../src/jf/jf_gate.cpp"

#include <cassert>

static std::vector<int> statuses;
static std::vector<std::string> requests;
static bool decodeFails = false;

extern "C" void say(const char *, ...) {}

extern "C" const char *jf_device_id(void)
{
	return "test-device";
}

extern "C" int jf_tls_open(const char *, int, int, int *)
{
	return -1;
}

extern "C" int http_get_binary(const char *, int, const char *path, const char *, HttpBinaryResponse *out)
{
	requests.emplace_back(path);
	assert(requests.size() <= statuses.size());
	const int status = statuses[requests.size() - 1];
	if (status < 0)
		return -1;
	out->status = status;
	if (status == 200) {
		out->data = static_cast<unsigned char *>(std::malloc(1));
		assert(out->data);
		out->data[0] = 1;
		out->size = 1;
	}
	return 0;
}

extern "C" void http_binary_response_free(HttpBinaryResponse *response)
{
	std::free(response->data);
	std::memset(response, 0, sizeof(*response));
}

extern "C" stbi_uc *stbi_load_from_memory(const stbi_uc *, int, int *x, int *y, int *channels, int)
{
	if (decodeFails)
		return nullptr;
	*x = *y = 1;
	*channels = 4;
	return static_cast<stbi_uc *>(std::calloc(4, 1));
}

extern "C" const char *stbi_failure_reason(void)
{
	return "test decode failure";
}

static Entry entry(const char *json, int kind)
{
	json_object *object = json_tokener_parse(json);
	assert(object);
	Entry result;
	parseEntry(result, object, kind);
	json_object_put(object);
	return result;
}

static bool fetch(const Entry &item, std::initializer_list<int> replies)
{
	while (!ready.empty()) {
		std::free(ready.front().rgba);
		ready.pop_front();
	}
	cats.clear();
	cats.emplace_back(new Category());
	cats[0]->generation = 3;
	cats[0]->entries.push_back(item);
	statuses = replies;
	requests.clear();
	Params p;
	p.generation = 3;
	JfSession session{};
	return fetchPoster(session, 0, 0, p);
}

static bool requested(const std::string &id, size_t index)
{
	return requests.size() > index && requests[index].find("/Items/" + id + "/Images/Primary?") == 0;
}

int main()
{
	const char *episode = R"({"Id":"ep","Name":"Episode","Type":"Episode","SeasonId":"season2",
		"SeriesId":"series","SeriesName":"The Show","SeriesPrimaryImageTag":"showtag",
		"ImageTags":{"Primary":"eptag"},"ParentIndexNumber":2,"IndexNumber":3})";
	for (int kind : { kResume, kNextUp }) {
		const Entry e = entry(episode, kind);
		assert(e.id == "ep" && e.name == "The Show" && e.genre == "S2E3  Episode");
		assert(e.imageId == "season2" && e.imageTag == "season");
		assert(e.fallbackImageId == "series" && e.fallbackImageTag == "showtag");
		assert(fetch(e, { 200 }));
		assert(requests.size() == 1 && requested("season2", 0) && ready.size() == 1);
		assert(fetch(e, { 404, 200 }));
		assert(requests.size() == 2 && requested("season2", 0) && requested("series", 1) && ready.size() == 1);
		assert(fetch(e, { 404, 404 }) == false);
		assert(requests.size() == 2 && ready.empty());
		assert(fetch(e, { 500 }) == false);
		assert(requests.size() == 1 && ready.empty());
		assert(fetch(e, { -1 }) == false);
		assert(requests.size() == 1 && ready.empty());
		decodeFails = true;
		assert(fetch(e, { 200 }) == false);
		assert(requests.size() == 1 && ready.empty());
		decodeFails = false;
	}
	const Entry browse = entry(episode, kBrowse);
	assert(browse.imageId == "season2" && browse.fallbackImageId.empty() && browse.name == "Episode");
	const Entry queued = entry(episode, kQueue);
	assert(queued.imageId == "series" && queued.fallbackImageId.empty());
	const Entry library = entry(episode, kLibrary);
	assert(library.imageId == "series" && library.fallbackImageId.empty());

	const Entry noSeason = entry(R"({"Id":"ep","Name":"Episode","Type":"Episode","SeriesId":"series",
		"SeriesPrimaryImageTag":"showtag","ImageTags":{"Primary":"eptag"}})", kResume);
	assert(noSeason.imageId == "series" && noSeason.fallbackImageId.empty());
	assert(fetch(noSeason, { 200 }) && requests.size() == 1 && requested("series", 0));
	const Entry noSeriesImage = entry(R"({"Id":"ep","Name":"Episode","Type":"Episode","SeasonId":"season1",
		"SeriesId":"series","ImageTags":{"Primary":"eptag"}})", kNextUp);
	assert(noSeriesImage.fallbackImageId == "ep" && noSeriesImage.fallbackImageTag == "eptag");
	assert(fetch(noSeriesImage, { 404, 200 }) && requested("ep", 1));
	const Entry noFallback = entry(R"({"Id":"ep","Name":"Episode","Type":"Episode","SeasonId":"season1"})", kResume);
	assert(fetch(noFallback, { 404 }) == false && requests.size() == 1);
	const Entry movie = entry(R"({"Id":"movie","Name":"Movie","Type":"Movie",
		"ImageTags":{"Primary":"movietag"}})", kResume);
	assert(movie.imageId == "movie" && movie.imageTag == "movietag" && movie.fallbackImageId.empty());
	assert(fetch(movie, { 200 }) && requests.size() == 1 && requested("movie", 0));
	while (!ready.empty()) {
		std::free(ready.front().rgba);
		ready.pop_front();
	}
}
