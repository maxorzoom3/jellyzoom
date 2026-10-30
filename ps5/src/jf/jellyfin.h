#pragma once
#include "jf_types.h"

int jf_init(const char *host, int port);
int jf_authenticate(const char *username, const char *password, JfSession *session);
int jf_get_libraries(const JfSession *session, JfItem *items, int max_items, int *count);
int jf_get_items(const JfSession *session, const char *parent_id,
                 JfItem *items, int max_items, int *count);
int jf_get_playback_info(const JfSession *session, const char *item_id,
                         JfPlayback *playback);

int jf_report_playing(
    const JfSession *session,
    const char *item_id,
    const JfPlayback *playback,
    int64_t position_ticks);

int jf_report_progress(
    const JfSession *session,
    const char *item_id,
    const JfPlayback *playback,
    int64_t position_ticks,
    int paused);

int jf_report_stopped(
    const JfSession *session,
    const char *item_id,
    const JfPlayback *playback,
    int64_t position_ticks);

int jf_mark_played(const JfSession *session, const char *item_id);
int jf_mark_unplayed(const JfSession *session, const char *item_id);

const char *jf_last_error(void);
