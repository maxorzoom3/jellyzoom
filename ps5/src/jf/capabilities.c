#include "capabilities.h"
#include <stdio.h>
#include <string.h>

static const char *video[] = {
    "h264", "hevc", "av1", "vp9", "vp8", "mpeg2video", "mpeg4"
};

static const char *audio[] = {
    "aac", "ac3", "eac3", "dts", "dca", "flac", "opus",
    "vorbis", "alac", "mp3", "pcm"
};

int jf_codec_supported(const char *codec) {
    if (!codec || !codec[0]) return 0;
    for (unsigned i = 0; i < sizeof(video)/sizeof(video[0]); ++i)
        if (!strcmp(codec, video[i])) return 1;
    for (unsigned i = 0; i < sizeof(audio)/sizeof(audio[0]); ++i)
        if (!strcmp(codec, audio[i])) return 1;
    return 0;
}

void jf_build_capability_profile(char *out, size_t out_size) {
    snprintf(out, out_size,
        "H264,HEVC,AV1,VP9,VP8,MPEG2VIDEO,MPEG4;"
        "AAC,AC3,EAC3,DTS,FLAC,OPUS,VORBIS,ALAC,MP3,PCM;"
        "SRT,ASS,PGS,DVDSUB,DVBSUB;"
        "MKV,WEBM,MP4,MOV,MPEGTS,AVI,OGG,WAV;"
        "HDR10,HLG");
}
