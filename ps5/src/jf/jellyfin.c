#include "jellyfin.h"
#include "platform.h"
#include "http_client.h"
#include "jf_config.h"
#include <json-c/json.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static char jf_host[256];
static int jf_port;
static char jf_error[512];

static void set_error(const char *s) {
    snprintf(jf_error, sizeof(jf_error), "%s", s ? s : "unknown error");
}

const char *jf_last_error(void) {
    return jf_error;
}

int jf_init(const char *host, int port) {
    snprintf(jf_host, sizeof(jf_host), "%s", host);
    jf_port = port;
    jf_error[0] = 0;
    return 0;
}

static void auth_header(const JfSession *s, char *out, size_t n) {
    if (!s->access_token[0]) {
        out[0] = 0;
        return;
    }

    snprintf(out, n,
        "Authorization: MediaBrowser Client=\"%s\", "
        "Device=\"%s\", DeviceId=\"%s\", Version=\"%s\", Token=\"%s\"\r\n",
        JF_CLIENT_NAME,
        JF_DEVICE_NAME,
        JF_DEVICE_ID,
        JF_CLIENT_VERSION,
        s->access_token);
}

static const char *obj_string(struct json_object *o, const char *key) {
    struct json_object *v = NULL;

    if (!o || !json_object_object_get_ex(o, key, &v))
        return "";

    if (!json_object_is_type(v, json_type_string))
        return "";

    return json_object_get_string(v);
}

static int parse_items(
    const char *body,
    JfItem *items,
    int max,
    int *count)
{
    struct json_object *root = json_tokener_parse(body);

    if (!root)
        return -1;

    struct json_object *arr = NULL;

    if (!json_object_object_get_ex(root, "Items", &arr) ||
        !json_object_is_type(arr, json_type_array)) {

        json_object_put(root);
        return -1;
    }

    int n = json_object_array_length(arr);

    if (n > max)
        n = max;

    for (int i = 0; i < n; ++i) {

        struct json_object *o =
            json_object_array_get_idx(arr, i);

        memset(&items[i], 0, sizeof(items[i]));

        snprintf(
            items[i].id,
            sizeof(items[i].id),
            "%s",
            obj_string(o, "Id"));

        snprintf(
            items[i].name,
            sizeof(items[i].name),
            "%s",
            obj_string(o, "Name"));

        snprintf(
            items[i].type,
            sizeof(items[i].type),
            "%s",
            obj_string(o, "Type"));

        snprintf(
            items[i].overview,
            sizeof(items[i].overview),
            "%s",
            obj_string(o, "Overview"));

        snprintf(
            items[i].image_tag,
            sizeof(items[i].image_tag),
            "%s",
            obj_string(o, "PrimaryImageTag"));

        struct json_object *ticks = NULL;

        if (json_object_object_get_ex(
                o,
                "RunTimeTicks",
                &ticks)) {

            items[i].runtime_ticks =
                json_object_get_int64(ticks);
        }

        /*
         * Jellyfin stores the user's resume position inside
         * UserData.PlaybackPositionTicks.
         */
        struct json_object *user_data = NULL;

        if (json_object_object_get_ex(
                o,
                "UserData",
                &user_data)) {

            struct json_object *playback_position = NULL;

            if (json_object_object_get_ex(
                    user_data,
                    "PlaybackPositionTicks",
                    &playback_position)) {

                items[i].playback_position_ticks =
                    json_object_get_int64(
                        playback_position);
            }
        }

        struct json_object *child_count = NULL;

        if (json_object_object_get_ex(
                o,
                "ChildCount",
                &child_count)) {

            items[i].child_count =
                json_object_get_int(child_count);
        }
    }

    *count = n;

    json_object_put(root);

    return 0;
}

int jf_authenticate(
    const char *username,
    const char *password,
    JfSession *session)
{
    char body[4096];

    struct json_object *auth_obj =
        json_object_new_object();

    struct json_object *user_obj =
        json_object_new_string(username);

    struct json_object *pass_obj =
        json_object_new_string(password);

    json_object_object_add(
        auth_obj,
        "Username",
        user_obj);

    json_object_object_add(
        auth_obj,
        "Pw",
        pass_obj);

    snprintf(
        body,
        sizeof(body),
        "%s",
        json_object_to_json_string(auth_obj));

    json_object_put(auth_obj);

    char auth[4096];

    snprintf(
        auth,
        sizeof(auth),
        "Authorization: MediaBrowser Client=\"%s\", "
        "Device=\"%s\", DeviceId=\"%s\", Version=\"%s\"\r\n",
        JF_CLIENT_NAME,
        JF_DEVICE_NAME,
        JF_DEVICE_ID,
        JF_CLIENT_VERSION);

    HttpResponse r;

    if (http_post_json(
            jf_host,
            jf_port,
            "/Users/AuthenticateByName",
            auth,
            body,
            &r) != 0) {

        set_error(
            "HTTP authentication request failed");

        return -1;
    }

    if (r.status < 200 || r.status >= 300) {

        char err[512];

        snprintf(
            err,
            sizeof(err),
            "Jellyfin authentication HTTP %d: %.400s",
            r.status,
            r.body ? r.body : "(empty response)");

        set_error(err);

        http_response_free(&r);

        return -1;
    }

    struct json_object *root =
        json_tokener_parse(r.body);

    if (!root) {

        set_error(
            "Invalid authentication JSON");

        http_response_free(&r);

        return -1;
    }

    memset(
        session,
        0,
        sizeof(*session));

    snprintf(
        session->access_token,
        sizeof(session->access_token),
        "%s",
        obj_string(root, "AccessToken"));

    snprintf(
        session->server_id,
        sizeof(session->server_id),
        "%s",
        obj_string(root, "ServerId"));

    struct json_object *user = NULL;

    if (json_object_object_get_ex(
            root,
            "User",
            &user)) {

        snprintf(
            session->user_id,
            sizeof(session->user_id),
            "%s",
            obj_string(user, "Id"));
    }

    json_object_put(root);

    http_response_free(&r);

    if (!session->access_token[0] ||
        !session->user_id[0]) {

        set_error(
            "Authentication response did not contain token/user");

        return -1;
    }

    return 0;
}

int jf_get_libraries(
    const JfSession *session,
    JfItem *items,
    int max_items,
    int *count)
{
    char auth[4096];

    auth_header(
        session,
        auth,
        sizeof(auth));

    char path[512];

    snprintf(
        path,
        sizeof(path),
        "/Users/%s/Views?IncludeHidden=false",
        session->user_id);

    HttpResponse r;

    if (http_get(
            jf_host,
            jf_port,
            path,
            auth,
            &r) != 0) {

        set_error(
            "Library request failed");

        return -1;
    }

    if (r.status != 200) {

        char err[512];

        snprintf(
            err,
            sizeof(err),
            "Libraries HTTP %d: %.400s",
            r.status,
            r.body ? r.body : "(empty response)");

        set_error(err);

        http_response_free(&r);

        return -1;
    }

    int rc =
        parse_items(
            r.body,
            items,
            max_items,
            count);

    if (rc != 0)
        set_error(
            "Invalid Jellyfin library JSON");

    http_response_free(&r);

    return rc;
}

int jf_get_items(
    const JfSession *session,
    const char *parent_id,
    JfItem *items,
    int max_items,
    int *count)
{
    char auth[4096];

    auth_header(
        session,
        auth,
        sizeof(auth));

    char path[1024];

    snprintf(
        path,
        sizeof(path),
        "/Users/%s/Items"
        "?ParentId=%s"
        "&Recursive=false"
        "&Fields=Overview,PrimaryImageAspectRatio,RunTimeTicks,ChildCount,UserData"
        "&SortBy=SortName"
        "&SortOrder=Ascending",
        session->user_id,
        parent_id);

    HttpResponse r;

    if (http_get(
            jf_host,
            jf_port,
            path,
            auth,
            &r) != 0) {

        set_error(
            "Items request failed");

        return -1;
    }

    if (r.status != 200) {

        char err[512];

        snprintf(
            err,
            sizeof(err),
            "Items HTTP %d: %.400s",
            r.status,
            r.body ? r.body : "(empty response)");

        set_error(err);

        http_response_free(&r);

        return -1;
    }

    int rc =
        parse_items(
            r.body,
            items,
            max_items,
            count);

    if (rc != 0)
        set_error(
            "Invalid Jellyfin items JSON");

    http_response_free(&r);

    return rc;
}

int jf_get_playback_info(
    const JfSession *session,
    const char *item_id,
    JfPlayback *playback)
{
    char auth[4096];

    auth_header(
        session,
        auth,
        sizeof(auth));

    char path[1024];

    snprintf(
        path,
        sizeof(path),
        "/Items/%s/PlaybackInfo"
        "?UserId=%s"
        "&IsPlayback=true"
        "&AutoOpenLiveStream=true"
        "&MaxStreamingBitrate=140000000",
        item_id,
        session->user_id);

    HttpResponse r;

    if (http_post_json(
            jf_host,
            jf_port,
            path,
            auth,
            "{}",
            &r) != 0) {

        set_error(
            "PlaybackInfo request failed");

        return -1;
    }

    if (r.status < 200 || r.status >= 300) {

        char err[512];

        snprintf(
            err,
            sizeof(err),
            "PlaybackInfo HTTP %d: %.400s",
            r.status,
            r.body ? r.body : "(empty response)");

        set_error(err);

        http_response_free(&r);

        return -1;
    }

    struct json_object *root =
        json_tokener_parse(r.body);

    if (!root) {

        set_error(
            "Invalid PlaybackInfo JSON");

        http_response_free(&r);

        return -1;
    }

    memset(
        playback,
        0,
        sizeof(*playback));

    snprintf(
        playback->play_session_id,
        sizeof(playback->play_session_id),
        "%s",
        obj_string(root, "PlaySessionId"));

    struct json_object *sources = NULL;

    if (json_object_object_get_ex(
            root,
            "MediaSources",
            &sources) &&
        json_object_is_type(
            sources,
            json_type_array) &&
        json_object_array_length(sources) > 0) {

        struct json_object *src =
            json_object_array_get_idx(
                sources,
                0);

        snprintf(
            playback->media_source_id,
            sizeof(playback->media_source_id),
            "%s",
            obj_string(src, "Id"));

        snprintf(
            playback->container,
            sizeof(playback->container),
            "%s",
            obj_string(src, "Container"));

        snprintf(
            playback->direct_stream_url,
            sizeof(playback->direct_stream_url),
            "%s",
            obj_string(src, "DirectStreamUrl"));

        snprintf(
            playback->transcoding_url,
            sizeof(playback->transcoding_url),
            "%s",
            obj_string(src, "TranscodingUrl"));

        struct json_object *br = NULL;

        if (json_object_object_get_ex(
                src,
                "Bitrate",
                &br)) {

            playback->bitrate =
                json_object_get_int64(br);
        }

        struct json_object *dp = NULL;

        if (json_object_object_get_ex(
                src,
                "SupportsDirectPlay",
                &dp)) {

            playback->direct_play =
                json_object_get_boolean(dp);
        }

        struct json_object *ds = NULL;

        if (json_object_object_get_ex(
                src,
                "SupportsDirectStream",
                &ds)) {

            playback->direct_stream =
                json_object_get_boolean(ds);
        }

        struct json_object *tr = NULL;

        if (json_object_object_get_ex(
                src,
                "SupportsTranscoding",
                &tr)) {

            playback->transcoding =
                json_object_get_boolean(tr);
        }

        struct json_object *streams = NULL;

        if (json_object_object_get_ex(
                src,
                "MediaStreams",
                &streams) &&
            json_object_is_type(
                streams,
                json_type_array)) {

            int sn =
                json_object_array_length(streams);

            playback->stream_count = 0;
            playback->selected_audio_stream_index = -1;
            playback->selected_subtitle_stream_index = -1;

            for (int i = 0;
                 i < sn && playback->stream_count < JF_MAX_MEDIA_STREAMS;
                 ++i) {

                struct json_object *st =
                    json_object_array_get_idx(streams, i);

                if (!st)
                    continue;

                JfMediaStream *ms =
                    &playback->streams[playback->stream_count];

                memset(ms, 0, sizeof(*ms));

                struct json_object *value = NULL;

                if (json_object_object_get_ex(st, "Index", &value))
                    ms->index = json_object_get_int(value);
                else
                    ms->index = i;

                snprintf(
                    ms->type,
                    sizeof(ms->type),
                    "%s",
                    obj_string(st, "Type"));

                snprintf(
                    ms->codec,
                    sizeof(ms->codec),
                    "%s",
                    obj_string(st, "Codec"));

                snprintf(
                    ms->language,
                    sizeof(ms->language),
                    "%s",
                    obj_string(st, "Language"));

                snprintf(
                    ms->display_title,
                    sizeof(ms->display_title),
                    "%s",
                    obj_string(st, "DisplayTitle"));

                snprintf(
                    ms->title,
                    sizeof(ms->title),
                    "%s",
                    obj_string(st, "Title"));

                snprintf(
                    ms->channel_layout,
                    sizeof(ms->channel_layout),
                    "%s",
                    obj_string(st, "ChannelLayout"));

                if (json_object_object_get_ex(
                        st,
                        "Channels",
                        &value))
                    ms->channels =
                        json_object_get_int(value);

                if (json_object_object_get_ex(
                        st,
                        "IsDefault",
                        &value))
                    ms->is_default =
                        json_object_get_boolean(value);

                if (json_object_object_get_ex(
                        st,
                        "IsForced",
                        &value))
                    ms->is_forced =
                        json_object_get_boolean(value);

                if (json_object_object_get_ex(
                        st,
                        "IsExternal",
                        &value))
                    ms->is_external =
                        json_object_get_boolean(value);

                if (json_object_object_get_ex(
                        st,
                        "IsTextSubtitleStream",
                        &value))
                    ms->is_text_subtitle =
                        json_object_get_boolean(value);

                say(
                    "MediaStream %d: Index=%d Type=%s Codec=%s "
                    "Language=%s Title=%s Channels=%d Default=%d Forced=%d\n",
                    i,
                    ms->index,
                    ms->type,
                    ms->codec,
                    ms->language,
                    ms->display_title[0]
                        ? ms->display_title
                        : ms->title,
                    ms->channels,
                    ms->is_default,
                    ms->is_forced);

                /*
                 * Keep the first video/audio codec for the
                 * existing playback logic.
                 */
                if (!strcmp(ms->type, "Video") &&
                    !playback->video_codec[0]) {

                    snprintf(
                        playback->video_codec,
                        sizeof(playback->video_codec),
                        "%s",
                        ms->codec);
                }

                if (!strcmp(ms->type, "Audio") &&
                    !playback->audio_codec[0]) {

                    snprintf(
                        playback->audio_codec,
                        sizeof(playback->audio_codec),
                        "%s",
                        ms->codec);
                }

                playback->stream_count++;
            }

            /*
             * Select Jellyfin's default audio stream.
             * Fall back to the first audio stream.
             */
            for (int i = 0;
                 i < playback->stream_count;
                 ++i) {

                JfMediaStream *ms =
                    &playback->streams[i];

                if (!strcmp(ms->type, "Audio")) {

                    if (playback->selected_audio_stream_index < 0 ||
                        ms->is_default) {

                        playback->selected_audio_stream_index =
                            ms->index;

                        if (ms->is_default)
                            break;
                    }
                }
            }

            /*
             * Subtitle defaults to OFF.
             */
            playback->selected_subtitle_stream_index = -1;
        }
    }

    json_object_put(root);

    http_response_free(&r);

    return 0;
}


/*
 * Report that an item has started playing.
 *
 * Jellyfin expects playback position in ticks.
 * 10,000,000 ticks = 1 second.
 */
int jf_report_playing(
    const JfSession *session,
    const char *item_id,
    const JfPlayback *playback,
    int64_t position_ticks)
{
    if (!session || !item_id || !playback)
        return -1;

    char auth[4096];

    auth_header(
        session,
        auth,
        sizeof(auth));

    char path[1024];

    snprintf(
        path,
        sizeof(path),
        "/Sessions/Playing");

    char body[4096];

    snprintf(
        body,
        sizeof(body),
        "{"
        "\"ItemId\":\"%s\","
        "\"PlaySessionId\":\"%s\","
        "\"PositionTicks\":%lld,"
        "\"CanSeek\":true,"
        "\"IsPaused\":false,"
        "\"IsMuted\":false"
        "}",
        item_id,
        playback->play_session_id,
        (long long)position_ticks);

    HttpResponse r;

    if (http_post_json(
            jf_host,
            jf_port,
            path,
            auth,
            body,
            &r) != 0) {

        set_error(
            "Playing report request failed");

        return -1;
    }

    if (r.status < 200 || r.status >= 300) {

        say(
            "Jellyfin Playing HTTP %d: %.300s\n",
            r.status,
            r.body ? r.body : "(empty)");

        http_response_free(&r);

        return -1;
    }

    http_response_free(&r);

    return 0;
}


/*
 * Report current playback progress.
 */
int jf_report_progress(
    const JfSession *session,
    const char *item_id,
    const JfPlayback *playback,
    int64_t position_ticks,
    int paused)
{
    if (!session || !item_id || !playback)
        return -1;

    char auth[4096];

    auth_header(
        session,
        auth,
        sizeof(auth));

    char path[1024];

    snprintf(
        path,
        sizeof(path),
        "/Sessions/Playing/Progress");

    char body[4096];

    snprintf(
        body,
        sizeof(body),
        "{"
        "\"ItemId\":\"%s\","
        "\"PlaySessionId\":\"%s\","
        "\"PositionTicks\":%lld,"
        "\"CanSeek\":true,"
        "\"IsPaused\":%s,"
        "\"IsMuted\":false"
        "}",
        item_id,
        playback->play_session_id,
        (long long)position_ticks,
        paused ? "true" : "false");

    HttpResponse r;

    if (http_post_json(
            jf_host,
            jf_port,
            path,
            auth,
            body,
            &r) != 0) {

        set_error(
            "Progress report request failed");

        return -1;
    }

    if (r.status < 200 || r.status >= 300) {

        say(
            "Jellyfin Progress HTTP %d: %.300s\n",
            r.status,
            r.body ? r.body : "(empty)");

        http_response_free(&r);

        return -1;
    }

    http_response_free(&r);

    return 0;
}


/*
 * Report that playback has stopped.
 */
int jf_report_stopped(
    const JfSession *session,
    const char *item_id,
    const JfPlayback *playback,
    int64_t position_ticks)
{
    if (!session || !item_id || !playback)
        return -1;

    char auth[4096];

    auth_header(
        session,
        auth,
        sizeof(auth));

    char path[1024];

    snprintf(
        path,
        sizeof(path),
        "/Sessions/Playing/Stopped");

    char body[4096];

    snprintf(
        body,
        sizeof(body),
        "{"
        "\"ItemId\":\"%s\","
        "\"PlaySessionId\":\"%s\","
        "\"PositionTicks\":%lld"
        "}",
        item_id,
        playback->play_session_id,
        (long long)position_ticks);

    HttpResponse r;

    if (http_post_json(
            jf_host,
            jf_port,
            path,
            auth,
            body,
            &r) != 0) {

        set_error(
            "Stopped report request failed");

        return -1;
    }

    if (r.status < 200 || r.status >= 300) {

        say(
            "Jellyfin Stopped HTTP %d: %.300s\n",
            r.status,
            r.body ? r.body : "(empty)");

        http_response_free(&r);

        return -1;
    }

    http_response_free(&r);

    return 0;
}
