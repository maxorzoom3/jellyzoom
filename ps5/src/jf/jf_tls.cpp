// Tunnel for names (plain http). See jf_tls.h. https is not part of this build: a request for it fails with a clear message.
//   /data/jf-dns        a DNS server to ask (an address); otherwise the router (192.168.1.1), then 1.1.1.1 and 8.8.8.8
//   /data/jf-hosts      lines "address name": names answered without asking anyone (a name that points at your own network)
//   /data/jf-ca.pem     extra trusted certificates (PEM), besides the bundle built into the program
//   /data/jf-tls-insecure  (any content) skips the certificate check: for a self-signed certificate
#include "jf_tls.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>


#ifndef JF_TLS_DATA
#define JF_TLS_DATA "/data"
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace {

std::mutex errLock;
std::string lastError;

void say(const char *fmt, ...)
{
	char line[400];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	std::fprintf(stderr, "JFTLS: %s\n", line);
	std::fflush(stderr);
}

void setError(const std::string &text)
{
	{
		std::lock_guard<std::mutex> g(errLock);
		lastError = text;
	}
	say("%s", text.c_str());
}

std::string readSmall(const std::string &path)
{
	std::string out;
	if (FILE *f = std::fopen(path.c_str(), "rb")) {
		char buf[1024];
		size_t n;
		while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && out.size() < 8192) {
			out.append(buf, n);
		}
		std::fclose(f);
	}
	return out;
}

bool fileExists(const std::string &path)
{
	if (FILE *f = std::fopen(path.c_str(), "rb")) {
		std::fclose(f);
		return true;
	}
	return false;
}

std::string trim(std::string s)
{
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
		s.pop_back();
	}
	size_t a = 0;
	while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) {
		a++;
	}
	return s.substr(a);
}

std::string lowerCase(std::string s)
{
	for (char &c : s) {
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c - 'A' + 'a');
		}
	}
	return s;
}

// ---- finding an address for a name ----

struct Cached {
	std::string name;
	in_addr ip;
	std::time_t when;
};
std::mutex dnsLock;
std::vector<Cached> dnsCache;

bool hostsFile(const std::string &name, in_addr *out)
{
	const std::string text = readSmall(std::string(JF_TLS_DATA) + "/jf-hosts");
	size_t at = 0;
	while (at < text.size()) {
		size_t eol = text.find('\n', at);
		if (eol == std::string::npos) {
			eol = text.size();
		}
		std::string line = trim(text.substr(at, eol - at));
		at = eol + 1;
		if (line.empty() || line[0] == '#') {
			continue;
		}
		const size_t sp = line.find_first_of(" \t");
		if (sp == std::string::npos) {
			continue;
		}
		const std::string addr = line.substr(0, sp);
		std::string rest = trim(line.substr(sp));
		in_addr ip;
		if (inet_pton(AF_INET, addr.c_str(), &ip) != 1) {
			continue;
		}
		size_t pos = 0;
		while (pos < rest.size()) {
			size_t e = rest.find_first_of(" \t", pos);
			if (e == std::string::npos) {
				e = rest.size();
			}
			if (lowerCase(rest.substr(pos, e - pos)) == name) {
				*out = ip;
				return true;
			}
			pos = rest.find_first_not_of(" \t", e);
			if (pos == std::string::npos) {
				break;
			}
		}
	}
	return false;
}

// Asks one DNS server for the A record of name
bool dnsAsk(const char *server, const std::string &name, in_addr *out)
{
	in_addr srv;
	if (inet_pton(AF_INET, server, &srv) != 1) {
		return false;
	}
	std::vector<unsigned char> q;
	const unsigned id = static_cast<unsigned>(std::rand()) & 0xffff;
	q.push_back(static_cast<unsigned char>(id >> 8));
	q.push_back(static_cast<unsigned char>(id & 255));
	const unsigned char head[10] = { 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0 };
	q.insert(q.end(), head, head + 10);
	size_t at = 0;
	while (at <= name.size()) {
		size_t dot = name.find('.', at);
		if (dot == std::string::npos) {
			dot = name.size();
		}
		const size_t len = dot - at;
		if (len == 0 || len > 63) {
			if (dot >= name.size()) {
				break;
			}
			return false;
		}
		q.push_back(static_cast<unsigned char>(len));
		q.insert(q.end(), name.begin() + static_cast<long>(at), name.begin() + static_cast<long>(dot));
		at = dot + 1;
		if (dot >= name.size()) {
			break;
		}
	}
	q.push_back(0);
	q.push_back(0);
	q.push_back(1); // type A
	q.push_back(0);
	q.push_back(1); // class IN
	const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		return false;
	}
	sockaddr_in sa;
	std::memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(53);
	sa.sin_addr = srv;
	bool ok = false;
	if (::sendto(fd, q.data(), q.size(), 0, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) == static_cast<ssize_t>(q.size())) {
		pollfd p;
		p.fd = fd;
		p.events = POLLIN;
		p.revents = 0;
		if (::poll(&p, 1, 2000) == 1) {
			unsigned char r[1500];
			const ssize_t n = ::recv(fd, r, sizeof(r), 0);
			if (n >= 12 && r[0] == q[0] && r[1] == q[1] && (r[3] & 15) == 0) {
				size_t pos = 12;
				auto skipName = [&](size_t &i) {
					while (i < static_cast<size_t>(n)) {
						const unsigned char b = r[i];
						if (b == 0) {
							i++;
							return;
						}
						if ((b & 0xC0) == 0xC0) {
							i += 2;
							return;
						}
						i += static_cast<size_t>(b) + 1;
					}
				};
				const int qd = (r[4] << 8) | r[5];
				const int an = (r[6] << 8) | r[7];
				for (int i = 0; i < qd; i++) {
					skipName(pos);
					pos += 4;
				}
				for (int i = 0; i < an && pos + 10 <= static_cast<size_t>(n); i++) {
					skipName(pos);
					if (pos + 10 > static_cast<size_t>(n)) {
						break;
					}
					const int type = (r[pos] << 8) | r[pos + 1];
					const int len = (r[pos + 8] << 8) | r[pos + 9];
					pos += 10;
					if (type == 1 && len == 4 && pos + 4 <= static_cast<size_t>(n)) {
						std::memcpy(out, r + pos, 4);
						ok = true;
						break;
					}
					pos += static_cast<size_t>(len);
				}
			}
		}
	}
	::close(fd);
	return ok;
}

bool resolveName(const std::string &rawName, in_addr *out)
{
	const std::string name = lowerCase(rawName);
	if (inet_pton(AF_INET, name.c_str(), out) == 1) {
		return true;
	}
	if (hostsFile(name, out)) {
		return true;
	}
	{
		std::lock_guard<std::mutex> g(dnsLock);
		const std::time_t now = std::time(nullptr);
		for (const Cached &c : dnsCache) {
			if (c.name == name && now - c.when < 300) {
				*out = c.ip;
				return true;
			}
		}
	}
	std::vector<std::string> servers;
	const std::string chosen = trim(readSmall(std::string(JF_TLS_DATA) + "/jf-dns"));
	if (!chosen.empty()) {
		servers.push_back(chosen);
	}
	servers.push_back("192.168.1.1");
	servers.push_back("1.1.1.1");
	servers.push_back("8.8.8.8");
	for (const std::string &s : servers) {
		if (dnsAsk(s.c_str(), name, out)) {
			char text[32];
			inet_ntop(AF_INET, out, text, sizeof(text));
			say("%s is %s (asked %s)", name.c_str(), text, s.c_str());
			std::lock_guard<std::mutex> g(dnsLock);
			dnsCache.push_back({ name, *out, std::time(nullptr) });
			return true;
		}
	}
	return false;
}

// ---- TLS ----

// ---- the tunnel ----

// Rewrites the Host line of every request that passes, so a server that chooses by name sees its own name
struct Rewriter {
	std::string vhost;
	enum { head, body, pass } state{ head };
	std::string buffered;
	long long bodyLeft{ 0 };

	std::string rewriteHead(const std::string &h)
	{
		std::string out;
		bool chunked = false, upgrade = false;
		long long length = 0;
		size_t at = 0;
		bool first = true;
		while (at < h.size()) {
			size_t eol = h.find("\r\n", at);
			if (eol == std::string::npos) {
				eol = h.size();
			}
			std::string line = h.substr(at, eol - at);
			at = eol + 2;
			if (!first && !line.empty()) {
				const std::string low = lowerCase(line);
				if (low.compare(0, 5, "host:") == 0) {
					line = "Host: " + vhost;
				} else if (low.compare(0, 15, "content-length:") == 0) {
					length = std::atoll(line.c_str() + 15);
				} else if (low.compare(0, 18, "transfer-encoding:") == 0 && low.find("chunked") != std::string::npos) {
					chunked = true;
				} else if (low.compare(0, 8, "upgrade:") == 0) {
					upgrade = true;
				}
			}
			first = false;
			out += line;
			out += "\r\n";
		}
		state = (chunked || upgrade) ? pass : length > 0 ? body : head;
		bodyLeft = length;
		return out;
	}

	std::string feed(const char *d, size_t n)
	{
		std::string out;
		size_t i = 0;
		while (i < n) {
			if (state == pass) {
				out.append(d + i, n - i);
				break;
			}
			if (state == body) {
				const size_t k = std::min(static_cast<size_t>(bodyLeft), n - i);
				out.append(d + i, k);
				i += k;
				bodyLeft -= static_cast<long long>(k);
				if (bodyLeft == 0) {
					state = head;
				}
				continue;
			}
			buffered.push_back(d[i++]);
			const size_t b = buffered.size();
			if (b >= 4 && buffered.compare(b - 4, 4, "\r\n\r\n") == 0) {
				out += rewriteHead(buffered);
				buffered.clear();
			} else if (b > 65536) {
				out += buffered;
				buffered.clear();
				state = pass;
			}
		}
		return out;
	}
};

int connectTo(const in_addr &ip, int port, int ms)
{
	const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		return -1;
	}
	::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
	sockaddr_in sa;
	std::memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(static_cast<uint16_t>(port));
	sa.sin_addr = ip;
	bool ok = ::connect(fd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) == 0;
	if (!ok && errno == EINPROGRESS) {
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
	if (!ok) {
		::close(fd);
		return -1;
	}
	::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
	return fd;
}

bool sendAll(int fd, std::string &pending)
{
	while (!pending.empty()) {
		const ssize_t n = ::send(fd, pending.data(), pending.size(), MSG_NOSIGNAL);
		if (n > 0) {
			pending.erase(0, static_cast<size_t>(n));
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
			return true;
		}
		return false;
	}
	return true;
}

void relay(int cfd, const std::string &name, int port, bool tls)
{
	in_addr ip;
	if (!resolveName(name, &ip)) {
		setError("Could not find " + name + " (the console cannot look names up by itself: put the address in /data/jf-hosts, e.g. \"192.168.1.10 " +
			name + "\", or a DNS server in /data/jf-dns)");
		::close(cfd);
		return;
	}
	const int ufd = connectTo(ip, port, 6000);
	if (ufd < 0) {
		char text[32];
		inet_ntop(AF_INET, &ip, text, sizeof(text));
		setError(std::string("Nothing answered at ") + text + ":" + std::to_string(port) + " for " + name);
		::close(cfd);
		return;
	}
	if (tls) {
		setError("https is not built into this version of Jellyfin Wheel: use http://" + name + ":" + std::to_string(port) + " or an address like 192.168.1.10:8096");
		::close(ufd);
		::close(cfd);
		return;
	}
	::fcntl(cfd, F_SETFL, ::fcntl(cfd, F_GETFL, 0) | O_NONBLOCK);
	::fcntl(ufd, F_SETFL, ::fcntl(ufd, F_GETFL, 0) | O_NONBLOCK);
	Rewriter rw;
	rw.vhost = name + (((tls && port != 443) || (!tls && port != 80)) ? ":" + std::to_string(port) : std::string());
	std::string toServer, toClient;
	char buf[16384];
	bool serverDone = false;
	for (;;) {
		pollfd p[2];
		p[0].fd = cfd;
		p[0].events = static_cast<short>((toServer.size() < 1048576 ? POLLIN : 0) | (toClient.empty() ? 0 : POLLOUT));
		p[0].revents = 0;
		p[1].fd = ufd;
		p[1].events = static_cast<short>((toClient.size() < 2097152 && !serverDone ? POLLIN : 0) | (toServer.empty() ? 0 : POLLOUT));
		p[1].revents = 0;
		const bool buffered = false;
		const int ready = ::poll(p, 2, buffered ? 0 : 600000);
		if (ready < 0 && errno != EINTR) {
			break;
		}
		if (ready == 0 && !buffered) {
			break; // ten minutes of nothing
		}
		// the program -> the server
		if (p[0].revents & (POLLIN | POLLHUP | POLLERR)) {
			const ssize_t n = ::recv(cfd, buf, sizeof(buf), 0);
			if (n > 0) {
				toServer += rw.feed(buf, static_cast<size_t>(n));
			} else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
				break;
			}
		}
		if (!toServer.empty()) {
			if (!sendAll(ufd, toServer)) {
				break;
			}
		}
		// the server -> the program
		if ((p[1].revents & (POLLIN | POLLHUP | POLLERR)) || buffered) {
			for (int round = 0; round < 8 && !serverDone && toClient.size() < 2097152; round++) {
				const ssize_t n = ::recv(ufd, buf, sizeof(buf), 0);
				if (n > 0) {
					toClient.append(buf, static_cast<size_t>(n));
					continue;
				}
				if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
					break;
				}
				serverDone = true;
			}
		}
		if (!toClient.empty() && !sendAll(cfd, toClient)) {
			break;
		}
		if (serverDone && toClient.empty()) {
			break;
		}
	}
	::close(ufd);
	::close(cfd);
}

struct Tunnel {
	std::string name;
	int port;
	bool tls;
	int local;
};
std::mutex tunnelLock;
std::vector<Tunnel> tunnels;

void acceptLoop(int listenFd, std::string name, int port, bool tls)
{
	for (;;) {
		const int cfd = ::accept(listenFd, nullptr, nullptr);
		if (cfd < 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			continue;
		}
		std::thread([cfd, name, port, tls]() { relay(cfd, name, port, tls); }).detach();
	}
}

} // namespace

extern "C" int jf_tls_open(const char *name, int port, int tls, int *local_port)
{
	if (!name || !*name || port <= 0 || port > 65535 || !local_port) {
		return -1;
	}
	std::lock_guard<std::mutex> g(tunnelLock);
	const std::string n = lowerCase(name);
	for (const Tunnel &t : tunnels) {
		if (t.name == n && t.port == port && t.tls == (tls != 0)) {
			*local_port = t.local;
			return 0;
		}
	}
	::signal(SIGPIPE, SIG_IGN);
	const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		return -1;
	}
	int one = 1;
	::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in sa;
	std::memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = 0;
	inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
	socklen_t len = sizeof(sa);
	if (::bind(fd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) != 0 || ::listen(fd, 16) != 0 ||
		::getsockname(fd, reinterpret_cast<sockaddr *>(&sa), &len) != 0) {
		::close(fd);
		setError("The tunnel to " + n + " could not open a local port");
		return -1;
	}
	const int local = ntohs(sa.sin_port);
	tunnels.push_back({ n, port, tls != 0, local });
	say("tunnel to %s%s:%d on 127.0.0.1:%d", tls ? "https://" : "", n.c_str(), port, local);
	std::thread(acceptLoop, fd, n, port, tls != 0).detach();
	*local_port = local;
	return 0;
}

extern "C" void jf_tls_error(char *buf, int size)
{
	if (!buf || size <= 0) {
		return;
	}
	std::lock_guard<std::mutex> g(errLock);
	std::snprintf(buf, static_cast<size_t>(size), "%s", lastError.c_str());
}
