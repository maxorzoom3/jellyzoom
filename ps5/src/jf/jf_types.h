#pragma once
#include <stdint.h>

#define JF_MAX_ITEMS 512
#define JF_MAX_STRING 1024

typedef struct {
    char id[128];
    char name[512];
    char type[64];
    char overview[2048];
    char image_tag[256];
    int64_t runtime_ticks;
    int64_t playback_position_ticks;
    int child_count;
} JfItem;

typedef struct {
    char access_token[2048];
    char user_id[128];
    char server_id[128];
} JfSession;

#define JF_MAX_MEDIA_STREAMS 64

typedef struct {
    int index;
    char type[32];
    char codec[64];
    char language[32];
    char display_title[128];
    char title[128];
    char channel_layout[64];
    int channels;
    int is_default;
    int is_forced;
    int is_external;
    int is_text_subtitle;
} JfMediaStream;

typedef struct {
    char media_source_id[128];
    char play_session_id[128];
    char container[64];
    char video_codec[64];
    char audio_codec[64];
    char direct_stream_url[2048];
    char transcoding_url[2048];
    int direct_play;
    int direct_stream;
    int transcoding;
    int64_t bitrate;

    JfMediaStream streams[JF_MAX_MEDIA_STREAMS];
    int stream_count;

    int selected_audio_stream_index;
    int selected_subtitle_stream_index;
} JfPlayback;

typedef struct {
    const char *name;
    int supported;
} CodecCapability;
