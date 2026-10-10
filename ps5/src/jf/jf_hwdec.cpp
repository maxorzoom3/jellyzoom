/* See jf_hwdec.h. The calls, structures and their order are those of EVO Player / Jelly5 (GPL-3.0-or-later,
 * evo_vdec_native.c), which follow ProsperoLight; the console accepted this sequence in Stage 8's probe. */
#include "jf_hwdec.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <map>
#include <vector>

#include <unistd.h>

extern "C" {
#include "platform.h"
#include <libavutil/dovi_meta.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>
}

extern "C" {
struct vd_config
{
	uint64_t size;
	uint32_t resource_type, codec_type, profile, max_level;
	int32_t max_width, max_height, max_dpb_frames;
	uint32_t pipeline_depth;
	uint64_t compute_queue, cpu_affinity;
	int32_t cpu_priority;
	uint32_t optimize_progressive, check_memory_type, reserved;
};
struct vd_memory
{
	uint64_t size, cpu_size;
	void *cpu;
	uint64_t gpu_size;
	void *gpu;
	uint64_t cpu_gpu_size;
	void *cpu_gpu;
	uint64_t max_frame_size;
	uint32_t frame_alignment, reserved;
};
struct vd_ccfg
{
	uint64_t size;
	uint16_t pipe_id, queue_id;
	uint8_t check_memory_type, r0;
	uint16_t r1;
};
struct vd_cmem
{
	uint64_t size, cpu_gpu_size;
	void *cpu_gpu;
};
struct vd_input
{
	uint64_t size;
	void *au;
	uint64_t au_size, pts, dts, attached;
};
struct vd_frame
{
	uint64_t size;
	void *buffer;
	uint64_t buffer_size;
	uint32_t accepted, reserved;
};
struct vd_output
{
	uint64_t size;
	uint8_t valid, error, picture_count, padding;
	uint32_t codec, width, pitch, height, reserved;
	void *buffer;
	uint64_t buffer_size;
	uint32_t frame_format, pitch_bytes;
};

int sceKernelAllocateDirectMemory(int64_t, int64_t, size_t, size_t, int, int64_t *);
int64_t sceKernelGetDirectMemorySize(void);
int sceKernelMapDirectMemory(void **, size_t, int, int, int64_t, size_t);
int sceKernelReleaseDirectMemory(int64_t, size_t);
int sceKernelMunmap(void *, size_t);
int sceKernelMapNamedFlexibleMemory(void **, size_t, int, int, const char *);
int sceKernelReleaseFlexibleMemory(void *, size_t);
int sceSysmoduleLoadModule(uint16_t id);

int32_t sceVideodec2QueryComputeMemoryInfo(vd_cmem *);
int32_t sceVideodec2AllocateComputeQueue(const vd_ccfg *, const vd_cmem *, void **);
int32_t sceVideodec2ReleaseComputeQueue(void *);
int32_t sceVideodec2QueryDecoderMemoryInfo(const vd_config *, vd_memory *);
int32_t sceVideodec2CreateDecoder(const vd_config *, const vd_memory *, void **);
int32_t sceVideodec2DeleteDecoder(void *);
int32_t sceVideodec2Reset(void *);
int32_t sceVideodec2Decode(void *, vd_input *, vd_frame *, vd_output *);
int32_t sceVideodec2Flush(void *, vd_frame *, vd_output *);
}

namespace jfhw
{
namespace
{
constexpr uint32_t kCodecAvc = 1u, kCodecHevc = 974921u;
constexpr size_t kInputSlot = size_t(8) << 20;
constexpr unsigned kInputSlots = 4, kFrameSlots = 8;
constexpr int kMaxNals = 512;
constexpr int kMaxErrors = 30;

size_t align16k(size_t v)
{
	return (v + 0x3fffu) & ~size_t(0x3fffu);
}

struct Region
{
	void *addr{ nullptr };
	int64_t start{ -1 };
	size_t size{ 0 };
	int grab(size_t bytes, int64_t limit)
	{
		size = bytes;
		int rc = sceKernelAllocateDirectMemory(0, limit, bytes, 0x4000, 12, &start);
		if (rc == 0) {
			rc = sceKernelMapDirectMemory(&addr, bytes, 0x33, 0, start, 0x4000);
		}
		return rc;
	}
	void drop()
	{
		if (addr) {
			sceKernelMunmap(addr, size);
		}
		if (start >= 0) {
			sceKernelReleaseDirectMemory(start, size);
		}
		addr = nullptr;
		start = -1;
	}
};

struct Nal
{
	const uint8_t *p;
	int n;
};

struct Picture
{
	const uint8_t *data;
	int codedH;
	int width, height; // the decoder's own figures
	int pitchBytes;
	bool ten;
	int64_t pts;
};
} // namespace

struct Decoder::Impl
{
	bool up{ false };
	bool hevc{ false };
	bool ten{ false };
	bool failed{ false };
	int nalLen{ 0 }; // bytes of length before each NAL unit; 0: the packets are Annex B already
	std::vector<uint8_t> params; // the file's parameter sets, in Annex B
	int dispW{ 0 }, dispH{ 0 };
	int colorspace{ 0 }, primaries{ 0 }, trc{ 0 }, range{ 0 };
	AVRational sar{ 0, 1 };
	std::vector<uint8_t> mdm, cll;

	Region compute, gpu, cpugpu, input, frame;
	void *queue{ nullptr };
	void *decoder{ nullptr };
	void *cpu{ nullptr };
	size_t cpuMap{ 0 };
	vd_config cfg{};
	vd_memory mem{};
	size_t frameSize{ 0 };
	unsigned ring{ 0 };

	bool needParams{ true }, dropLeading{ true }, needIrap{ false };
	int errors{ 0 };
	unsigned decodes{ 0 }, outputs{ 0 };
	bool firstLogged{ false };
	std::vector<int64_t> ptsPool;
	std::deque<Picture> pictures;
	// where the colour plane starts and how the 10-bit samples sit, found from the first pictures
	bool lowAligned{ false }; // ten bits in the bottom of each 16-bit sample, not the top
	bool uvLocked{ false };
	int uvRow{ 0 };
	std::map<int, int> uvVotes;
	std::vector<uint16_t> scratch;

	~Impl()
	{
		teardown();
	}

	// The memory and the decoder, in the order Stage 8's probe ran them
	int build(const char *&stage, int w, int h)
	{
		int rc = 0;
		const int64_t dm = sceKernelGetDirectMemorySize();
		vd_cmem cm{};
		vd_ccfg cc{};
		cm.size = sizeof cm;
#define STEP(name, call) \
	do { \
		stage = name; \
		rc = (call); \
		if (rc != 0) { \
			return rc; \
		} \
	} while (0)
		STEP("QueryComputeMemoryInfo", sceVideodec2QueryComputeMemoryInfo(&cm));
		STEP("alloc(compute)", compute.grab(align16k(size_t(cm.cpu_gpu_size)), dm));
		cm.cpu_gpu = compute.addr;
		cm.cpu_gpu_size = compute.size;
		cc.size = sizeof cc;
		STEP("AllocateComputeQueue", sceVideodec2AllocateComputeQueue(&cc, &cm, &queue));
		if (!queue) {
			return -1;
		}
		cfg = vd_config{};
		cfg.size = sizeof cfg;
		cfg.resource_type = 1;
		cfg.codec_type = hevc ? kCodecHevc : kCodecAvc;
		cfg.profile = hevc ? (ten ? 2u : 1u) : 100u;
		const bool big = w > 1920 || h > 1088;
		cfg.max_level = hevc ? (big ? 153u : 123u) : (big ? 52u : 51u);
		cfg.max_width = w;
		cfg.max_height = h;
		cfg.max_dpb_frames = -1;
		cfg.pipeline_depth = ten ? 1u : 4u;
		cfg.compute_queue = uint64_t(uintptr_t(queue));
		cfg.cpu_affinity = 0x3f;
		cfg.cpu_priority = 700;
		cfg.optimize_progressive = 1;
		mem = vd_memory{};
		mem.size = sizeof mem;
		STEP("QueryDecoderMemoryInfo", sceVideodec2QueryDecoderMemoryInfo(&cfg, &mem));
		cpuMap = align16k(size_t(mem.cpu_size));
		STEP("MapNamedFlexibleMemory", sceKernelMapNamedFlexibleMemory(&cpu, cpuMap, 0x03, 0, "JfHwDec"));
		mem.cpu = cpu;
		frameSize = align16k(size_t(mem.max_frame_size));
		if (frameSize == 0) {
			return -1;
		}
		STEP("alloc(gpu)", gpu.grab(align16k(size_t(mem.gpu_size)), dm));
		mem.gpu = gpu.addr;
		mem.gpu_size = gpu.size;
		if (mem.cpu_gpu_size) {
			STEP("alloc(cpu_gpu)", cpugpu.grab(align16k(size_t(mem.cpu_gpu_size)), dm));
			mem.cpu_gpu = cpugpu.addr;
			mem.cpu_gpu_size = cpugpu.size;
		}
		STEP("alloc(input)", input.grab(kInputSlot * kInputSlots, dm));
		STEP("alloc(frame)", frame.grab(frameSize * kFrameSlots, dm));
		STEP("CreateDecoder", sceVideodec2CreateDecoder(&cfg, &mem, &decoder));
		if (!decoder) {
			return -1;
		}
		STEP("Reset", sceVideodec2Reset(decoder));
#undef STEP
		stage = "ok";
		return 0;
	}

	void teardown()
	{
		if (decoder) {
			sceVideodec2DeleteDecoder(decoder);
			decoder = nullptr;
		}
		frame.drop();
		input.drop();
		cpugpu.drop();
		gpu.drop();
		if (cpu) {
			sceKernelReleaseFlexibleMemory(cpu, cpuMap);
			sceKernelMunmap(cpu, cpuMap);
			cpu = nullptr;
		}
		if (queue) {
			sceVideodec2ReleaseComputeQueue(queue);
			queue = nullptr;
		}
		compute.drop();
		up = false;
	}

	// A fresh decoder on the same memory: what a seek needs (a Reset in mid-stream can leave some 4K streams stuck)
	bool renew()
	{
		if (decoder) {
			sceVideodec2DeleteDecoder(decoder);
			decoder = nullptr;
		}
		vd_memory m = mem;
		if (sceVideodec2CreateDecoder(&cfg, &m, &decoder) != 0 || !decoder) {
			decoder = nullptr;
			return false;
		}
		return sceVideodec2Reset(decoder) == 0;
	}

	int nalType(const uint8_t *p) const
	{
		return hevc ? (p[0] >> 1) & 0x3f : p[0] & 0x1f;
	}

	int splitAnnexB(const uint8_t *d, int size, Nal *out) const
	{
		int count = 0, pos = 0;
		while (pos + 3 <= size) {
			if (d[pos] == 0 && d[pos + 1] == 0 && d[pos + 2] == 1) {
				const int st = pos + 3;
				int j = st;
				while (j + 3 <= size && !(d[j] == 0 && d[j + 1] == 0 && d[j + 2] == 1)) {
					j++;
				}
				const bool more = j + 3 <= size;
				int end = more ? j : size;
				while (end > st && d[end - 1] == 0) {
					end--;
				}
				if (end > st && count < kMaxNals) {
					out[count++] = { d + st, end - st };
				}
				pos = more ? j : size;
			} else {
				pos++;
			}
		}
		return count;
	}

	int splitLengths(const uint8_t *d, int size, Nal *out) const
	{
		int count = 0, pos = 0;
		while (pos + nalLen <= size) {
			uint32_t len = 0;
			for (int k = 0; k < nalLen; k++) {
				len = (len << 8) | d[pos + k];
			}
			pos += nalLen;
			if (len == 0 || pos + static_cast<int>(len) > size) {
				break;
			}
			if (count < kMaxNals) {
				out[count++] = { d + pos, static_cast<int>(len) };
			}
			pos += static_cast<int>(len);
		}
		return count;
	}

	void takePictureFrom(const vd_output &out, int64_t pts)
	{
		if (!out.buffer) {
			return;
		}
		Picture p;
		p.data = static_cast<const uint8_t *>(out.buffer);
		p.width = static_cast<int>(out.width);
		p.height = static_cast<int>(out.height);
		p.codedH = static_cast<int>(out.height);
		p.pitchBytes = static_cast<int>(out.pitch_bytes ? out.pitch_bytes : out.pitch);
		p.ten = out.pitch_bytes && out.pitch_bytes >= out.pitch * 2u;
		p.pts = pts;
		if (!firstLogged) {
			firstLogged = true;
			say("HWDEC: first picture %ux%u pitch %u pitch_bytes %u format %u error %u (%s)", out.width, out.height,
				out.pitch, out.pitch_bytes, out.frame_format, out.error, p.ten ? "10-bit" : "8-bit");
			if (p.ten) {
				// where the ten bits sit in each 16-bit sample: the top (P010) or the bottom
				unsigned lo = 0xffff, hi = 0, orAll = 0;
				for (int y = 0; y < p.height; y += 37) {
					const uint16_t *row = reinterpret_cast<const uint16_t *>(p.data + size_t(y) * p.pitchBytes);
					for (int x = 0; x < p.width; x += 41) {
						lo = std::min<unsigned>(lo, row[x]);
						hi = std::max<unsigned>(hi, row[x]);
						orAll |= row[x];
					}
				}
				lowAligned = (orAll & 0x3f) != 0 || hi <= 0x3ff; // a black first picture has no low bits set but never exceeds 0x3ff
				say("HWDEC: 10-bit luma samples range 0x%04x..0x%04x, bits used 0x%04x: %s", lo, hi, orAll,
					lowAligned ? "in the bottom ten bits, shifted up into P010 for the converter" : "in the top ten bits (P010)");
			}
		}
		pictures.push_back(p);
	}

	int64_t takePts()
	{
		if (ptsPool.empty()) {
			return AV_NOPTS_VALUE;
		}
		size_t m = 0;
		for (size_t i = 1; i < ptsPool.size(); i++) {
			if (ptsPool[i] != AV_NOPTS_VALUE && (ptsPool[m] == AV_NOPTS_VALUE || ptsPool[i] < ptsPool[m])) {
				m = i;
			}
		}
		const int64_t v = ptsPool[m];
		ptsPool[m] = ptsPool.back();
		ptsPool.pop_back();
		return v;
	}

	// Does the plane that starts at luma row `c` look like colour (values near the middle)?
	bool chromaLike(const Picture &p, int c, int w, int h) const
	{
		const int rows = (h + 1) / 2;
		int far = 0, n = 0;
		const int reach = p.ten ? 160 : 40; // how far from the middle still counts as colour
		for (int r = 0; r < 16; r++) {
			const int row = (rows - 1) * r / 15;
			const uint8_t *rp = p.data + size_t(p.pitchBytes) * size_t(c + row);
			for (int k = 0; k < 16; k++) {
				const int i = ((w / 2) * k / 16) * 2;
				for (int j = 0; j < 2; j++) {
					int v;
					if (p.ten) {
						uint16_t smp;
						std::memcpy(&smp, rp + 2 * (i + j), 2);
						v = (lowAligned ? smp : smp >> 6) - 512;
					} else {
						v = rp[i + j] - 128;
					}
					far += std::abs(v) > reach;
					n++;
				}
			}
		}
		return far * 20 <= n; // all but a few samples near the middle: colour, not picture or padding
	}

	// The row the colour plane starts at. The decoder's own height is not always it (a 1608-line film), so the
	// first pictures decide: the smallest candidate whose plane looks like colour, three pictures running.
	int chooseChroma(const Picture &p, int w, int h)
	{
		if (uvLocked) {
			return uvRow;
		}
		const int hs = p.codedH;
		int cands[] = { hs, (hs + 7) & ~7, (hs + 15) & ~15, (hs + 31) & ~31, (hs + 63) & ~63, (hs + 127) & ~127, cfg.max_height };
		std::sort(std::begin(cands), std::end(cands));
		int best = -1;
		int last = -1;
		for (int c : cands) {
			if (c == last || c < hs) {
				continue;
			}
			last = c;
			if (size_t(c + (h + 1) / 2) * size_t(p.pitchBytes) > frameSize) {
				continue;
			}
			if (chromaLike(p, c, w, h)) {
				best = c;
				break;
			}
		}
		if (best >= 0 && ++uvVotes[best] >= 3) {
			uvLocked = true;
			uvRow = best;
			say("HWDEC: the colour plane starts at row %d (the decoder reports %d rows)", best, hs);
		}
		if (!uvLocked && ++uvTries > 90) {
			uvLocked = true;
			uvRow = hs;
			say("HWDEC: could not tell where the colour plane starts, using row %d", hs);
		}
		return best >= 0 ? best : hs;
	}
	int uvTries{ 0 };

	void drain()
	{
		for (int guard = 0; guard < 64; guard++) {
			const unsigned slot = ring++ % kFrameSlots;
			vd_frame fb{};
			vd_output out{};
			fb.size = sizeof fb;
			fb.buffer = static_cast<uint8_t *>(frame.addr) + size_t(slot) * frameSize;
			fb.buffer_size = frameSize;
			out.size = sizeof out;
			if (sceVideodec2Flush(decoder, &fb, &out) != 0 || !(out.valid && out.picture_count)) {
				break;
			}
			takePictureFrom(out, takePts());
		}
	}

	bool submit(const AVPacket *pkt)
	{
		if (pkt->size <= 0 || !pkt->data) {
			return true;
		}
		Nal nals[kMaxNals];
		const int count = nalLen > 0 ? splitLengths(pkt->data, pkt->size, nals) : splitAnnexB(pkt->data, pkt->size, nals);
		if (count == 0) {
			return true;
		}
		bool hasParams = false;
		int vcl = -1;
		for (int i = 0; i < count; i++) {
			const int t = nalType(nals[i].p);
			if (hevc ? (t >= 32 && t <= 34) : (t == 7 || t == 8)) {
				hasParams = true;
			}
			if (vcl < 0 && (hevc ? t < 32 : (t >= 1 && t <= 5))) {
				vcl = t;
			}
		}
		const bool irap = hevc && vcl >= 16 && vcl <= 23;
		if (hevc) {
			// pictures before the first key frame that lean on what was never decoded
			if (dropLeading && (vcl == 8 || vcl == 9)) {
				return true;
			}
			if (vcl >= 0 && vcl <= 5) {
				dropLeading = false;
			}
			if (needIrap && vcl >= 0 && !irap) {
				return true;
			}
		}
		const unsigned islot = ring % kInputSlots;
		uint8_t *dst = static_cast<uint8_t *>(input.addr) + size_t(islot) * kInputSlot;
		size_t at = 0;
		static const uint8_t start[4] = { 0, 0, 0, 1 };
		if (needParams && !hasParams && !params.empty() && params.size() < kInputSlot) {
			std::memcpy(dst, params.data(), params.size());
			at = params.size();
		}
		for (int i = 0; i < count; i++) {
			const int t = nalType(nals[i].p);
			if (hevc ? t >= 48 : (t == 14 || t == 15 || t == 20)) {
				continue; // Dolby Vision RPU and enhancement layer, MVC / SVC extensions: the base layer only
			}
			if (at + 4 + size_t(nals[i].n) > kInputSlot) {
				say("HWDEC: a %d-byte picture does not fit the input slot, skipped", pkt->size);
				return true;
			}
			std::memcpy(dst + at, start, 4);
			std::memcpy(dst + at + 4, nals[i].p, size_t(nals[i].n));
			at += 4 + size_t(nals[i].n);
		}
		if (at == 0) {
			return true;
		}
		needParams = false;
		const unsigned fslot = ring % kFrameSlots;
		ring++;

		vd_input in{};
		vd_frame fb{};
		vd_output out{};
		in.size = sizeof in;
		in.au = dst;
		in.au_size = at;
		const int64_t pts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
		in.pts = uint64_t(pts);
		in.dts = UINT64_MAX;
		fb.size = sizeof fb;
		fb.buffer = static_cast<uint8_t *>(frame.addr) + size_t(fslot) * frameSize;
		fb.buffer_size = frameSize;
		out.size = sizeof out;
		const int rc = sceVideodec2Decode(decoder, &in, &fb, &out);
		decodes++;
		if (decodes <= 3) {
			say("HWDEC: Decode #%u rc=0x%08x accepted=%u valid=%u error=%u %ux%u bytes=%zu", decodes, unsigned(rc),
				fb.accepted, out.valid, out.error, out.width, out.height, at);
		}
		if (irap) {
			needIrap = rc != 0;
		}
		if (rc != 0) {
			if (errors == 0 || errors % 10 == 0) {
				say("HWDEC: Decode refused rc=0x%08x (%d in a row, %u pictures so far)", unsigned(rc), errors + 1, outputs);
			}
			if (++errors > kMaxErrors) {
				return false;
			}
			return true;
		}
		errors = 0;
		ptsPool.push_back(pts);
		if (ptsPool.size() > 64) {
			ptsPool.erase(ptsPool.begin());
		}
		if (out.valid && out.picture_count) {
			takePictureFrom(out, takePts());
		}
		if (decodes % 600 == 0) {
			say("HWDEC: %u packets in, %u pictures out", decodes, outputs);
		}
		return true;
	}
};

Decoder::Decoder() : impl_(new Impl) {}

Decoder::~Decoder()
{
	delete impl_;
}

bool Decoder::active() const
{
	return impl_->up && !impl_->failed;
}

bool Decoder::open(const AVCodecContext *ctx)
{
	Impl &d = *impl_;
	if (access("/data/jf-hw-off", F_OK) == 0) {
		say("HWDEC: switched off (/data/jf-hw-off)");
		return false;
	}
	if (!ctx || ctx->width <= 0 || ctx->height <= 0) {
		return false;
	}
	d.hevc = ctx->codec_id == AV_CODEC_ID_HEVC;
	if (!d.hevc && ctx->codec_id != AV_CODEC_ID_H264) {
		say("HWDEC: %s stays on the software decoder", avcodec_get_name(ctx->codec_id));
		return false;
	}
	const AVPixelFormat pf = ctx->pix_fmt;
	if (d.hevc) {
		if (ctx->profile == AV_PROFILE_HEVC_MAIN_10 || pf == AV_PIX_FMT_YUV420P10LE) {
			d.ten = true;
		} else if (ctx->profile == AV_PROFILE_HEVC_MAIN || pf == AV_PIX_FMT_YUV420P) {
			d.ten = false;
		} else {
			say("HWDEC: HEVC profile %d (pixel format %d) stays on the software decoder", ctx->profile, int(pf));
			return false;
		}
	} else if (pf != AV_PIX_FMT_NONE && pf != AV_PIX_FMT_YUV420P && pf != AV_PIX_FMT_YUVJ420P) {
		say("HWDEC: H.264 in %s stays on the software decoder", av_get_pix_fmt_name(pf));
		return false;
	}
	if (ctx->width > 3840 || ctx->height > 2176) {
		say("HWDEC: %dx%d is bigger than the decoder takes", ctx->width, ctx->height);
		return false;
	}
	d.dispW = ctx->width;
	d.dispH = ctx->height;
	d.colorspace = ctx->colorspace;
	d.primaries = ctx->color_primaries;
	d.trc = ctx->color_trc;
	d.range = ctx->color_range;
	d.sar = ctx->sample_aspect_ratio;

	// parameter sets: avcC / hvcC in the file's header, else they are in the stream
	const uint8_t *x = ctx->extradata;
	const int xs = ctx->extradata_size;
	static const uint8_t start[4] = { 0, 0, 0, 1 };
	if (x && xs >= 7 && x[0] == 1) {
		if (d.hevc && xs >= 23) {
			d.nalLen = (x[21] & 3) + 1;
			int pos = 23;
			for (int a = 0; a < x[22] && pos + 3 <= xs; a++) {
				const int count = (x[pos + 1] << 8) | x[pos + 2];
				pos += 3;
				for (int i = 0; i < count && pos + 2 <= xs; i++) {
					const int len = (x[pos] << 8) | x[pos + 1];
					pos += 2;
					if (pos + len > xs) {
						break;
					}
					d.params.insert(d.params.end(), start, start + 4);
					d.params.insert(d.params.end(), x + pos, x + pos + len);
					pos += len;
				}
			}
		} else if (!d.hevc) {
			d.nalLen = (x[4] & 3) + 1;
			int pos = 5;
			for (int set = 0; set < 2 && pos < xs; set++) {
				const int count = set == 0 ? (x[pos++] & 31) : x[pos++];
				for (int i = 0; i < count && pos + 2 <= xs; i++) {
					const int len = (x[pos] << 8) | x[pos + 1];
					pos += 2;
					if (pos + len > xs) {
						break;
					}
					d.params.insert(d.params.end(), start, start + 4);
					d.params.insert(d.params.end(), x + pos, x + pos + len);
					pos += len;
				}
			}
		}
	}
	for (int i = 0; i < ctx->nb_coded_side_data; i++) {
		const AVPacketSideData &sd = ctx->coded_side_data[i];
		if (sd.type == AV_PKT_DATA_MASTERING_DISPLAY_METADATA && sd.size >= sizeof(AVMasteringDisplayMetadata)) {
			d.mdm.assign(sd.data, sd.data + sizeof(AVMasteringDisplayMetadata));
		} else if (sd.type == AV_PKT_DATA_CONTENT_LIGHT_LEVEL && sd.size >= sizeof(AVContentLightMetadata)) {
			d.cll.assign(sd.data, sd.data + sizeof(AVContentLightMetadata));
		} else if (sd.type == AV_PKT_DATA_DOVI_CONF && sd.size >= sizeof(AVDOVIDecoderConfigurationRecord)) {
			const AVDOVIDecoderConfigurationRecord *c = reinterpret_cast<const AVDOVIDecoderConfigurationRecord *>(sd.data);
			say("HWDEC: Dolby Vision profile %d level %d, rpu %d, enhancement layer %d, base layer %d, compatibility %d",
				c->dv_profile, c->dv_level, c->rpu_present_flag, c->el_present_flag, c->bl_present_flag,
				c->dv_bl_signal_compatibility_id);
		}
	}

	const int rc0 = sceSysmoduleLoadModule(207);
	const bool big = ctx->width > 1920 || ctx->height > 1088;
	const int w = big ? 3840 : 1920, h = big ? 2176 : 1088;
	const char *stage = "start";
	say("HWDEC: %s %s %dx%d, %s parameter sets, decoder %dx%d (module 0x%08x)", d.hevc ? "HEVC" : "H.264",
		d.ten ? "10-bit" : "8-bit", ctx->width, ctx->height, d.nalLen ? "file-header" : "in-stream", w, h,
		unsigned(rc0));
	const int rc = d.build(stage, w, h);
	if (rc != 0) {
		say("HWDEC: bring-up FAILED at [%s] rc=0x%08x: the software decoder takes this film", stage, unsigned(rc));
		d.teardown();
		return false;
	}
	d.up = true;
	say("HWDEC: decoder up (frame %zuK, flexible %zuK, direct %zuK)", d.frameSize >> 10, d.cpuMap >> 10,
		(d.compute.size + d.gpu.size + d.cpugpu.size + d.input.size + d.frame.size) >> 10);
	return true;
}

bool Decoder::send(const AVPacket *pkt)
{
	Impl &d = *impl_;
	if (!d.up || d.failed) {
		return false;
	}
	if (!pkt) {
		d.drain();
		return true;
	}
	if (!d.submit(pkt)) {
		d.failed = true;
		say("HWDEC: too many refused pictures, handing over to the software decoder");
		return false;
	}
	return true;
}

bool Decoder::receive(AVFrame *fr)
{
	Impl &d = *impl_;
	if (d.pictures.empty()) {
		return false;
	}
	const Picture p = d.pictures.front();
	d.pictures.pop_front();
	d.outputs++;
	av_frame_unref(fr);
	fr->format = p.ten ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
	fr->width = std::min(d.dispW, p.width);
	fr->height = std::min(d.dispH, p.height);
	const int uvRow = d.chooseChroma(p, fr->width, fr->height);
	const uint8_t *yPlane = p.data;
	const uint8_t *uvPlane = p.data + size_t(p.pitchBytes) * size_t(uvRow);
	if (p.ten && d.lowAligned) {
		// ten bits in the bottom of each sample: shift them up so the picture is plain P010
		const int w = fr->width, h = fr->height, rows = (h + 1) / 2;
		const size_t lumaSamples = size_t(w) * size_t(h), chromaSamples = size_t(w) * size_t(rows);
		d.scratch.resize(lumaSamples + chromaSamples);
		uint16_t *out = d.scratch.data();
		for (int y = 0; y < h; y++) {
			const uint16_t *src = reinterpret_cast<const uint16_t *>(yPlane + size_t(y) * size_t(p.pitchBytes));
			uint16_t *dst = out + size_t(y) * size_t(w);
			for (int x = 0; x < w; x++) {
				dst[x] = static_cast<uint16_t>(src[x] << 6);
			}
		}
		for (int y = 0; y < rows; y++) {
			const uint16_t *src = reinterpret_cast<const uint16_t *>(uvPlane + size_t(y) * size_t(p.pitchBytes));
			uint16_t *dst = out + lumaSamples + size_t(y) * size_t(w);
			for (int x = 0; x < w; x++) {
				dst[x] = static_cast<uint16_t>(src[x] << 6);
			}
		}
		fr->data[0] = reinterpret_cast<uint8_t *>(out);
		fr->linesize[0] = w * 2;
		fr->data[1] = reinterpret_cast<uint8_t *>(out + lumaSamples);
		fr->linesize[1] = w * 2;
	} else {
		fr->data[0] = const_cast<uint8_t *>(yPlane);
		fr->linesize[0] = p.pitchBytes;
		fr->data[1] = const_cast<uint8_t *>(uvPlane);
		fr->linesize[1] = p.pitchBytes;
	}
	fr->pts = p.pts;
	fr->best_effort_timestamp = p.pts;
	fr->colorspace = static_cast<AVColorSpace>(d.colorspace);
	fr->color_primaries = static_cast<AVColorPrimaries>(d.primaries);
	fr->color_trc = static_cast<AVColorTransferCharacteristic>(d.trc);
	fr->color_range = static_cast<AVColorRange>(d.range);
	fr->sample_aspect_ratio = d.sar;
	if (!d.mdm.empty()) {
		if (AVFrameSideData *sd = av_frame_new_side_data(fr, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA, int(d.mdm.size()))) {
			std::memcpy(sd->data, d.mdm.data(), d.mdm.size());
		}
	}
	if (!d.cll.empty()) {
		if (AVFrameSideData *sd = av_frame_new_side_data(fr, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL, int(d.cll.size()))) {
			std::memcpy(sd->data, d.cll.data(), d.cll.size());
		}
	}
	return true;
}

void Decoder::flush()
{
	Impl &d = *impl_;
	if (!d.up) {
		return;
	}
	d.pictures.clear();
	d.ptsPool.clear();
	d.ring = 0;
	d.needParams = true;
	d.dropLeading = true;
	d.needIrap = false;
	d.errors = 0;
	if (!d.renew()) {
		d.failed = true;
		say("HWDEC: could not renew the decoder after a seek, handing over to the software decoder");
	}
}

void Decoder::close()
{
	Impl &d = *impl_;
	if (d.up) {
		say("HWDEC: closed after %u packets, %u pictures", d.decodes, d.outputs);
	}
	d.teardown();
}
} // namespace jfhw
