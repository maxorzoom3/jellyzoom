#include "../src/jf/jellyfin.h"
#include "../src/jf/http_client.h"

#include <assert.h>
#include <stdarg.h>
#include <string.h>

static int response_status = 200;
static int request_failure;
static int requests;
static char last_path[1024];
static int last_delete;

const char *jf_device_id(void) { return "test-device"; }
void say(const char *format, ...) { (void)format; }

int http_post_json(const char *host, int port, const char *path, const char *authorization,
                   const char *json, HttpResponse *out)
{
    assert(strcmp(host, "test-host") == 0 && port == 8096);
    assert(strstr(authorization, "test-token") != NULL);
    assert(json != NULL);
    ++requests;
    last_delete = 0;
    strncpy(last_path, path, sizeof(last_path) - 1);
    last_path[sizeof(last_path) - 1] = '\0';
    if (request_failure) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->status = response_status;
    return 0;
}

int http_delete(const char *host, int port, const char *path, const char *authorization,
                HttpResponse *out)
{
    const int result = http_post_json(host, port, path, authorization, "", out);
    last_delete = 1;
    return result;
}

void http_response_free(HttpResponse *response) { (void)response; }

int main(void)
{
    JfSession session = {0};
    strcpy(session.user_id, "user-id");
    strcpy(session.access_token, "test-token");
    assert(jf_init("test-host", 8096) == 0);
    assert(jf_mark_played(&session, "episode-id") == 0);
    assert(strcmp(last_path, "/Users/user-id/PlayedItems/episode-id") == 0);
    assert(last_delete == 0);
    assert(requests == 1);

    response_status = 204;
    assert(jf_mark_unplayed(&session, "episode-id") == 0);
    assert(strcmp(last_path, "/Users/user-id/PlayedItems/episode-id") == 0);
    assert(last_delete == 1);
    assert(requests == 2);

    response_status = 500;
    assert(jf_mark_played(&session, "episode-id") != 0);
    assert(strstr(jf_last_error(), "server") != NULL);
    assert(jf_mark_unplayed(&session, "episode-id") != 0);
    assert(strstr(jf_last_error(), "server") != NULL);
    request_failure = 1;
    assert(jf_mark_played(&session, "episode-id") != 0);
    assert(strstr(jf_last_error(), "request") != NULL);
    assert(jf_mark_unplayed(&session, "episode-id") != 0);
    assert(strstr(jf_last_error(), "request") != NULL);
    assert(jf_mark_played(&session, "") != 0);
    assert(jf_mark_unplayed(&session, "") != 0);
    assert(jf_mark_unplayed(NULL, "episode-id") != 0);
    assert(requests == 6);
    return 0;
}
