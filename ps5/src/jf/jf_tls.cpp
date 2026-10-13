// Tunnel for names and https. See jf_tls.h. The rest of the program talks plain http to 127.0.0.1; the tunnel finds the
// address of the name and, for https, does the TLS (mbedTLS: TLS 1.2, the server's certificate and name are checked).
//   /data/jf-dns        a DNS server to ask (an address). When it is there, no other server is asked: your DNS decides.
//                       Without it: the router (192.168.1.1), then 1.1.1.1 and 8.8.8.8
//   /data/jf-hosts      lines "address name": names answered without asking anyone (a name that points at your own network)
//   /data/jf-ca.pem     extra trusted certificates (PEM), besides Mozilla's roots built into the program (read at start)
//   /data/jf-tls-insecure  (any content) skips the certificate check: for a self-signed certificate (read at start)
// Built without JF_HAVE_MBEDTLS, https fails with a clear message and names work over plain http, as before.
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
#include <memory>
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

#if defined(JF_HAVE_MBEDTLS)
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#include <mbedtls/platform_time.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include "jf_ca_bundle.inc"
#endif

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

std::string readBig(const std::string &path, size_t limit)
{
	std::string out;
	if (FILE *f = std::fopen(path.c_str(), "rb")) {
		char buf[4096];
		size_t n;
		while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && out.size() < limit) {
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
		servers.push_back(chosen); // the one you chose, and only it: your DNS may block names on purpose
	} else {
		servers.push_back("192.168.1.1");
		servers.push_back("1.1.1.1");
		servers.push_back("8.8.8.8");
	}
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

#if defined(JF_HAVE_MBEDTLS)

struct TlsShared {
	std::mutex lock;
	bool tried{ false };
	bool ok{ false };
	std::string why;
	bool insecure{ false };
	mbedtls_ssl_config conf;
	mbedtls_x509_crt roots;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context drbg;
	std::mutex rngLock;
};
TlsShared shared;

std::string mbedText(int code)
{
	char buf[160];
	mbedtls_strerror(code, buf, sizeof(buf));
	return std::string(buf) + " (-0x" + [&] {
		char h[16];
		std::snprintf(h, sizeof(h), "%04x", static_cast<unsigned>(-code));
		return std::string(h);
	}() + ")";
}

int randomBytes(void *, unsigned char *out, size_t len)
{
	std::lock_guard<std::mutex> g(shared.rngLock);
	return mbedtls_ctr_drbg_random(&shared.drbg, out, len);
}

// Starts the random generator and loads the trusted roots, once
bool tlsPrepare(std::string &why)
{
	std::lock_guard<std::mutex> g(shared.lock);
	if (shared.tried) {
		why = shared.why;
		return shared.ok;
	}
	shared.tried = true;
	mbedtls_ssl_config_init(&shared.conf);
	mbedtls_x509_crt_init(&shared.roots);
	mbedtls_entropy_init(&shared.entropy);
	mbedtls_ctr_drbg_init(&shared.drbg);
	static const char personal[] = "jellyfin-wheel-tls";
	int r = mbedtls_ctr_drbg_seed(&shared.drbg, mbedtls_entropy_func, &shared.entropy,
		reinterpret_cast<const unsigned char *>(personal), sizeof(personal) - 1);
	if (r != 0) {
		shared.why = "the random number generator would not start: " + mbedText(r);
		why = shared.why;
		return false;
	}
	const int skipped = mbedtls_x509_crt_parse(&shared.roots, reinterpret_cast<const unsigned char *>(kJfCaBundle), sizeof(kJfCaBundle));
	if (skipped < 0) {
		shared.why = "the built-in trusted certificates could not be read: " + mbedText(skipped);
		why = shared.why;
		return false;
	}
	int builtin = 0;
	for (const mbedtls_x509_crt *c = &shared.roots; c && c->version != 0; c = c->next) {
		builtin++;
	}
	const std::string extra = readBig(std::string(JF_TLS_DATA) + "/jf-ca.pem", 1 << 20);
	int added = 0;
	if (!extra.empty()) {
		std::string pem = extra;
		pem.push_back('\0');
		const int bad = mbedtls_x509_crt_parse(&shared.roots, reinterpret_cast<const unsigned char *>(pem.data()), pem.size());
		if (bad < 0) {
			say("/data/jf-ca.pem could not be read (%s): ignored", mbedText(bad).c_str());
		} else {
			for (const mbedtls_x509_crt *c = &shared.roots; c && c->version != 0; c = c->next) {
				added++;
			}
			added -= builtin;
		}
	}
	shared.insecure = fileExists(std::string(JF_TLS_DATA) + "/jf-tls-insecure");
	r = mbedtls_ssl_config_defaults(&shared.conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
	if (r != 0) {
		shared.why = "TLS could not be set up: " + mbedText(r);
		why = shared.why;
		return false;
	}
	mbedtls_ssl_conf_min_tls_version(&shared.conf, MBEDTLS_SSL_VERSION_TLS1_2);
	mbedtls_ssl_conf_rng(&shared.conf, randomBytes, nullptr);
	mbedtls_ssl_conf_ca_chain(&shared.conf, &shared.roots, nullptr);
	mbedtls_ssl_conf_authmode(&shared.conf, shared.insecure ? MBEDTLS_SSL_VERIFY_NONE : MBEDTLS_SSL_VERIFY_REQUIRED);
	say("TLS ready: %d trusted roots (%d skipped), %d from /data/jf-ca.pem%s", builtin, skipped, added,
		shared.insecure ? "; CERTIFICATE CHECK IS OFF (/data/jf-tls-insecure)" : "");
	shared.ok = true;
	return true;
}

struct TlsConn {
	mbedtls_ssl_context ssl;
	int fd{ -1 };
	bool started{ false };
	std::string out; // the bytes being written: kept as they are until they are all written (mbedTLS asks for the same call again)
	TlsConn() { mbedtls_ssl_init(&ssl); }
	~TlsConn()
	{
		mbedtls_ssl_free(&ssl);
	}
};

int bioSend(void *ctx, const unsigned char *buf, size_t len)
{
	const int fd = *static_cast<int *>(ctx);
	const ssize_t n = ::send(fd, buf, len, MSG_NOSIGNAL);
	if (n >= 0) {
		return static_cast<int>(n);
	}
	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
		return MBEDTLS_ERR_SSL_WANT_WRITE;
	}
	return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

int bioRecv(void *ctx, unsigned char *buf, size_t len)
{
	const int fd = *static_cast<int *>(ctx);
	const ssize_t n = ::recv(fd, buf, len, 0);
	if (n > 0) {
		return static_cast<int>(n);
	}
	if (n == 0) {
		return 0; // the other end closed
	}
	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
		return MBEDTLS_ERR_SSL_WANT_READ;
	}
	return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

std::string certProblem(uint32_t flags, const std::string &name)
{
	std::string text;
	if (flags & MBEDTLS_X509_BADCERT_EXPIRED) {
		const mbedtls_time_t now = static_cast<mbedtls_time_t>(std::time(nullptr));
		tm parts;
		std::memset(&parts, 0, sizeof(parts));
		mbedtls_platform_gmtime_r(&now, &parts);
		char when[40];
		std::snprintf(when, sizeof(when), "%04d-%02d-%02d", parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday);
		text = "the certificate has expired, or the console's clock is wrong (it thinks today is " + std::string(when) + ")";
	} else if (flags & MBEDTLS_X509_BADCERT_FUTURE) {
		text = "the certificate is not valid yet: the console's clock may be wrong";
	} else if (flags & MBEDTLS_X509_BADCERT_CN_MISMATCH) {
		text = "the certificate is for a different name than " + name;
	} else if (flags & (MBEDTLS_X509_BADCERT_NOT_TRUSTED | MBEDTLS_X509_BADCERT_BAD_KEY | MBEDTLS_X509_BADCERT_BAD_MD | MBEDTLS_X509_BADCERT_BAD_PK)) {
		text = "the certificate is not trusted (a self-signed one? put its CA in /data/jf-ca.pem, or create /data/jf-tls-insecure to skip the check)";
	} else {
		char buf[256];
		mbedtls_x509_crt_verify_info(buf, sizeof(buf), "", flags);
		text = std::string("the certificate was refused: ") + buf;
	}
	return text;
}

// The handshake, on a blocking socket with a time limit. Afterwards the socket is non-blocking.
bool tlsConnect(TlsConn &c, int fd, const std::string &name, std::string &why)
{
	c.fd = fd;
	int r = mbedtls_ssl_setup(&c.ssl, &shared.conf);
	if (r == 0) {
		r = mbedtls_ssl_set_hostname(&c.ssl, name.c_str());
	}
	if (r != 0) {
		why = "TLS could not start: " + mbedText(r);
		return false;
	}
	mbedtls_ssl_set_bio(&c.ssl, &c.fd, bioSend, bioRecv, nullptr);
	timeval tv;
	tv.tv_sec = 5;
	tv.tv_usec = 0;
	::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	for (;;) {
		r = mbedtls_ssl_handshake(&c.ssl);
		if (r == 0) {
			break;
		}
		if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
			if (std::chrono::steady_clock::now() > limit) {
				why = "the server did not answer the TLS handshake";
				return false;
			}
			continue;
		}
		if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
			why = certProblem(mbedtls_ssl_get_verify_result(&c.ssl), name);
		} else if (r == MBEDTLS_ERR_SSL_CONN_EOF) {
			why = "the server closed the connection during the TLS handshake (is that port really https?)";
		} else {
			why = "the TLS handshake failed: " + mbedText(r);
		}
		return false;
	}
	tv.tv_sec = 0;
	::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
	say("%s: %s, %s%s", name.c_str(), mbedtls_ssl_get_version(&c.ssl), mbedtls_ssl_get_ciphersuite(&c.ssl),
		shared.insecure ? " (certificate not checked)" : "");
	return true;
}

#endif // JF_HAVE_MBEDTLS

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
#if defined(JF_HAVE_MBEDTLS)
	std::unique_ptr<TlsConn> tc;
	if (tls) {
		std::string why;
		if (!tlsPrepare(why)) {
			setError("https is not available: " + why);
			::close(ufd);
			::close(cfd);
			return;
		}
		tc.reset(new TlsConn());
		if (!tlsConnect(*tc, ufd, name, why)) {
			setError("Could not make a safe connection to " + name + ": " + why);
			tc.reset();
			::close(ufd);
			::close(cfd);
			return;
		}
	}
#else
	if (tls) {
		setError("https is not built into this version of Jellyfin Wheel: use http://" + name + ":" + std::to_string(port) + " or an address like 192.168.1.10:8096");
		::close(ufd);
		::close(cfd);
		return;
	}
#endif
	::fcntl(cfd, F_SETFL, ::fcntl(cfd, F_GETFL, 0) | O_NONBLOCK);
	::fcntl(ufd, F_SETFL, ::fcntl(ufd, F_GETFL, 0) | O_NONBLOCK);
	Rewriter rw;
	rw.vhost = name + (((tls && port != 443) || (!tls && port != 80)) ? ":" + std::to_string(port) : std::string());
	std::string toServer, toClient;
	char buf[16384];
	bool serverDone = false;
	// the server side: plain sockets, or the TLS session on top of the socket
	// read: > 0 bytes, 0 the server closed, -1 nothing now, -2 broken
	auto upRead = [&](char *b, size_t cap) -> ssize_t {
#if defined(JF_HAVE_MBEDTLS)
		if (tc) {
			for (int tries = 0; tries < 4; tries++) {
				const int r = mbedtls_ssl_read(&tc->ssl, reinterpret_cast<unsigned char *>(b), cap);
				if (r > 0) {
					return r;
				}
				if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == MBEDTLS_ERR_SSL_CONN_EOF) {
					return 0;
				}
				if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
					return -1;
				}
				if (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
					continue;
				}
				return -2;
			}
			return -1;
		}
#endif
		const ssize_t n = ::recv(ufd, b, cap, 0);
		if (n > 0) {
			return n;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
			return -1;
		}
		return n == 0 ? 0 : -2;
	};
	// write what is waiting in `pending`; false if the connection is broken
	auto upWrite = [&](std::string &pending) -> bool {
#if defined(JF_HAVE_MBEDTLS)
		if (tc) {
			for (;;) {
				if (tc->out.empty()) {
					if (pending.empty()) {
						return true;
					}
					const size_t k = std::min<size_t>(pending.size(), 16384);
					tc->out.assign(pending, 0, k);
					pending.erase(0, k);
				}
				const int r = mbedtls_ssl_write(&tc->ssl, reinterpret_cast<const unsigned char *>(tc->out.data()), tc->out.size());
				if (r > 0) {
					tc->out.erase(0, static_cast<size_t>(r));
					continue;
				}
				if (r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_WANT_READ) {
					return true;
				}
				return false;
			}
		}
#endif
		return sendAll(ufd, pending);
	};
	for (;;) {
		bool upOut = !toServer.empty();
		bool buffered = false;
#if defined(JF_HAVE_MBEDTLS)
		if (tc) {
			upOut = upOut || !tc->out.empty();
			// bytes the TLS layer already holds are not on the socket any more
			buffered = !serverDone && toClient.size() < 2097152 &&
				(mbedtls_ssl_check_pending(&tc->ssl) != 0 || mbedtls_ssl_get_bytes_avail(&tc->ssl) > 0);
		}
#endif
		pollfd p[2];
		p[0].fd = cfd;
		p[0].events = static_cast<short>((toServer.size() < 1048576 ? POLLIN : 0) | (toClient.empty() ? 0 : POLLOUT));
		p[0].revents = 0;
		p[1].fd = ufd;
		p[1].events = static_cast<short>((toClient.size() < 2097152 && !serverDone ? POLLIN : 0) | (upOut ? POLLOUT : 0));
		p[1].revents = 0;
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
		if (!toServer.empty()
#if defined(JF_HAVE_MBEDTLS)
			|| (tc && !tc->out.empty())
#endif
		) {
			if (!upWrite(toServer)) {
				break;
			}
		}
		// the server -> the program
		if ((p[1].revents & (POLLIN | POLLHUP | POLLERR)) || buffered) {
			for (int round = 0; round < 8 && !serverDone && toClient.size() < 2097152; round++) {
				const ssize_t n = upRead(buf, sizeof(buf));
				if (n > 0) {
					toClient.append(buf, static_cast<size_t>(n));
					continue;
				}
				if (n == -1) {
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
#if defined(JF_HAVE_MBEDTLS)
	if (tc) {
		mbedtls_ssl_close_notify(&tc->ssl); // a courtesy: if the socket is full it does not matter
	}
#endif
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

#if defined(JF_HAVE_MBEDTLS)
// ---- what mbedTLS asks the platform for ----
//
// Time as a calendar date, for the validity dates in certificates (the library's own would call the console's libc)
extern "C" struct tm *mbedtls_platform_gmtime_r(const mbedtls_time_t *tt, struct tm *out)
{
	long long t = static_cast<long long>(*tt);
	long long days = t / 86400;
	long long rem = t % 86400;
	if (rem < 0) {
		rem += 86400;
		days--;
	}
	out->tm_hour = static_cast<int>(rem / 3600);
	out->tm_min = static_cast<int>((rem % 3600) / 60);
	out->tm_sec = static_cast<int>(rem % 60);
	// days since 1970-01-01 -> year, month, day (the proleptic Gregorian calendar)
	const long long z = days + 719468;
	const long long era = (z >= 0 ? z : z - 146096) / 146097;
	const unsigned doe = static_cast<unsigned>(z - era * 146097);
	const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	long long year = static_cast<long long>(yoe) + era * 400;
	const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const unsigned mp = (5 * doy + 2) / 153;
	const unsigned day = doy - (153 * mp + 2) / 5 + 1;
	const unsigned month = mp < 10 ? mp + 3 : mp - 9;
	if (month <= 2) {
		year++;
	}
	out->tm_year = static_cast<int>(year - 1900);
	out->tm_mon = static_cast<int>(month) - 1;
	out->tm_mday = static_cast<int>(day);
	long long wd = (days + 4) % 7; // 1970-01-01 was a Thursday
	if (wd < 0) {
		wd += 7;
	}
	out->tm_wday = static_cast<int>(wd);
	static const int before[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
	const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
	out->tm_yday = before[month - 1] + static_cast<int>(day) - 1 + (leap && month > 2 ? 1 : 0);
	out->tm_isdst = 0;
	return out;
}

namespace {

// Where the random bytes come from: the console's libc has arc4random_buf (the SDK's stubs export it; a symbol they do
// not export, getentropy for one, stops the link).
extern "C" void arc4random_buf(void *, size_t);

const char *fillRandom(unsigned char *out, size_t len)
{
	arc4random_buf(out, len);
	return "arc4random_buf";
}

} // namespace

extern "C" int mbedtls_hardware_poll(void *, unsigned char *out, size_t len, size_t *olen)
{
	const char *how = fillRandom(out, len);
	static std::atomic<bool> told{ false };
	if (!told.exchange(true)) {
		std::fprintf(stderr, "JFTLS: random bytes from %s\n", how);
		std::fflush(stderr);
	}
	*olen = len;
	return 0;
}
#endif // JF_HAVE_MBEDTLS
