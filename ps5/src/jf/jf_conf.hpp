// Runtime server and login settings. Files in /data win; config.h values are the fallback.
//   /data/jf-server   "host" or "host:port"
//   /data/jf-user     user name
//   /data/jf-pass     password (optional, only if you put it there yourself: the program never writes it, and keeps only
//                     the sign-in token in /data/jf-token. A plain-text password is best deleted once you are signed in)
#pragma once
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

#include <arpa/inet.h>

#include "jf_tls.h"

namespace jfconf {

struct Values {
	std::string host;
	int port{ 8096 };
	std::string user;
	std::string pass;
	std::string effHost; // where the program really connects: the address itself, or the local tunnel for a name or https
	int effPort{ 0 };
	std::string effKey;
};

inline std::string readFile(const char *path)
{
	std::string out;
	if (FILE *f = std::fopen(path, "rb")) {
		char buf[256];
		size_t n;
		while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && out.size() < 512) {
			out.append(buf, n);
		}
		std::fclose(f);
	}
	while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) {
		out.pop_back();
	}
	return out;
}

inline bool writeFile(const char *path, const std::string &text)
{
	FILE *f = std::fopen(path, "wb");
	if (!f) {
		return false;
	}
	const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
	std::fclose(f);
	return ok;
}

inline std::mutex &lockRef()
{
	static std::mutex m;
	return m;
}

// "192.168.1.10:8096", "name:8096", "https://name", "https://name:8920", "http://name". A bare address keeps defaultPort.
inline void parseAddress(const std::string &text, int defaultPort, std::string *host, int *port)
{
	std::string t = text;
	std::string scheme;
	if (t.rfind("https://", 0) == 0) {
		scheme = "https://";
		t = t.substr(8);
	} else if (t.rfind("http://", 0) == 0) {
		scheme = "http://";
		t = t.substr(7);
	}
	while (!t.empty() && t.back() == '/') {
		t.pop_back();
	}
	int p = scheme == "https://" ? 443 : scheme == "http://" ? 80 : defaultPort;
	const size_t colon = t.rfind(':');
	if (colon != std::string::npos && colon + 1 < t.size()) {
		const int q = std::atoi(t.c_str() + colon + 1);
		t = t.substr(0, colon);
		if (q > 0 && q < 65536) {
			p = q;
		}
	}
	*host = scheme + t;
	*port = p;
}

// What to connect to for this server: an address as it is; a name or https through the local tunnel (jf_tls.cpp)
// True for a plain IPv4 address like 192.168.1.10 (no socket headers needed)
inline bool isDottedQuad(const std::string &s)
{
	int parts = 0, digits = 0, value = 0;
	for (std::size_t i = 0; i <= s.size(); ++i) {
		const char c = i < s.size() ? s[i] : '.';
		if (c >= '0' && c <= '9') {
			value = value * 10 + (c - '0');
			if (++digits > 3 || value > 255) {
				return false;
			}
		} else if (c == '.' && digits > 0) {
			++parts;
			digits = 0;
			value = 0;
		} else {
			return false;
		}
	}
	return parts == 4;
}

inline void route(const std::string &host, int port, std::string *outHost, int *outPort)
{
	bool tls = false;
	std::string name = host;
	if (name.rfind("https://", 0) == 0) {
		tls = true;
		name = name.substr(8);
	} else if (name.rfind("http://", 0) == 0) {
		name = name.substr(7);
	}
	if (!tls && isDottedQuad(name)) {
		*outHost = name;
		*outPort = port;
		return;
	}
	int local = 0;
	if (!name.empty() && jf_tls_open(name.c_str(), port, tls ? 1 : 0, &local) == 0) {
		*outHost = "127.0.0.1";
		*outPort = local;
		return;
	}
	*outHost = name;
	*outPort = port;
}

// The server as a person writes it
inline std::string serverTextOf(const std::string &host, int port)
{
	const bool https = host.rfind("https://", 0) == 0;
	const bool http = host.rfind("http://", 0) == 0;
	if ((https && port == 443) || (http && port == 80)) {
		return host;
	}
	return host + ":" + std::to_string(port);
}

inline Values &store(const char *defHost, int defPort, const char *defUser, const char *defPass)
{
	static Values v;
	static bool loaded = false;
	if (!loaded) {
		loaded = true;
		v.host = defHost ? defHost : "";
		v.port = defPort;
		v.user = defUser ? defUser : "";
		v.pass = defPass ? defPass : "";
		const std::string server = readFile("/data/jf-server");
		if (!server.empty()) {
			parseAddress(server, defPort, &v.host, &v.port);
		}
		const std::string u = readFile("/data/jf-user");
		if (!u.empty()) {
			v.user = u;
		}
		const std::string p = readFile("/data/jf-pass");
		if (!p.empty()) {
			v.pass = p;
		}
	}
	return v;
}

} // namespace jfconf

// config.h must be included before this header.
// A release build carries no server or login: jf_config.h gives it empty ones and never reads config.h
#ifdef JF_SERVER_HOST
#define JFC_DEF_HOST JF_SERVER_HOST
#else
#define JFC_DEF_HOST ""
#endif
#ifdef JF_SERVER_PORT
#define JFC_DEF_PORT JF_SERVER_PORT
#else
#define JFC_DEF_PORT 8096
#endif
#ifdef JF_USERNAME
#define JFC_DEF_USER JF_USERNAME
#else
#define JFC_DEF_USER ""
#endif
#ifdef JF_PASSWORD
#define JFC_DEF_PASS JF_PASSWORD
#else
#define JFC_DEF_PASS ""
#endif

namespace jfconf {
inline Values &now() { return store(JFC_DEF_HOST, JFC_DEF_PORT, JFC_DEF_USER, JFC_DEF_PASS); }
inline Values &effective()
{
	Values &v = now();
	std::lock_guard<std::mutex> g(lockRef());
	const std::string key = v.host + "|" + std::to_string(v.port);
	if (v.effKey != key) {
		route(v.host, v.port, &v.effHost, &v.effPort);
		v.effKey = key;
	}
	return v;
}
inline const char *host() { return effective().effHost.c_str(); }
inline int port() { return effective().effPort; }
inline std::string serverText() { return serverTextOf(now().host, now().port); }
inline const char *user() { return now().user.c_str(); }
inline const char *pass() { return now().pass.c_str(); }
inline bool configured() { return !now().host.empty(); }

// The sign-in flow found the server: use it from now on.
inline void setServer(const std::string &host, int port)
{
	Values &v = now();
	v.host = host;
	v.port = port;
}

// Used by the setup screen: change the values and keep them across restarts.
inline bool save(const std::string &host, int port, const std::string &user, const std::string &pass)
{
	Values &v = now();
	v.host = host;
	v.port = port;
	v.user = user;
	v.pass = pass;
	bool ok = writeFile("/data/jf-server", serverTextOf(host, port));
	ok = writeFile("/data/jf-user", user) && ok;
	// the password is never written: the sign-in token (jf-token) is what is kept
	return ok;
}
} // namespace jfconf
