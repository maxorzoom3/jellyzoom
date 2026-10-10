#pragma once
#include <atomic>
extern "C" {
#include "jf_types.h"
}
/* Opens one item's real stream with FFmpeg and logs what happens (FFPROBE lines). */
void jf_probe(const JfSession *session, const char *itemId, const char *itemName, std::atomic<bool> *stopFlag);
