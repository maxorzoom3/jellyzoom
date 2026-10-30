/* Which config a build uses.
 *
 * An ordinary (development) build reads ps5/src/jf/config.h, the file that holds your own server and login.
 * It is never committed (see .gitignore); ps5/src/jf/config.h.example shows its shape.
 *
 * A release build (ps5/tools/release.sh, which defines JF_RELEASE) never reads config.h at all: no server, no
 * login and no device id of yours can reach the program. The device id is made on the console at first start
 * (jf_identity.c), so every tester is a device of their own on their own Jellyfin server.
 */
#pragma once

#ifdef JF_RELEASE
#ifdef __cplusplus
extern "C" {
#endif
const char *jf_device_id(void);
#ifdef __cplusplus
}
#endif
#define JF_SERVER_HOST ""
#define JF_SERVER_PORT 8096
#define JF_SERVER_TLS 0
#define JF_USERNAME ""
#define JF_PASSWORD ""
#define JF_CLIENT_NAME "Jellyzoom"
#define JF_DEVICE_NAME "PlayStation 5"
#ifdef JF_APP_VERSION
#define JF_CLIENT_VERSION JF_APP_VERSION
#else
#define JF_CLIENT_VERSION "0"
#endif
#define JF_DEVICE_ID jf_device_id()
#else
#include "config.h"
#endif
