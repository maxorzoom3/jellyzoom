// Jellyfin Wheel - HDR lab (Stage 11). See jf_hdrlab.h.
#include "jf_hdrlab.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <atomic>
#include <cstring>

#include <unistd.h>

#include "platform.h"

extern "C" {
// Exported by the console's libSceVideoOut (the link stub carries them); not declared in ps5platform/videoout.h.
void sceVideoOutSetBufferAttribute2(void *attribute, std::uint64_t format, std::uint32_t tiling, std::uint32_t width,
                                    std::uint32_t height, std::uint64_t option, std::uint32_t reserved0,
                                    std::uint64_t reserved1);
int sceVideoOutSubmitChangeBufferAttribute2(std::int32_t handle, std::int32_t setIndex, const void *attribute,
                                            const void *option);
int sceVideoOutGetOutputStatus(std::int32_t handle, void *status);
int sceVideoOutGetFlipStatus(std::int32_t handle, void *status);
int sceVideoOutConfigureOutput(std::int32_t handle, std::uint32_t mode, const void *a, const void *b, const void *c);
int __real_sceVideoOutRegisterBuffers2(std::int32_t handle, std::int32_t set, std::int32_t start, void *buffers,
                                       std::int32_t count, void *attribute, std::int32_t option, void *reserved);
int __real_sceVideoOutSubmitFlip(std::int32_t handle, std::int32_t index, std::uint32_t mode, std::int64_t arg);
int __real_sceVideoOutOpen(std::int32_t user, std::int32_t bus, std::int32_t index, const void *parameter);
}

static std::atomic<int> g_openedHandle{ -1 };
static std::atomic<int> g_openCount{ 0 };
// The first set of buffers the graphics layer registered (its set 0), noted by the wrapped sceVideoOutRegisterBuffers2.
struct RegisteredSet {
	std::atomic<bool> valid{ false };
	int handle{ -1 };
	int start{ 0 };
	int count{ 0 };
	void *data[8]{};
};
static RegisteredSet g_reg;
// While above zero, every flip is shown from the HDR-typed view of the same buffers: index + g_redirect.
static std::atomic<int> g_redirect{ 0 };
static bool g_was120 = false;   // the output was at 119.88 Hz when the lab began and goes back to it afterwards
static bool g_usedChange = false; // HDR came from retyping the set in place

namespace jfhdr_impl {

namespace {

constexpr std::uint64_t kFormatSdr = UINT64_C(0x8000000000000000); // B8G8R8A8
constexpr std::uint64_t kFormatHdr = UINT64_C(0x8100070400000000); // Bgr10A2Bt2100Pq

struct OutputStatus {
	std::uint32_t resolution;
	std::uint32_t dynamicRange; // 0 unknown, 1 SDR, 2 HDR
	std::uint64_t refreshRate;
	std::uint64_t flags; // bit 0: HDR output active
	std::uint64_t reserved[3];
};

std::uint32_t code10(double v)
{
	return static_cast<std::uint32_t>(std::lround(std::min(1.0, std::max(0.0, v)) * 1023.0));
}

} // namespace

double pqEncode(double nits)
{
	const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0, c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0,
	             c3 = 2392.0 / 4096.0 * 32.0;
	const double y = std::pow(std::min(1.0, std::max(0.0, nits / 10000.0)), m1);
	return std::pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

bool wanted()
{
	static const char *path = "/data/jf-hdr-lab";
	if (access(path, F_OK) != 0) {
		return false;
	}
	if (unlink(path) != 0) {
		say("HDRLAB: the flag file stays (could not remove %s): delete it yourself or the lab runs at every start", path);
	}
	return true;
}

int findHandle()
{
	// The handle is the graphics layer's and is not exported: the link step wraps sceVideoOutOpen (--wrap) so that every
	// open is noted here. Scanning for it found nothing, and a second open is refused as busy.
	const int found = g_openedHandle.load();
	say("HDRLAB: sceVideoOutOpen was seen %d time(s), the last handle is %d", g_openCount.load(), found);
	if (found >= 0) {
		std::uint64_t status[16];
		std::memset(status, 0, sizeof(status));
		say("HDRLAB: that handle: GetFlipStatus rc=0x%08x", static_cast<unsigned>(sceVideoOutGetFlipStatus(found, status)));
	}
	return found;
}

void logStatus(int handle, const char *when)
{
	OutputStatus st;
	std::memset(&st, 0, sizeof(st));
	const int rc = sceVideoOutGetOutputStatus(handle, &st);
	say("HDRLAB: %s: output status rc=0x%08x resolution=%u dynamic_range=%u (%s) refresh=%llu flags=0x%llx", when,
	    static_cast<unsigned>(rc), st.resolution, st.dynamicRange,
	    st.dynamicRange == 2 ? "HDR" : (st.dynamicRange == 1 ? "SDR" : "unknown"),
	    static_cast<unsigned long long>(st.refreshRate), static_cast<unsigned long long>(st.flags));
}

bool setOutput(int handle, bool hdr, std::uint32_t width, std::uint32_t height)
{
	if (handle < 0) {
		return false;
	}
	auto change = [&](const char *what, std::uint64_t format) {
		std::uint8_t attr[80];
		std::memset(attr, 0, sizeof(attr));
		sceVideoOutSetBufferAttribute2(attr, format, 0, width, height, 0, 0, 0);
		const int rc = sceVideoOutSubmitChangeBufferAttribute2(handle, 0, attr, nullptr);
		say("HDRLAB: %s (0x%llx), retyping set 0 in place: rc=0x%08x", what, static_cast<unsigned long long>(format), static_cast<unsigned>(rc));
		return rc == 0;
	};
	if (!hdr) {
		g_redirect = 0;
		if (g_usedChange) {
			change("back to SDR", kFormatSdr);
			g_usedChange = false;
		} else {
			say("HDRLAB: flips back on the SDR buffers");
		}
		if (g_was120) {
			usleep(300000);
			const int rc = sceVideoOutConfigureOutput(handle, 15, nullptr, nullptr, nullptr);
			say("HDRLAB: 119.88 Hz mode restored: rc=0x%08x", static_cast<unsigned>(rc));
			g_was120 = false;
		}
		return true;
	}
	OutputStatus st;
	std::memset(&st, 0, sizeof(st));
	const int statusRc = sceVideoOutGetOutputStatus(handle, &st);
	if (statusRc != 0) {
		say("HDRLAB: could not read output status: rc=0x%08x", static_cast<unsigned>(statusRc));
	}
	if (st.refreshRate == 13 && access("/data/jf-hdr-60", F_OK) != 0) {
		say("HDRLAB: trying HDR10 without leaving 119.88 Hz");
		if (change("Bgr10A2Bt2100Pq at 119.88 Hz", kFormatHdr)) {
			g_usedChange = true;
			say("HDRLAB: 119.88 Hz HDR buffer accepted; check the display's actual signal");
			return true;
		}
		say("HDRLAB: 119.88 Hz HDR buffer refused; trying the normal 59.94 Hz path");
	}
	// A refused HDR buffer or the compatibility flag uses the established 59.94 Hz path.
	if (st.refreshRate == 13) {
		const int rc = sceVideoOutConfigureOutput(handle, 1, nullptr, nullptr, nullptr);
		say("HDRLAB: output was at 119.88 Hz (refresh code 13); going to 59.94 Hz: rc=0x%08x", static_cast<unsigned>(rc));
		g_was120 = rc == 0;
		usleep(1500000); // the display re-syncs
		logStatus(handle, "after the mode change");
	} else {
		say("HDRLAB: refresh code %llu, the mode is left alone", static_cast<unsigned long long>(st.refreshRate));
	}
	// The format word differs between firmware generations: the Bgr10A2Bt2100Pq value is from newer system software, and this
	// console runs 4.03. So try the others that exist in the PS4-style table (the format is its upper 32 bits there) as well.
	static const struct {
		const char *what;
		std::uint64_t format;
	} formats[] = { { "Bgr10A2Bt2100Pq", UINT64_C(0x8100070400000000) },
	                { "Rgb10A2Bt2100Pq", UINT64_C(0x8100070422000000) },
	                { "A2R10G10B10 BT.2020 PQ, PS4 value in the upper word", UINT64_C(0x8874000000000000) },
	                { "A2R10G10B10 BT.2020 PQ, PS4 value in the lower word", UINT64_C(0x0000000088740000) },
	                { "A2R10G10B10, PS4 value in the upper word", UINT64_C(0x8806000000000000) },
	                { "A2R10G10B10 sRGB, PS4 value in the upper word", UINT64_C(0x8800000000000000) } };
	struct VideoBuffer {
		void *data;
		void *metadata;
		void *reserved[2];
	} buffers[8];
	std::memset(buffers, 0, sizeof(buffers));
	const bool canRegister = g_reg.valid && g_reg.handle == handle;
	if (canRegister) {
		for (int i = 0; i < g_reg.count; i++) {
			buffers[i].data = g_reg.data[i];
		}
	} else {
		say("HDRLAB: the graphics layer's buffers were not seen being registered, so only the in-place retype is tried");
	}
	const int newStart = canRegister ? g_reg.start + g_reg.count : 0;
	for (const auto &f : formats) {
		if (change(f.what, f.format)) {
			g_usedChange = true;
			say("HDRLAB: ACCEPTED in place: %s", f.what);
			return true;
		}
		if (!canRegister) {
			continue;
		}
		std::uint8_t attr[80];
		std::memset(attr, 0, sizeof(attr));
		sceVideoOutSetBufferAttribute2(attr, f.format, 0, width, height, 0, 0, 0);
		const int rc = __real_sceVideoOutRegisterBuffers2(handle, 1, newStart, buffers, g_reg.count, attr, 0, nullptr);
		say("HDRLAB: %s, registered again as set 1 (indices %d..): rc=0x%08x", f.what, newStart, static_cast<unsigned>(rc));
		if (rc == 0) {
			g_redirect = newStart - g_reg.start;
			say("HDRLAB: ACCEPTED as a second set: %s; flips now go to it (index + %d)", f.what, g_redirect.load());
			return true;
		}
		// a refused registration leaves set 1 free for the next try
	}
	return false;
}

const char *variantName(int variant)
{
	static const char *names[jfhdr::kVariants] = { "1: red low bits, PQ", "2: blue low bits, PQ", "3: red low bits, gamma",
	                                        "4: blue low bits, gamma" };
	return names[(variant % jfhdr::kVariants + jfhdr::kVariants) % jfhdr::kVariants];
}

void fill(std::uint32_t *px, std::uint32_t width, std::uint32_t height, int variant)
{
	const bool blueLow = (variant & 1) != 0;
	const bool pq = (variant & 2) == 0;
	auto enc = [&](double nits) {
		// PQ: absolute nits. Gamma: 100 nits is full scale, 2.2 power (what SDR content would use).
		return pq ? pqEncode(nits) : std::pow(std::min(1.0, nits / 100.0), 1.0 / 2.2);
	};
	auto word = [&](double r, double g, double b) {
		const std::uint32_t R = code10(r), G = code10(g), B = code10(b);
		return blueLow ? (B | (G << 10) | (R << 20) | (3u << 30)) : (R | (G << 10) | (B << 20) | (3u << 30));
	};
	const std::uint32_t black = word(0, 0, 0);
	for (std::uint32_t i = 0; i < width * height; i++) {
		px[i] = black;
	}
	auto rect = [&](std::uint32_t x0, std::uint32_t y0, std::uint32_t x1, std::uint32_t y1, std::uint32_t w) {
		x1 = std::min(x1, width);
		y1 = std::min(y1, height);
		for (std::uint32_t y = y0; y < y1; y++) {
			for (std::uint32_t x = x0; x < x1; x++) {
				px[static_cast<std::size_t>(y) * width + x] = w;
			}
		}
	};
	auto Y = [&](double f) { return static_cast<std::uint32_t>(f * height); };

	// ticks: the variant's number, in white (all ones is white in any layout)
	const std::uint32_t side = Y(0.04), gap = side / 2;
	for (int i = 0; i <= (variant % jfhdr::kVariants); i++) {
		const std::uint32_t x = gap + static_cast<std::uint32_t>(i) * (side + gap);
		rect(x, gap, x + side, gap + side, 0xFFFFFFFFu);
	}

	// colour bars at 203 nits (SDR reference white in PQ): white yellow cyan green magenta red blue, then a grey
	static const int bars[7][3] = { { 1, 1, 1 }, { 1, 1, 0 }, { 0, 1, 1 }, { 0, 1, 0 }, { 1, 0, 1 }, { 1, 0, 0 }, { 0, 0, 1 } };
	const double level = enc(203.0);
	for (int i = 0; i < 7; i++) {
		rect(width * i / 7, Y(0.12), width * (i + 1) / 7, Y(0.45), word(bars[i][0] * level, bars[i][1] * level, bars[i][2] * level));
	}

	// 32 grey steps, signal 0..1 (PQ: up to 10000 nits)
	for (int i = 0; i < 32; i++) {
		const double v = i / 31.0;
		rect(width * i / 32, Y(0.48), width * (i + 1) / 32, Y(0.68), word(v, v, v));
	}

	// peak steps: 100 203 400 600 1000 2000 4000 10000 nits
	static const double nits[8] = { 100, 203, 400, 600, 1000, 2000, 4000, 10000 };
	for (int i = 0; i < 8; i++) {
		const double v = enc(nits[i]);
		rect(width * i / 8, Y(0.72), width * (i + 1) / 8, Y(0.95), word(v, v, v));
	}
}

} // namespace jfhdr_impl

extern "C" {
// The link step has every reference to sceVideoOutOpen come here (--wrap=sceVideoOutOpen).
int __wrap_sceVideoOutOpen(std::int32_t user, std::int32_t bus, std::int32_t index, const void *parameter)
{
	const int handle = __real_sceVideoOutOpen(user, bus, index, parameter);
	g_openCount++;
	if (handle >= 0) {
		g_openedHandle = handle;
	}
	return handle;
}
int __wrap_sceVideoOutRegisterBuffers2(std::int32_t handle, std::int32_t set, std::int32_t start, void *buffers,
                                      std::int32_t count, void *attribute, std::int32_t option, void *reserved)
{
	const int rc = __real_sceVideoOutRegisterBuffers2(handle, set, start, buffers, count, attribute, option, reserved);
	if (rc == 0 && set == 0 && count > 0 && count <= 8 && buffers != nullptr && !g_reg.valid) {
		struct VideoBuffer {
			void *data;
			void *metadata;
			void *reserved[2];
		};
		const VideoBuffer *b = static_cast<const VideoBuffer *>(buffers);
		for (int i = 0; i < count; i++) {
			g_reg.data[i] = b[i].data;
		}
		g_reg.handle = handle;
		g_reg.start = start;
		g_reg.count = count;
		g_reg.valid = true;
	}
	return rc;
}
int __wrap_sceVideoOutSubmitFlip(std::int32_t handle, std::int32_t index, std::uint32_t mode, std::int64_t arg)
{
	const int add = g_redirect.load();
	if (add > 0 && g_reg.valid && index >= g_reg.start && index < g_reg.start + g_reg.count) {
		index += add;
	}
	return __real_sceVideoOutSubmitFlip(handle, index, mode, arg);
}
bool jfhdr_wanted(void) { return jfhdr_impl::wanted(); }
int jfhdr_findHandle(void) { return jfhdr_impl::findHandle(); }
bool jfhdr_setOutput(int h, bool hdr, std::uint32_t w, std::uint32_t ht) { return jfhdr_impl::setOutput(h, hdr, w, ht); }
void jfhdr_logStatus(int h, const char *when) { jfhdr_impl::logStatus(h, when); }
void jfhdr_fill(std::uint32_t *px, std::uint32_t w, std::uint32_t ht, int v) { jfhdr_impl::fill(px, w, ht, v); }
const char *jfhdr_variantName(int v) { return jfhdr_impl::variantName(v); }
double jfhdr_pqEncode(double n) { return jfhdr_impl::pqEncode(n); }
}
