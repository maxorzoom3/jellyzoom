/* getaddrinfo for numeric IPv4 hosts and ports: the title only talks to an IP address,
 * and the console's own getaddrinfo fails even for those. Logs to klog (NET lines). */
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "platform.h"

static int parse_ipv4(const char *s, unsigned char out[4])
{
	unsigned v[4];
	int used = 0;
	if (sscanf(s, "%u.%u.%u.%u%n", &v[0], &v[1], &v[2], &v[3], &used) != 4 || s[used] != 0) {
		return 0;
	}
	for (int i = 0; i < 4; i++) {
		if (v[i] > 255) {
			return 0;
		}
		out[i] = (unsigned char)v[i];
	}
	return 1;
}

int getaddrinfo(const char *host, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
	say("NET: getaddrinfo host=%s service=%s family=%d socktype=%d flags=0x%x", host ? host : "(null)",
		service ? service : "(null)", hints ? hints->ai_family : -1, hints ? hints->ai_socktype : -1,
		hints ? (unsigned)hints->ai_flags : 0u);
	if (hints && hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET) {
		say("NET: refused, family %d is not IPv4", hints->ai_family);
		return EAI_FAMILY;
	}
	unsigned char ip[4] = { 127, 0, 0, 1 };
	if (!host || !*host) {
		if (hints && (hints->ai_flags & AI_PASSIVE)) {
			memset(ip, 0, sizeof(ip));
		}
	} else if (!parse_ipv4(host, ip)) {
		say("NET: refused, \"%s\" is not a dotted IPv4 address", host);
		return EAI_NONAME;
	}
	unsigned long port = 0;
	if (service && *service) {
		char *end = 0;
		port = strtoul(service, &end, 10);
		if (*end || port > 65535) {
			say("NET: refused, service \"%s\" is not a port number", service);
			return EAI_SERVICE;
		}
	}
	struct addrinfo *ai = (struct addrinfo *)calloc(1, sizeof(*ai) + sizeof(struct sockaddr_in));
	if (!ai) {
		return EAI_MEMORY;
	}
	struct sockaddr_in *sin = (struct sockaddr_in *)(ai + 1);
	sin->sin_len = sizeof(*sin);
	sin->sin_family = AF_INET;
	sin->sin_port = (unsigned short)(((port & 0xff) << 8) | ((port >> 8) & 0xff));
	memcpy(&sin->sin_addr, ip, 4);
	ai->ai_family = AF_INET;
	ai->ai_socktype = (hints && hints->ai_socktype) ? hints->ai_socktype : SOCK_STREAM;
	ai->ai_protocol = hints ? hints->ai_protocol : 0;
	ai->ai_addrlen = sizeof(*sin);
	ai->ai_addr = (struct sockaddr *)sin;
	*res = ai;
	say("NET: resolved to %u.%u.%u.%u:%lu", ip[0], ip[1], ip[2], ip[3], port);
	return 0;
}

void freeaddrinfo(struct addrinfo *ai)
{
	while (ai) {
		struct addrinfo *next = ai->ai_next;
		free(ai);
		ai = next;
	}
}
