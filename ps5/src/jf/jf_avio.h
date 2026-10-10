#pragma once
#include <atomic>
#include <cstdint>
extern "C" {
#include <libavformat/avio.h>
}
/* An AVIOContext that reads one file from the Jellyfin server over plain sockets:
 * connect by IP, GET with a Range header, reconnect on seek. FFmpeg's own HTTP
 * code needs name lookups the console cannot do. Returns null on failure. */
AVIOContext *jf_avio_open(const char *host, int port, const char *path, const char *authHeader,
	std::atomic<bool> *stop, int64_t *sizeOut);
void jf_avio_close(AVIOContext **ctx);
