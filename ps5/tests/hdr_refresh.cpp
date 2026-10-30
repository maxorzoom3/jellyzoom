#include "../src/jf/jf_hdrlab.h"

#include <cassert>
#include <cstdint>
#include <cstring>

namespace
{
constexpr std::uint64_t hdrFormat = UINT64_C(0x8100070400000000);
bool flag = false;
int refusedHdrChanges = 0;
std::uint64_t refresh = 13;
int downshifts = 0, restores = 0, hdrChanges = 0, sdrChanges = 0;

void reset(bool enabled, std::uint64_t rate, int refuse = 0)
{
	flag = enabled;
	refresh = rate;
	refusedHdrChanges = refuse;
	downshifts = restores = hdrChanges = sdrChanges = 0;
}
}

extern "C" {
void say(const char *, ...) {}
int __wrap_access(const char *path, int)
{
	return flag && std::strcmp(path, "/data/jf-hdr-60") == 0 ? 0 : -1;
}
int __wrap_usleep(unsigned int) { return 0; }
int sceVideoOutGetOutputStatus(std::int32_t, void *status)
{
	std::memcpy(static_cast<std::uint8_t *>(status) + 8, &refresh, sizeof(refresh));
	return 0;
}
void sceVideoOutSetBufferAttribute2(void *attribute, std::uint64_t format, std::uint32_t, std::uint32_t,
	std::uint32_t, std::uint64_t, std::uint32_t, std::uint64_t)
{
	std::memcpy(attribute, &format, sizeof(format));
}
int sceVideoOutSubmitChangeBufferAttribute2(std::int32_t, std::int32_t, const void *attribute, const void *)
{
	std::uint64_t format = 0;
	std::memcpy(&format, attribute, sizeof(format));
	if (format == hdrFormat) {
		++hdrChanges;
		if (refusedHdrChanges > 0) {
			--refusedHdrChanges;
			return -1;
		}
	} else {
		++sdrChanges;
	}
	return 0;
}
int sceVideoOutConfigureOutput(std::int32_t, std::uint32_t mode, const void *, const void *, const void *)
{
	if (mode == 1) {
		++downshifts;
		refresh = 3;
	} else if (mode == 15) {
		++restores;
		refresh = 13;
	}
	return 0;
}
int __real_sceVideoOutRegisterBuffers2(std::int32_t, std::int32_t, std::int32_t, void *,
	std::int32_t, void *, std::int32_t, void *) { return -1; }
}

int main()
{
	assert(!jfhdr_setOutput(-1, true, 3840, 2160));

	reset(false, 13);
	assert(jfhdr_setOutput(1, true, 3840, 2160));
	assert(downshifts == 0 && hdrChanges == 1);
	assert(jfhdr_setOutput(1, false, 3840, 2160));
	assert(sdrChanges == 1 && restores == 0);

	reset(true, 13);
	assert(jfhdr_setOutput(1, true, 3840, 2160));
	assert(downshifts == 1 && hdrChanges == 1);
	assert(jfhdr_setOutput(1, false, 3840, 2160));
	assert(sdrChanges == 1 && restores == 1);

	reset(false, 13, 1);
	assert(jfhdr_setOutput(1, true, 3840, 2160));
	assert(downshifts == 1 && hdrChanges == 2);
	assert(jfhdr_setOutput(1, false, 3840, 2160));
	assert(sdrChanges == 1 && restores == 1);

	reset(false, 3);
	assert(jfhdr_setOutput(1, true, 3840, 2160));
	assert(downshifts == 0 && hdrChanges == 1);
	assert(jfhdr_setOutput(1, false, 3840, 2160));
	assert(sdrChanges == 1 && restores == 0);
}
