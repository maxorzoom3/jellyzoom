/* A local tunnel for servers that are not a plain IP address: names (the console cannot resolve them, so the tunnel does)
 * and https (the tunnel does the TLS). The rest of the program talks plain HTTP to 127.0.0.1:<local_port>. */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
/* Opens (or finds) the tunnel to name:port. tls 1 for https. Returns 0 and the local port, or -1. */
int jf_tls_open(const char *name, int port, int tls, int *local_port);
/* 1 when /data/jellyzoom-tls-insecure exists: the certificate check is OFF (for a self-signed server), 0 when it is on. */
int jf_tls_insecure(void);
/* The last thing that went wrong in a tunnel, in words ("" if nothing did). */
void jf_tls_error(char *buf, int size);
#ifdef __cplusplus
}
#endif
