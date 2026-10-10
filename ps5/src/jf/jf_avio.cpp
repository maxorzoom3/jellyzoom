/* Plain-socket HTTP source for FFmpeg. Every log line starts with AVIO. */
#include "jf_avio.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

extern "C" {
#include "platform.h"
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

namespace {
constexpr int bufferSize = 256 * 1024;
constexpr int64_t skipLimit = 512 * 1024; // a forward seek this short is read through, not reconnected

struct Source {
	std::string host, path, auth;
	int port{ 0 };
	std::atomic<bool> *stop{ nullptr };
	int fd{ -1 };
	int64_t size{ -1 };
	int64_t pos{ 0 };           // the offset of the next byte FFmpeg will be given
	std::vector<uint8_t> lead;  // body bytes that arrived with the headers
	size_t leadOff{ 0 };
	int connects{ 0 };
};

bool sendAll(int fd, const char *data, size_t n)
{
	while (n > 0) {
		ssize_t w = send(fd, data, n, 0);
		if (w <= 0) {
			if (w < 0 && errno == EINTR) {
				continue;
			}
			return false;
		}
		data += w;
		n -= static_cast<size_t>(w);
	}
	return true;
}

void hangUp(Source *s)
{
	if (s->fd >= 0) {
		close(s->fd);
		s->fd = -1;
	}
	s->lead.clear();
	s->leadOff = 0;
}

// header value, case-insensitive name match at the start of a line; -1 if absent
bool headerValue(const std::string &headers, const char *name, std::string &out)
{
	const size_t nameLen = std::strlen(name);
	size_t at = 0;
	while (at < headers.size()) {
		size_t end = headers.find("\r\n", at);
		if (end == std::string::npos) {
			end = headers.size();
		}
		if (end - at > nameLen + 1 && headers[at + nameLen] == ':') {
			bool same = true;
			for (size_t i = 0; i < nameLen; i++) {
				char a = headers[at + i], b = name[i];
				if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + 32);
				if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + 32);
				if (a != b) {
					same = false;
					break;
				}
			}
			if (same) {
				size_t v = at + nameLen + 1;
				while (v < end && headers[v] == ' ') v++;
				out = headers.substr(v, end - v);
				return true;
			}
		}
		at = end + 2;
	}
	return false;
}

bool connectAt(Source *s, int64_t offset)
{
	hangUp(s);
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		say("AVIO: socket() failed, errno %d", errno);
		return false;
	}
	sockaddr_in addr;
	std::memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(static_cast<uint16_t>(s->port));
	if (inet_pton(AF_INET, s->host.c_str(), &addr.sin_addr) != 1 ||
		connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
		say("AVIO: connect to %s:%d failed, errno %d", s->host.c_str(), s->port, errno);
		close(fd);
		return false;
	}
	timeval tv;
	tv.tv_sec = 5;
	tv.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	char req[6144];
	int n = std::snprintf(req, sizeof(req),
		"GET %s HTTP/1.1\r\nHost: %s:%d\r\n%sRange: bytes=%lld-\r\nAccept: */*\r\nConnection: close\r\n\r\n",
		s->path.c_str(), s->host.c_str(), s->port, s->auth.c_str(), static_cast<long long>(offset));
	if (n <= 0 || n >= static_cast<int>(sizeof(req)) || !sendAll(fd, req, static_cast<size_t>(n))) {
		say("AVIO: request send failed");
		close(fd);
		return false;
	}

	std::string head;
	char chunk[2048];
	size_t headEnd = std::string::npos;
	while (headEnd == std::string::npos && head.size() < 16384) {
		ssize_t got = recv(fd, chunk, sizeof(chunk), 0);
		if (got <= 0) {
			say("AVIO: connection ended before the response headers (recv %zd, errno %d)", got, errno);
			close(fd);
			return false;
		}
		head.append(chunk, static_cast<size_t>(got));
		headEnd = head.find("\r\n\r\n");
	}
	if (headEnd == std::string::npos) {
		say("AVIO: response headers too long");
		close(fd);
		return false;
	}
	int status = 0;
	std::sscanf(head.c_str(), "HTTP/%*s %d", &status);
	std::string headers = head.substr(0, headEnd);
	if (offset > 0 ? status != 206 : (status != 200 && status != 206)) {
		say("AVIO: HTTP %d for offset %lld", status, static_cast<long long>(offset));
		close(fd);
		return false;
	}
	std::string value;
	if (headerValue(headers, "content-range", value)) {
		long long a = 0, b = 0, total = 0;
		if (std::sscanf(value.c_str(), "bytes %lld-%lld/%lld", &a, &b, &total) == 3 && total > 0) {
			s->size = total;
		}
	} else if (offset == 0 && headerValue(headers, "content-length", value)) {
		s->size = std::atoll(value.c_str());
	}
	s->fd = fd;
	s->lead.assign(head.begin() + static_cast<long>(headEnd) + 4, head.end());
	s->leadOff = 0;
	s->connects++;
	if (s->connects <= 12) {
		say("AVIO: connected at offset %lld, HTTP %d, size %lld", static_cast<long long>(offset), status,
			static_cast<long long>(s->size));
	}
	return true;
}

int readRaw(Source *s, uint8_t *buf, int size)
{
	int failures = 0;
	for (;;) {
		if (s->stop && s->stop->load()) {
			return AVERROR_EXIT;
		}
		if (s->size >= 0 && s->pos >= s->size) {
			return AVERROR_EOF;
		}
		if (s->fd < 0 && !connectAt(s, s->pos)) {
			if (++failures >= 3) {
				return AVERROR(EIO);
			}
			continue;
		}
		if (s->leadOff < s->lead.size()) {
			size_t n = s->lead.size() - s->leadOff;
			if (n > static_cast<size_t>(size)) {
				n = static_cast<size_t>(size);
			}
			std::memcpy(buf, s->lead.data() + s->leadOff, n);
			s->leadOff += n;
			s->pos += static_cast<int64_t>(n);
			return static_cast<int>(n);
		}
		ssize_t got = recv(s->fd, buf, static_cast<size_t>(size), 0);
		if (got > 0) {
			s->pos += got;
			return static_cast<int>(got);
		}
		if (got < 0 && errno == EINTR) {
			continue;
		}
		// closed early, or timed out: reconnect at the same offset
		say("AVIO: read at %lld ended (recv %zd, errno %d), reconnecting", static_cast<long long>(s->pos), got, errno);
		hangUp(s);
		if (++failures >= 3) {
			return AVERROR(EIO);
		}
	}
}

int readCb(void *opaque, uint8_t *buf, int size)
{
	return readRaw(static_cast<Source *>(opaque), buf, size);
}

int64_t seekCb(void *opaque, int64_t offset, int whence)
{
	Source *s = static_cast<Source *>(opaque);
	if (whence & AVSEEK_SIZE) {
		return s->size;
	}
	whence &= ~AVSEEK_FORCE;
	int64_t target;
	if (whence == SEEK_SET) {
		target = offset;
	} else if (whence == SEEK_CUR) {
		target = s->pos + offset;
	} else if (whence == SEEK_END && s->size >= 0) {
		target = s->size + offset;
	} else {
		return AVERROR(EINVAL);
	}
	if (target < 0) {
		return AVERROR(EINVAL);
	}
	if (target == s->pos) {
		return target;
	}
	if (s->fd >= 0 && target > s->pos && target - s->pos <= skipLimit) {
		uint8_t scratch[16384];
		while (s->pos < target) {
			int want = static_cast<int>(target - s->pos < static_cast<int64_t>(sizeof(scratch)) ? target - s->pos
																								 : static_cast<int64_t>(sizeof(scratch)));
			int got = readRaw(s, scratch, want);
			if (got <= 0) {
				return got < 0 ? got : AVERROR(EIO);
			}
		}
		return target;
	}
	hangUp(s);
	s->pos = target;
	return target;
}
} // namespace

AVIOContext *jf_avio_open(const char *host, int port, const char *path, const char *authHeader,
	std::atomic<bool> *stop, int64_t *sizeOut)
{
	Source *s = new Source();
	s->host = host;
	s->port = port;
	s->path = path;
	s->auth = authHeader ? authHeader : "";
	s->stop = stop;
	if (!connectAt(s, 0)) {
		delete s;
		return nullptr;
	}
	uint8_t *buffer = static_cast<uint8_t *>(av_malloc(bufferSize));
	AVIOContext *io = buffer ? avio_alloc_context(buffer, bufferSize, 0, s, readCb, nullptr, seekCb) : nullptr;
	if (!io) {
		av_free(buffer);
		hangUp(s);
		delete s;
		return nullptr;
	}
	if (sizeOut) {
		*sizeOut = s->size;
	}
	return io;
}

void jf_avio_close(AVIOContext **ctx)
{
	if (!ctx || !*ctx) {
		return;
	}
	Source *s = static_cast<Source *>((*ctx)->opaque);
	hangUp(s);
	delete s;
	av_freep(&(*ctx)->buffer);
	avio_context_free(ctx);
}
