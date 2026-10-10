/* See jf_backdrop.h. One request at a time, always for the latest wanted title. */
#include "jf_backdrop.h"
#include "jf_gate_private.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

extern "C" {
#include "platform.h"
#include "jf_config.h"
#include "jf_types.h"
#include "http_client.h"
}

// The decoder's implementation is compiled once, in base/VulkanglTFModel.cpp
#include "jf_conf.hpp"
#include "stb_image.h"

namespace {
std::mutex gLock;
std::condition_variable gWake;
std::thread gThread;
bool gStop = false;
int gWanted = -1;
int gDone = -2;      // the title the worker has dealt with (delivered, or the server has no picture for it)
int gDelivered = -2; // the last title whose picture was delivered
bool gHave = false;  // gReady holds a picture
JfBackdrop gReady{};

// Fetches and decodes one title's backdrop; returns false when there is none (or the server did not answer)
bool fetch(int index, JfBackdrop *out)
{
	JfSession session;
	std::memset(&session, 0, sizeof(session));
	if (!jf_gate_session(&session)) {
		return false;
	}
	const std::string id = jf_gate_backdrop_id(index);
	if (id.empty()) {
		return false;
	}
	char auth[4096];
	std::snprintf(auth, sizeof(auth),
		"Authorization: MediaBrowser Client=\"%s\", Device=\"%s\", DeviceId=\"%s\", Version=\"%s\", Token=\"%s\"\r\n",
		JF_CLIENT_NAME, JF_DEVICE_NAME, JF_DEVICE_ID, JF_CLIENT_VERSION, session.access_token);
	char path[256];
	std::snprintf(path, sizeof(path), "/Items/%s/Images/Backdrop/0?maxWidth=1280&quality=78&format=Jpg", id.c_str());
	HttpBinaryResponse response;
	std::memset(&response, 0, sizeof(response));
	if (http_get_binary(jfconf::host(), jfconf::port(), path, auth, &response) != 0) {
		say("JFBACKDROP: request failed");
		return false;
	}
	if (response.status != 200 || !response.data || response.size == 0) {
		say("JFBACKDROP: HTTP %d (no backdrop for this title)", response.status);
		http_binary_response_free(&response);
		return false;
	}
	int w = 0, h = 0, channels = 0;
	unsigned char *pixels = stbi_load_from_memory(response.data, static_cast<int>(response.size), &w, &h, &channels, 4);
	http_binary_response_free(&response);
	if (!pixels) {
		say("JFBACKDROP: would not decode: %s", stbi_failure_reason());
		return false;
	}
	out->gate_index = index;
	out->width = w;
	out->height = h;
	out->rgba = pixels;
	return true;
}

void run()
{
	for (;;) {
		int index;
		{
			std::unique_lock<std::mutex> lock(gLock);
			gWake.wait(lock, [] { return gStop || (gWanted >= 0 && gWanted != gDone); });
			if (gStop) {
				return;
			}
			index = gWanted;
		}
		JfBackdrop got{};
		const bool ok = fetch(index, &got);
		std::unique_lock<std::mutex> lock(gLock);
		if (!ok && !jf_gate_item_id(index)[0]) {
			// not signed in yet, or the list started over: try again in a moment
			lock.unlock();
			std::this_thread::sleep_for(std::chrono::milliseconds(400));
			continue;
		}
		gDone = index;
		if (ok) {
			if (gHave) {
				std::free(gReady.rgba);
			}
			gReady = got;
			gHave = true;
			gDelivered = index;
			say("JFBACKDROP: %dx%d for title %d", got.width, got.height, index);
		}
	}
}
} // namespace

// ---- Stage 35: the title's logo ----
namespace {
std::mutex gLogoLock;
int gLogoWanted = -1;
int gLogoGen = 0;
int gLogoState = 0;
bool gLogoHave = false;
JfBackdrop gLogoReady{};

// 1 got it, 0 the server has none (404 and the like), -1 not signed in or no answer yet: try again
int logoFetch(int index, JfBackdrop *out)
{
	JfSession session;
	std::memset(&session, 0, sizeof(session));
	if (!jf_gate_session(&session)) {
		return -1;
	}
	const std::string id = jf_gate_backdrop_id(index);
	if (id.empty()) {
		return -1;
	}
	char auth[4096];
	std::snprintf(auth, sizeof(auth),
		"Authorization: MediaBrowser Client=\"%s\", Device=\"%s\", DeviceId=\"%s\", Version=\"%s\", Token=\"%s\"\r\n",
		JF_CLIENT_NAME, JF_DEVICE_NAME, JF_DEVICE_ID, JF_CLIENT_VERSION, session.access_token);
	char path[256];
	std::snprintf(path, sizeof(path), "/Items/%s/Images/Logo?maxWidth=800&maxHeight=300&quality=90&format=Png", id.c_str());
	HttpBinaryResponse response;
	std::memset(&response, 0, sizeof(response));
	if (http_get_binary(jfconf::host(), jfconf::port(), path, auth, &response) != 0) {
		return -1;
	}
	if (response.status != 200 || !response.data || response.size == 0) {
		say("JFLOGO: HTTP %d (no logo for this title)", response.status);
		const int status = response.status;
		http_binary_response_free(&response);
		return status >= 500 || status == 0 ? -1 : 0;
	}
	int w = 0, h = 0, channels = 0;
	unsigned char *pixels = stbi_load_from_memory(response.data, static_cast<int>(response.size), &w, &h, &channels, 4);
	http_binary_response_free(&response);
	if (!pixels) {
		say("JFLOGO: would not decode: %s", stbi_failure_reason());
		return 0;
	}
	out->gate_index = index;
	out->width = w;
	out->height = h;
	out->rgba = pixels;
	return 1;
}

void logoRun(int index, int gen)
{
	JfBackdrop got{};
	int result = -1;
	for (int attempt = 0; attempt < 4 && result < 0; attempt++) {
		{
			std::lock_guard<std::mutex> lock(gLogoLock);
			if (gen != gLogoGen) {
				return; // another title was asked for meanwhile
			}
		}
		result = logoFetch(index, &got);
		if (result < 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
		}
	}
	std::lock_guard<std::mutex> lock(gLogoLock);
	if (gen != gLogoGen) {
		if (result == 1) {
			std::free(got.rgba);
		}
		return;
	}
	if (result == 1) {
		if (gLogoHave) {
			std::free(gLogoReady.rgba);
		}
		gLogoReady = got;
		gLogoHave = true;
		gLogoState = 2;
		say("JFLOGO: %dx%d for title %d", got.width, got.height, index);
	} else {
		gLogoState = -1;
	}
}
} // namespace

extern "C" void jf_logo_want(int gate_index)
{
	int gen = 0;
	{
		std::lock_guard<std::mutex> lock(gLogoLock);
		if (gate_index == gLogoWanted) {
			return;
		}
		gLogoWanted = gate_index;
		gen = ++gLogoGen;
		if (gLogoHave) {
			std::free(gLogoReady.rgba);
			gLogoHave = false;
			gLogoReady = JfBackdrop{};
		}
		gLogoState = gate_index < 0 ? 0 : 1;
		if (gate_index < 0) {
			return;
		}
	}
	std::thread(logoRun, gate_index, gen).detach();
}

extern "C" int jf_logo_take(JfBackdrop *out)
{
	std::lock_guard<std::mutex> lock(gLogoLock);
	if (!gLogoHave) {
		return 0;
	}
	*out = gLogoReady;
	gLogoHave = false;
	gLogoReady = JfBackdrop{};
	return 1;
}

extern "C" int jf_logo_state(void)
{
	std::lock_guard<std::mutex> lock(gLogoLock);
	return gLogoState;
}

extern "C" void jf_backdrop_want(int gate_index)
{
	std::lock_guard<std::mutex> lock(gLock);
	if (gate_index == gWanted) {
		return;
	}
	gWanted = gate_index;
	if (gate_index < 0) {
		if (gDone != gDelivered) {
			gDone = -2; // that one failed: asking again later tries again
		}
		return;
	}
	if (!gThread.joinable()) {
		gThread = std::thread(run);
	}
	gWake.notify_all();
}

extern "C" int jf_backdrop_take(JfBackdrop *out)
{
	std::lock_guard<std::mutex> lock(gLock);
	if (!gHave) {
		return 0;
	}
	*out = gReady;
	gHave = false;
	gReady = JfBackdrop{};
	return 1;
}

extern "C" void jf_backdrop_stop(void)
{
	{
		std::lock_guard<std::mutex> lock(gLock);
		gStop = true;
		gWake.notify_all();
	}
	if (gThread.joinable()) {
		gThread.join();
	}
	if (gHave) {
		std::free(gReady.rgba);
		gHave = false;
	}
}
