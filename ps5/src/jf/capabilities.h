#include <stddef.h>
#pragma once
#include "jf_types.h"

void jf_build_capability_profile(char *out, size_t out_size);
int jf_codec_supported(const char *codec);
