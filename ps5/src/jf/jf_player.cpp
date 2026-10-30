/* The film player. Two threads: the demux thread reads the file (through jf_avio's sockets), decodes the
 * sound and keeps the audio ring full; the video thread decodes the pictures, scales them to RGBA and
 * queues them with their times. The render thread asks present() for the picture due at the audio clock.
 * Every log line starts with PLAYER. */
#include "jf_player.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "jf_avio.h"
#include "jf_tonemap.hpp"
#include "jf_subimage.hpp"
#include "jf_subtitle_text.hpp"
#include "jf_hdrsub.hpp"
#include "jf_hwdec.h"
#include "jf_av1.hpp"
#include "jf_gate.h"
#include "jf_gate_private.h"
#include "jf_iec61937.hpp"
#include "jf_mat.hpp"
#include "jf_conf.hpp"
#include "jf_present_metrics.h"

extern "C" {
#include "platform.h"
#include "jf_config.h"
#include "jf_types.h"
#include "jellyfin.h"
#include "http_client.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

extern "C" {
// Stage 17: the console's compressed-audio calls, bound by symbol name (strong references, so the link imports them)
int jf_sce_ex_open(int32_t, int32_t) __asm__("sceAudioOutExOpen");
int jf_sce_ex_close(int32_t) __asm__("sceAudioOutExClose");
int jf_sce_ex_configure(int32_t, uint32_t, int32_t, int32_t, uint64_t) __asm__("sceAudioOutExConfigureOutput");
int jf_sce_out_output(int32_t, const void *) __asm__("sceAudioOutOutput");
int jf_sce_sys_monitor_info(int32_t, void *, uint32_t) __asm__("sceAudioOutSysGetHdmiMonitorInfo");
}

namespace jfp
{
namespace
{
using Clock = std::chrono::steady_clock;

constexpr int kRate = 48000;
constexpr int kMaxW = 1920;
constexpr int kMaxH = 1080;
constexpr size_t kQueueFrames = 6;
constexpr size_t kRingFrames = kRate * 4;          // four seconds of sound
constexpr size_t kPacketBytes = 40u * 1024u * 1024u; // compressed video waiting for the decoder

struct VFrame
{
	std::vector<std::uint8_t> rgba; // RGBA8, or RGBA16 when hdr
	int w{ 0 };
	int h{ 0 };
	double pts{ 0.0 };
	bool hdr{ false };
};

struct PacketItem
{
	AVPacket *pkt{ nullptr }; // null with flush=false: the end of the file, drain the decoder
	unsigned gen{ 0 };
	bool flush{ false };
};

void sleepMs(int ms)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

struct Cue
{
	double start;
	double end;
	std::string text;
};

// What the subtitle fetch brings. Shared with a thread that may outlive the track that asked for it.
struct SubState
{
	std::mutex m;
	std::vector<Cue> cues;
	std::string status;
	std::atomic<bool> stop{ false };
};

std::string stripMarkup(const std::string &in)
{
	std::string out;
	int angle = 0, brace = 0;
	for (size_t i = 0; i < in.size(); i++) {
		const char c = in[i];
		if (c == '<') { angle++; continue; }
		if (c == '>' && angle > 0) { angle--; continue; }
		if (c == '{') { brace++; continue; }
		if (c == '}' && brace > 0) { brace--; continue; }
		if (angle == 0 && brace == 0) {
			out += c;
		}
	}
	const struct { const char *from; const char *to; } entities[] = { { "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" },
		{ "&quot;", "\"" }, { "&nbsp;", " " } };
	for (const auto &e : entities) {
		size_t at = 0;
		while ((at = out.find(e.from, at)) != std::string::npos) {
			out.replace(at, std::strlen(e.from), e.to);
			at += std::strlen(e.to);
		}
	}
	return jfsubtext::withoutFormattingMarks(out);
}

// SRT: numbered blocks of "00:01:02,345 --> 00:01:05,000" and one or two lines of text
void parseSrt(const std::string &text, std::vector<Cue> &out)
{
	size_t pos = 0;
	if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
	    static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
		pos = 3;
	}
	bool inCue = false;
	Cue cue{ 0.0, 0.0, std::string() };
	while (pos <= text.size()) {
		size_t end = text.find('\n', pos);
		if (end == std::string::npos) {
			end = text.size();
		}
		std::string line = text.substr(pos, end - pos);
		pos = end + 1;
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
			line.pop_back();
		}
		if (line.find("-->") != std::string::npos) {
			if (inCue && !cue.text.empty()) {
				out.push_back(cue);
			}
			int h1, m1, s1, ms1, h2, m2, s2, ms2;
			if (std::sscanf(line.c_str(), "%d:%d:%d%*[,.]%d --> %d:%d:%d%*[,.]%d", &h1, &m1, &s1, &ms1, &h2, &m2, &s2, &ms2) == 8) {
				cue.start = h1 * 3600.0 + m1 * 60.0 + s1 + ms1 / 1000.0;
				cue.end = h2 * 3600.0 + m2 * 60.0 + s2 + ms2 / 1000.0;
				cue.text.clear();
				inCue = true;
			} else {
				inCue = false;
			}
		} else if (line.empty()) {
			if (inCue && !cue.text.empty()) {
				out.push_back(cue);
			}
			inCue = false;
			cue.text.clear();
		} else if (inCue) {
			const std::string clean = stripMarkup(line);
			if (!clean.empty()) {
				cue.text += (cue.text.empty() ? "" : "\n") + clean;
			}
		}
	}
	if (inCue && !cue.text.empty()) {
		out.push_back(cue);
	}
	std::stable_sort(out.begin(), out.end(), [](const Cue &a, const Cue &b) { return a.start < b.start; });
}

const char *channelText(int channels)
{
	switch (channels) {
	case 1: return "1.0";
	case 2: return "2.0";
	case 3: return "2.1";
	case 6: return "5.1";
	case 7: return "6.1";
	case 8: return "7.1";
	default: return "";
	}
}
// ---- Stage 36: sound and subtitle languages chosen in Settings ----
std::string prefText(const char *path)
{
	std::string out;
	if (FILE *f = std::fopen(path, "rb")) {
		char buf[64];
		const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
		out.assign(buf, n);
		std::fclose(f);
	}
	while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) {
		out.pop_back();
	}
	for (char &c : out) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return out;
}

// Files tag languages as "eng", "fre" or "fra", "en" ...: the same language under several spellings
bool sameLanguage(const std::string &a, const std::string &b)
{
	static const char *const groups[][3] = { { "eng", "en", "" }, { "fre", "fra", "fr" }, { "ger", "deu", "de" },
		{ "spa", "es", "" }, { "ita", "it", "" }, { "por", "pt", "" }, { "dut", "nld", "nl" }, { "jpn", "ja", "" },
		{ "kor", "ko", "" }, { "chi", "zho", "zh" }, { "rus", "ru", "" }, { "pol", "pl", "" }, { "swe", "sv", "" } };
	if (a.empty() || b.empty()) {
		return false;
	}
	if (a == b) {
		return true;
	}
	for (const auto &g : groups) {
		bool hasA = false, hasB = false;
		for (const char *code : g) {
			hasA = hasA || (code[0] && a == code);
			hasB = hasB || (code[0] && b == code);
		}
		if (hasA && hasB) {
			return true;
		}
	}
	return false;
}

std::string streamLanguage(const AVStream *s)
{
	AVDictionaryEntry *e = av_dict_get(s->metadata, "language", nullptr, 0);
	std::string out = e && e->value ? e->value : "";
	for (char &c : out) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return out;
}

// The first stream of a kind in a language; for subtitles a full set is preferred to the forced-only one
int streamByLanguage(AVFormatContext *fc, AVMediaType type, const std::string &language)
{
	if (language.empty()) {
		return -1;
	}
	int forced = -1;
	for (unsigned i = 0; i < fc->nb_streams; i++) {
		const AVStream *s = fc->streams[i];
		if (s->codecpar->codec_type != type || !sameLanguage(streamLanguage(s), language)) {
			continue;
		}
		if (type == AVMEDIA_TYPE_SUBTITLE && (s->disposition & AV_DISPOSITION_FORCED)) {
			if (forced < 0) {
				forced = static_cast<int>(i);
			}
			continue;
		}
		return static_cast<int>(i);
	}
	return forced;
}
} // namespace

std::string Info::audioText() const
{
	std::string text = audioCodec;
	if (!audioProfile.empty()) {
		text = audioProfile;
	}
	if (audioProfile.empty()) {
		for (char &c : text) {
			c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		}
	}
	if (audioCodec == "eac3") {
		text = "Dolby Digital Plus";
	} else if (audioCodec == "ac3") {
		text = "Dolby Digital";
	} else if (audioCodec == "truehd") {
		text = "Dolby TrueHD";
	} else if (audioCodec == "dts" && audioProfile.empty()) {
		text = "DTS";
	}
	if (atmos) {
		text += " Atmos";
	}
	const char *layout = channelText(audioChannels);
	if (*layout) {
		text += std::string(" ") + layout;
	}
	return text;
}

struct Player::Impl
{
	// ---- the session ----
	std::atomic<int> phase{ 0 }; // 0 idle, 1 opening, 2 running, 3 ended, 4 failed
	std::atomic<bool> gate{ false }; // the first picture of this position is queued
	std::atomic<bool> presentationReady{ true };
	std::atomic<bool> stopFlag{ false };
	std::atomic<bool> pausedFlag{ false };
	std::atomic<long long> seekMs{ -1 };
	std::atomic<unsigned> generation{ 0 };
	std::atomic<double> skipUntil{ 0.0 };
	std::atomic<double> durationSec{ 0.0 };
	std::atomic<float> volume{ 1.0f };
	std::atomic<bool> muted{ false };
	std::atomic<bool> hasAudio{ false };
	std::atomic<bool> eof{ false };

	mutable std::mutex textLock;
	std::string errorText, titleText;
	std::vector<Chapter> chapterList;

	std::thread demux, video;

	// ---- sound track and subtitle choices ----
	std::atomic<int> wantAudio{ -1 };    // chosen before the file opened
	std::atomic<int> pendingAudio{ -1 }; // chosen while it plays: the demux thread switches
	std::atomic<int> curAudio{ -1 };
	std::atomic<int> curSub{ -1 };
	std::atomic<int> wantSub{ -1 };
	std::atomic<int> pendingSub{ -2 }; // chosen while it plays: the demux thread applies it (-2: nothing waits)
	// Picture subtitles (PGS, DVD, DVB): decoded by the demux thread, burnt into the pictures by the video thread
	std::atomic<int> bitmapStream{ -1 };
	std::atomic<bool> hdrOut{ false }; // HDR10 pictures go out as 16-bit PQ
	AVCodecContext *subctx{ nullptr };
	std::mutex bitmapLock;
	struct PicEvent
	{
		double start{ 0.0 }, end{ 0.0 };
		int x{ 0 }, y{ 0 }, w{ 0 }, h{ 0 }, cw{ 0 }, ch{ 0 };
		std::uint64_t id{ 0 };
		std::shared_ptr<const std::vector<std::uint8_t>> rgba;
	};
	std::deque<PicEvent> bitmaps; // Stage 37: the app draws these
	std::uint64_t bitmapSerial{ 0 };
	std::atomic<long long> lateTotal{ 0 }; // pictures dropped because the decoder fell behind
	std::mutex subLock;
	std::shared_ptr<SubState> sub;
	std::string itemId, mediaSource, noteText;
	Info infoData;

	// ---- the sound ring and the clock ----
	std::mutex audioLock;
	std::vector<std::int16_t> ring;
	int ringCh{ 2 };                    // channels in the ring: 2, or 8 (7.1)
	std::atomic<int> outChannels{ 2 };  // what the console output must be opened with for this sound
	std::atomic<bool> surround{ true }; // 5.1 / 7.1 sound goes out as 7.1 instead of being mixed down to stereo
	size_t ringRead{ 0 };
	size_t ringCount{ 0 }; // samples (two a frame)
	bool baseValid{ false };
	double basePts{ 0.0 };
	long long consumed{ 0 }; // frames played since basePts
	double seekTarget{ 0.0 };
	double wall{ 0.0 }; // the clock of a film with no sound track
	Clock::time_point wallTick{ Clock::now() };

	// ---- pictures ----
	std::mutex videoLock;
	std::deque<VFrame> frames;
	std::vector<VFrame> pool;
	VFrame shown;
	std::uint64_t serial{ 0 };
	PresentMetrics presentMetrics;
	unsigned presentGeneration{ 0 };

	// ---- packets for the video thread ----
	std::mutex packetLock;
	std::condition_variable packetCv;
	std::deque<PacketItem> packets;
	size_t packetBytes{ 0 };

	Impl()
	{
		ring.resize(kRingFrames * 8);
		surround = access("/data/jellyzoom-stereo", F_OK) != 0; // a file there keeps the sound stereo
	}

	void fail(const std::string &why)
	{
		say("PLAYER: failed: %s", why.c_str());
		{
			std::lock_guard<std::mutex> lock(textLock);
			errorText = why;
		}
		phase = 4;
	}

	void resetSession()
	{
		stopFlag = false;
		pausedFlag = false;
		seekMs = -1;
		generation++;
		skipUntil = 0.0;
		durationSec = 0.0;
		hasAudio = false;
		eof = false;
		gate = false;
		lateTotal = 0;
		pendingAudio = -1;
		curAudio = -1;
		{
			std::lock_guard<std::mutex> lock(subLock);
			if (sub) {
				sub->stop = true;
			}
			sub.reset();
		}
		curSub = -1;
		pendingSub = -2;
		bitmapStream = -1;
		{
			std::lock_guard<std::mutex> lock(bitmapLock);
			bitmaps.clear();
		}
		{
			std::lock_guard<std::mutex> lock(textLock);
			noteText.clear();
			itemId.clear();
			mediaSource.clear();
			infoData = Info();
			errorText.clear();
			titleText.clear();
			chapterList.clear();
		}
		{
			std::lock_guard<std::mutex> lock(audioLock);
			ringRead = ringCount = 0;
			baseValid = false;
			consumed = 0;
			seekTarget = 0.0;
			ringCh = 2;
			outChannels = 2;
			wall = 0.0;
			wallTick = Clock::now();
		}
		clearFrames();
		clearPackets();
		shown = VFrame();
		serial = 0;
		presentMetrics.reset();
		presentGeneration = generation.load();
	}

	void recycle(VFrame &&f)
	{
		if (pool.size() < kQueueFrames + 2 && !f.rgba.empty()) {
			pool.push_back(std::move(f));
		}
	}

	void clearFrames()
	{
		std::lock_guard<std::mutex> lock(videoLock);
		while (!frames.empty()) {
			recycle(std::move(frames.front()));
			frames.pop_front();
		}
	}

	void clearPackets()
	{
		std::lock_guard<std::mutex> lock(packetLock);
		for (PacketItem &it : packets) {
			av_packet_free(&it.pkt);
		}
		packets.clear();
		packetBytes = 0;
	}

	// ---- the clock ----
	bool ready() const
	{
		return playbackReady(gate.load(), presentationReady.load());
	}

	double position()
	{
		std::lock_guard<std::mutex> lock(audioLock);
		if (hasAudio) {
			return baseValid ? basePts + static_cast<double>(consumed) / (static_cast<double>(kRate) * ringMul.load()) - (bsKind.load() != 0 ? bsDelayMs / 1000.0 : 0.0) : seekTarget;
		}
		const auto now = Clock::now();
		if (ready() && !pausedFlag && phase == 2) {
			wall += std::chrono::duration<double>(now - wallTick).count();
		}
		wallTick = now;
		return seekTarget + wall;
	}

	// Adds the sound to out, `ch` channels to a frame; nothing while the ring holds the other layout
	void mixChannels(int ch, std::int16_t *out, int frames)
	{
		std::lock_guard<std::mutex> lock(audioLock);
		if (bsKind.load() != 0 || !ready() || !baseValid || pausedFlag || ringCh != ch) {
			return;
		}
		const size_t cap = kRingFrames * static_cast<size_t>(ch);
		const size_t available = ringCount / static_cast<size_t>(ch);
		const int n = static_cast<int>(std::min<size_t>(available, static_cast<size_t>(frames)));
		const float v = muted ? 0.0f : volume.load();
		for (int i = 0; i < n * ch; i++) {
			int s = out[i] + static_cast<int>(static_cast<float>(ring[(ringRead + static_cast<size_t>(i)) % cap]) * v);
			out[i] = static_cast<std::int16_t>(std::min(32767, std::max(-32768, s)));
		}
		ringRead = (ringRead + static_cast<size_t>(n) * static_cast<size_t>(ch)) % cap;
		ringCount -= static_cast<size_t>(n) * static_cast<size_t>(ch);
		consumed += n;
	}

	// Waits for room, then writes. False when the session ended or a seek made this sound stale.
	bool pushAudio(const std::int16_t *data, int frames, int ch, double pts, unsigned gen)
	{
		const size_t need = static_cast<size_t>(frames) * static_cast<size_t>(ch);
		for (;;) {
			{
				std::lock_guard<std::mutex> lock(audioLock);
				if (stopFlag || gen != generation.load()) {
					return false;
				}
				if (ringCh != ch) {
					// a sound track with another layout: what is in the ring is of no use any more
					ringCh = ch;
					outChannels = ch;
					ringRead = ringCount = 0;
					baseValid = false;
					consumed = 0;
					say("PLAYER: sound output is %s", ch == 8 ? "7.1" : "stereo");
				}
				const size_t cap = ringCap(ch);
				if (cap - ringCount >= need) {
					if (!baseValid) {
						baseValid = true;
						basePts = pts;
						consumed = 0;
					}
					size_t at = (ringRead + ringCount) % cap;
					for (size_t i = 0; i < need; i++) {
						ring[at] = data[i];
						at = (at + 1) % cap;
					}
					ringCount += need;
					return true;
				}
			}
			sleepMs(4);
		}
	}

	// ---- Stage 17: compressed sound (AC-3, E-AC-3, DTS core) sent to the soundbar as it is ----
	// The ring then holds IEC 61937 bursts as S16 stereo, and a thread of its own feeds the console's compressed port.
	// Seeks, pauses and the clock work as before: only who empties the ring, and the frame rate of the clock, change.
	std::atomic<int> bsKind{ 0 };  // 0 off, else a jfiec::Kind
	std::atomic<int> ringMul{ 1 }; // port frames per 48 kHz frame of sound (4 for E-AC-3, whose port runs at 192 kHz)
	std::atomic<bool> bsRun{ false };
	std::atomic<int> bsState{ 0 }; // 0 starting, 1 running, 2 the port would not open
	std::thread bsThread;
	std::atomic<long long> bsBursts{ 0 }, bsBad{ 0 };
	int bsBadRun{ 0 };
	bool bsFirstLogged{ false };
	std::vector<std::uint8_t> bsBuf;
	int bsDelayMs{ 0 };
	bool sinkKnown{ false }, sinkAc3{ false }, sinkEac3{ false }, sinkDts{ false };
	// Stage 39 (experiment): Dolby TrueHD in MAT frames, sent through the port mode named in /data/jellyzoom-truehd-test
	static constexpr int kMat = 4;
	jfmat::Packer matPacker;
	double matFirstPts{ 0.0 };
	int matMode{ 4 };
	int matBytes{ 16384 }; // bytes the port is given a call
	int matCfg{ 1 };       // the port's configure argument (1 for the Dolby and DTS ports)
	int matFmt{ 0 };       // 0: 16-bit samples as they are, 1: each 16-bit word in the top half of a 32-bit sample
	// room in the ring: MAT is 3 MB/s, so it gets the whole ring as stereo frames (a second of it) instead of a quarter
	size_t ringCap(int ch) const
	{
		return bsKind.load() == kMat && ch == 2 ? kRingFrames * 8 : kRingFrames * static_cast<size_t>(ch);
	}
	static const char *nameOf(int kind)
	{
		return kind == kMat ? "Dolby TrueHD" : jfiec::kindName(kind);
	}

	// /data/jellyzoom-audio-mode: "pcm" decodes everything here, "bitstream" sends whatever can be sent, anything else (or no file): auto
	static int audioMode()
	{
		char b[32] = { 0 };
		FILE *f = std::fopen("/data/jellyzoom-audio-mode", "r");
		if (!f) {
			return 0;
		}
		const size_t got = std::fread(b, 1, sizeof(b) - 1, f);
		(void)got;
		std::fclose(f);
		if (std::strncmp(b, "pcm", 3) == 0) {
			return 2;
		}
		if (std::strncmp(b, "bit", 3) == 0) {
			return 1;
		}
		return 0;
	}

	void readSink()
	{
		alignas(16) std::uint8_t info[0x180];
		std::memset(info, 0, sizeof(info));
		const int rc = jf_sce_sys_monitor_info(1, info, sizeof(info));
		sinkKnown = true;
		if (rc == 0) {
			jfiec::sinkFormats(info, sizeof(info), &sinkAc3, &sinkEac3, &sinkDts);
		}
		say("AUDIO: the HDMI sink lists Dolby Digital=%d Dolby Digital Plus=%d DTS=%d (monitor info 0x%08x)",
			sinkAc3 ? 1 : 0, sinkEac3 ? 1 : 0, sinkDts ? 1 : 0, static_cast<unsigned>(rc));
		// Stage 38: every record the sink reports (format 1 PCM, 2 Dolby Digital, 7 DTS, 10 Dolby Digital Plus, 11 DTS-HD, 12 TrueHD)
		for (unsigned at = 0x98; rc == 0 && at + 8 <= sizeof(info) && at < 0xF0; at += 8) {
			if (info[at] == 0) {
				break;
			}
			say("AUDIO: sink record at 0x%02x: format %u, bytes %02x %02x %02x %02x %02x %02x %02x %02x", at, static_cast<unsigned>(info[at]),
				info[at], info[at + 1], info[at + 2], info[at + 3], info[at + 4], info[at + 5], info[at + 6], info[at + 7]);
		}
	}

	// One grain from the ring for the compressed port: 1 real sound, 0 nothing due (paused, or not started), -1 the ring ran short
	int takeGrain(std::int16_t *out, int frames)
	{
		std::lock_guard<std::mutex> lock(audioLock);
		if (!ready() || !baseValid || pausedFlag || ringCh != 2) {
			return 0;
		}
		const size_t n = static_cast<size_t>(frames) * 2;
		if (ringCount < n) {
			return -1;
		}
		const size_t cap = ringCap(2);
		for (size_t i = 0; i < n; i++) {
			out[i] = ring[(ringRead + i) % cap];
		}
		ringRead = (ringRead + n) % cap;
		ringCount -= n;
		consumed += frames;
		return 1;
	}

	void bsMain(int kind)
	{
		const int mode = kind == kMat ? matMode : jfiec::portMode(kind);
		const int grain = kind == kMat ? (matFmt == 1 ? matBytes / 8 : matBytes / 4) : jfiec::grainFrames(kind); // MAT: stereo frames of the stream a call carries
		const int mul = kind == kMat ? 16 : jfiec::portMul(kind);          // 768 kHz of stereo frames
		const int handle = jf_sce_ex_open(0xFF, mode);
		if (handle < 0) {
			say("AUDIO: ExOpen(0xFF,%d) = 0x%08x: no bitstream port, the sound is decoded here instead", mode, static_cast<unsigned>(handle));
			bsState = 2;
			return;
		}
		const int rc = jf_sce_ex_configure(0, 0, mode, kind == kMat ? matCfg : 1, 0);
		if (rc != 0) {
			say("AUDIO: ExConfigureOutput(mode %d) = 0x%08x: the sound is decoded here instead", mode, static_cast<unsigned>(rc));
			jf_sce_ex_close(handle);
			jf_sce_ex_configure(0, 0, 0xFF, 0xFF, 0);
			bsState = 2;
			return;
		}
		say("AUDIO: %s port open (mode %d, handle 0x%08x, grain %d frames at %d Hz)", nameOf(kind), mode,
			static_cast<unsigned>(handle), grain, 48000 * mul);
		bsState = 1;
		alignas(64) std::int16_t g[4096 * 2];
		alignas(64) std::int32_t g32[4096 * 2];
		const bool wide = kind == kMat && matFmt == 1;
		const auto period = std::chrono::nanoseconds(static_cast<long long>(1.0e9 * grain / (48000.0 * mul)));
		auto next = Clock::now();
		auto logAt = Clock::now() + std::chrono::seconds(5);
		long long real = 0, idle = 0, shortRing = 0, errors = 0, calls = 0;
		double inCall = 0.0;
		while (bsRun) {
			const int got = takeGrain(g, grain);
			if (got <= 0) {
				std::memset(g, 0, static_cast<size_t>(grain) * 4);
				(got < 0 ? shortRing : idle)++;
			} else {
				real++;
			}
			const auto t0 = Clock::now();
			if (wide) {
				for (int i = 0; i < grain * 2; i++) {
					g32[i] = static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::uint16_t>(g[i])) << 16);
				}
			}
			const int r = jf_sce_out_output(handle, wide ? static_cast<const void *>(g32) : static_cast<const void *>(g));
			const auto t1 = Clock::now();
			inCall += std::chrono::duration<double, std::milli>(t1 - t0).count();
			calls++;
			if (calls == 1) {
				say("AUDIO: first Output returned 0x%08x", static_cast<unsigned>(r));
			}
			if (r < 0 && errors++ < 3) {
				say("AUDIO: Output = 0x%08x", static_cast<unsigned>(r));
			}
			// The port may or may not block for a grain: pace by the clock too, so a fast return cannot run the ring dry
			next += period;
			const auto now = Clock::now();
			if (now < next) {
				std::this_thread::sleep_for(next - now);
			} else if (now - next > std::chrono::milliseconds(100)) {
				next = now;
			}
			if (now > logAt) {
				logAt = now + std::chrono::seconds(5);
				say("AUDIO: bitstream %s: bursts packed %lld, grains sent %lld, silent %lld, ring short %lld, Output avg %.2f ms (grain %.2f ms)",
					nameOf(kind), bsBursts.load(), real, idle, shortRing, inCall / static_cast<double>(calls ? calls : 1),
					1000.0 * grain / (48000.0 * mul));
			}
		}
		jf_sce_out_output(handle, nullptr);
		jf_sce_ex_close(handle);
		jf_sce_ex_configure(0, 0, 0xFF, 0xFF, 0);
		say("AUDIO: bitstream port closed, output back on PCM (%lld bursts, %lld grains, %lld ring short)", bsBursts.load(), real, shortRing);
	}

	bool bsStart(int kind)
	{
		bsBursts = bsBad = 0;
		bsBadRun = 0;
		bsFirstLogged = false;
		bsState = 0;
		bsRun = true;
		bsThread = std::thread([this, kind] { bsMain(kind); });
		for (int i = 0; i < 600 && bsState.load() == 0; i++) {
			sleepMs(5);
		}
		if (bsState.load() != 1) {
			bsRun = false;
			if (bsThread.joinable()) {
				bsThread.join();
			}
			return false;
		}
		{
			std::lock_guard<std::mutex> lock(audioLock);
			ringRead = ringCount = 0;
			baseValid = false;
			consumed = 0;
			ringCh = 2;
			outChannels = 2;
		}
		matPacker.reset();
		ringMul = kind == kMat ? 16 : jfiec::portMul(kind);
		bsKind = kind;
		return true;
	}

	void bsStop()
	{
		if (bsThread.joinable()) {
			bsRun = false;
			bsThread.join();
		}
		if (bsKind.load() != 0) {
			bsKind = 0;
			ringMul = 1;
			{
				std::lock_guard<std::mutex> lock(textLock);
				infoData.bitstream.clear();
			}
			std::lock_guard<std::mutex> lock(audioLock);
			ringRead = ringCount = 0;
			baseValid = false;
			consumed = 0;
		}
	}

	// Stage 38: which compressed-port modes does the console accept? Opened and closed straight away, nothing is sent.
	void probePorts()
	{
		static bool done = false;
		if (done) {
			return;
		}
		done = true;
		for (int mode = 0; mode <= 9; mode++) {
			const int h = jf_sce_ex_open(0xFF, mode);
			int rc = 0;
			if (h >= 0) {
				rc = jf_sce_ex_configure(0, 0, mode, 1, 0);
				jf_sce_ex_close(h);
				jf_sce_ex_configure(0, 0, 0xFF, 0xFF, 0);
			}
			say("PROBE: port mode %d: open 0x%08x, configure %s0x%08x", mode, static_cast<unsigned>(h), h >= 0 ? "" : "(not tried) ",
				static_cast<unsigned>(rc));
		}
	}

	// Decides, for the sound track about to play, between the soundbar decoding it and this app decoding it
	void chooseSound(const AVStream *as)
	{
		bsStop();
		if (!as) {
			return;
		}
		const AVCodecParameters *cp = as->codecpar;
		const int mode = audioMode();
		int kind = jfiec::None;
		const char *why = "decoded here";
		if (mode == 2) {
			why = "PCM chosen in /data/jellyzoom-audio-mode";
		} else if (cp->sample_rate != 48000) {
			why = "not 48 kHz";
		} else if (cp->codec_id == AV_CODEC_ID_AC3 || cp->codec_id == AV_CODEC_ID_EAC3 || cp->codec_id == AV_CODEC_ID_DTS) {
			if (!sinkKnown) {
				readSink();
			}
			if (cp->codec_id == AV_CODEC_ID_AC3) {
				kind = (mode == 1 || sinkAc3) ? jfiec::Ac3 : jfiec::None;
			} else if (cp->codec_id == AV_CODEC_ID_EAC3) {
				kind = (mode == 1 || sinkEac3) ? jfiec::Eac3 : jfiec::None;
			} else if (mode == 1 || (sinkDts && cp->profile < 50)) { // auto leaves DTS-HD to the decoder here: the core alone is the lesser sound
				kind = jfiec::Dts;
			}
			why = kind ? "sent as it is" : "the sink does not list it, or it is DTS-HD";
		}
		say("AUDIO: track %s (%d ch, profile %d, %d Hz): %s (%s)", avcodec_get_name(cp->codec_id), cp->ch_layout.nb_channels,
			cp->profile, cp->sample_rate, nameOf(kind), why);
		if (cp->codec_id == AV_CODEC_ID_TRUEHD) {
			if (!sinkKnown) {
				readSink();
			}
			if (FILE *probe = std::fopen("/data/jellyzoom-probe-truehd", "r")) {
				std::fclose(probe);
				probePorts();
			}
			// Stage 39: the experiment is on when /data/jellyzoom-truehd-test exists; its number is the port mode to try (4, or 1)
			if (mode != 2 && cp->sample_rate == 48000) {
				if (FILE *test = std::fopen("/data/jellyzoom-truehd-test", "r")) {
					int m = 4, b = 16384, f = 0, c = 1;
					const int got = std::fscanf(test, "%d %d %d %d", &m, &b, &f, &c);
					matCfg = got >= 4 ? std::max(0, std::min(16, c)) : 1;
					std::fclose(test);
					if (got < 1) {
						m = 4;
					}
					matMode = std::max(0, std::min(9, m));
					matFmt = got >= 3 && f == 1 ? 1 : 0;
					// the bytes a call carries: whole 4096s; the stream part of them is all (16-bit) or half (32-bit)
					b = got >= 2 ? b : 16384;
					b = std::max(matFmt == 1 ? 8192 : 4096, std::min(matFmt == 1 ? 32768 : 16384, (b / 4096) * 4096));
					matBytes = b;
					kind = kMat;
					say("AUDIO: TrueHD experiment: MAT frames through port mode %d, %d bytes a call, %s samples, configure argument %d", matMode,
						matBytes, matFmt == 1 ? "32-bit" : "16-bit", matCfg);
				}
			}
		}
		if (kind != jfiec::None && !bsStart(kind)) {
			kind = jfiec::None;
		}
		if (kind != jfiec::None) {
			bsDelayMs = 0;
			if (FILE *f = std::fopen("/data/jellyzoom-audio-delay", "r")) { // milliseconds the soundbar adds, to hold the picture back by
				int ms = 0;
				if (std::fscanf(f, "%d", &ms) == 1) {
					bsDelayMs = std::max(0, std::min(500, ms));
				}
				std::fclose(f);
			}
			say("AUDIO: delay %d ms (/data/jellyzoom-audio-delay)", bsDelayMs);
			noteSet(std::string("Sound: ") + nameOf(kind) + ", sent to your soundbar");
			std::lock_guard<std::mutex> lock(textLock);
			infoData.bitstream = nameOf(kind);
		}
	}

	// ---- the video thread ----
	struct Converter
	{
		SwsContext *sws{ nullptr };
		int sw{ 0 }, sh{ 0 }, fmt{ -1 }, cs{ -1 }, range{ -1 }, outFmt{ -1 }, outW{ 0 }, outH{ 0 };
		std::vector<std::uint16_t> wide; // 16-bit R'G'B' of an HDR picture, before the tone mapping
		jftm::Mapper mapper;
		int peakNits{ 0 };
		int transfer{ 0 }; // 0 SDR, 1 PQ, 2 HLG: of the last picture
		bool passThrough{ false }; // the last picture was left as PQ (RGBA16)

		~Converter()
		{
			sws_freeContext(sws);
		}

		// How bright the brightest part of the picture can be: the file's own figures, else a guess
		static int peakOf(const AVFrame *fr)
		{
			double nits = 0.0;
			if (const AVFrameSideData *sd = av_frame_get_side_data(fr, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL)) {
				nits = reinterpret_cast<const AVContentLightMetadata *>(sd->data)->MaxCLL;
			}
			if (nits < 100.0) {
				if (const AVFrameSideData *sd = av_frame_get_side_data(fr, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA)) {
					const AVMasteringDisplayMetadata *m = reinterpret_cast<const AVMasteringDisplayMetadata *>(sd->data);
					if (m->has_luminance) {
						nits = av_q2d(m->max_luminance);
					}
				}
			}
			if (nits < 100.0) {
				nits = 1000.0;
			}
			return static_cast<int>(std::min(4000.0, std::max(400.0, nits)));
		}

		bool convert(const AVFrame *fr, VFrame &out, bool hdrOutput)
		{
			const int w = fr->width, h = fr->height;
			const double scale = std::min(static_cast<double>(kMaxW) / w, static_cast<double>(kMaxH) / h);
			int dw = static_cast<int>(w * scale + 0.5) & ~1;
			int dh = static_cast<int>(h * scale + 0.5) & ~1;
			dw = std::max(2, std::min(dw, kMaxW));
			dh = std::max(2, std::min(dh, kMaxH));
			transfer = fr->color_trc == AVCOL_TRC_SMPTE2084 ? 1 : fr->color_trc == AVCOL_TRC_ARIB_STD_B67 ? 2 : 0;
			passThrough = hdrOutput && transfer == 1;
			const AVPixelFormat target = passThrough ? AV_PIX_FMT_RGBA64LE : transfer ? AV_PIX_FMT_RGB48LE : AV_PIX_FMT_RGBA;
			SwsContext *before = sws;
			// a picture that shrinks is averaged, one that does not is smoothed
			sws = sws_getCachedContext(sws, w, h, static_cast<AVPixelFormat>(fr->format), dw, dh, target,
				dw < w ? SWS_AREA : SWS_BILINEAR, nullptr, nullptr, nullptr);
			if (!sws) {
				return false;
			}
			if (sws != before) {
				av_opt_set_int(sws, "threads", 3, 0); // slice threads, where this FFmpeg has them
			}
			const int range1 = fr->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
			if (w != sw || h != sh || fr->format != fmt || fr->colorspace != cs || range1 != range || outFmt != target ||
				outW != dw || outH != dh) {
				int table = SWS_CS_DEFAULT;
				switch (fr->colorspace) {
				case AVCOL_SPC_BT709: table = SWS_CS_ITU709; break;
				case AVCOL_SPC_BT470BG:
				case AVCOL_SPC_SMPTE170M: table = SWS_CS_ITU601; break;
				case AVCOL_SPC_BT2020_NCL:
				case AVCOL_SPC_BT2020_CL: table = SWS_CS_BT2020; break;
				default: table = transfer ? SWS_CS_BT2020 : (h >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601); break;
				}
				sws_setColorspaceDetails(sws, sws_getCoefficients(table), range1, sws_getCoefficients(SWS_CS_DEFAULT), 1,
					0, 1 << 16, 1 << 16);
				sw = w;
				sh = h;
				fmt = fr->format;
				cs = fr->colorspace;
				range = range1;
				outFmt = target;
				outW = dw;
				outH = dh;
			}
			out.rgba.resize(static_cast<size_t>(dw) * dh * (passThrough ? 8 : 4));
			out.hdr = passThrough;
			if (passThrough) {
				// HDR10 for an HDR10 display: the code values stay as they are (PQ, BT.2020), 16 bits a channel
				std::uint8_t *dst[4] = { out.rgba.data(), nullptr, nullptr, nullptr };
				int stride[4] = { dw * 8, 0, 0, 0 };
				sws_scale(sws, fr->data, fr->linesize, 0, h, dst, stride);
				peakNits = peakOf(fr);
			} else if (!transfer) {
				std::uint8_t *dst[4] = { out.rgba.data(), nullptr, nullptr, nullptr };
				int stride[4] = { dw * 4, 0, 0, 0 };
				sws_scale(sws, fr->data, fr->linesize, 0, h, dst, stride);
			} else {
				wide.resize(static_cast<size_t>(dw) * dh * 3);
				std::uint8_t *dst[4] = { reinterpret_cast<std::uint8_t *>(wide.data()), nullptr, nullptr, nullptr };
				int stride[4] = { dw * 6, 0, 0, 0 };
				sws_scale(sws, fr->data, fr->linesize, 0, h, dst, stride);
				peakNits = transfer == 2 ? 1000 : peakOf(fr);
				mapper.prepare(transfer, static_cast<float>(peakNits));
				// bands of rows on a few threads
				const int bands = dh >= 480 ? 3 : 1;
				std::thread helpers[2];
				const std::uint16_t *src = wide.data();
				std::uint8_t *dest = out.rgba.data();
				const size_t rowElems = static_cast<size_t>(dw) * 3;
				for (int b = 1; b < bands; b++) {
					const int from = dh * b / bands, to = dh * (b + 1) / bands;
					helpers[b - 1] = std::thread([this, src, rowElems, dest, dw, from, to] {
						mapper.run(src, rowElems, dest, dw, from, to);
					});
				}
				mapper.run(src, rowElems, dest, dw, 0, dh / bands);
				for (std::thread &t : helpers) {
					if (t.joinable()) {
						t.join();
					}
				}
			}
			out.w = dw;
			out.h = dh;
			return true;
		}
	};

	void runVideo(AVCodecContext *vctx, AVRational tb, double startOffset, double fps)
	{
		AVFrame *fr = av_frame_alloc();
		Converter conv;
		jfhw::Decoder hw; // the console's hardware decoder, when it takes this film
		jfav1::Decoder av1; // AV1 goes to dav1d (Stage 18)
		bool useAv1 = false;
		if (vctx->codec_id == AV_CODEC_ID_AV1) {
			useAv1 = av1.open(6, [](const char *line) { say("%s", line); });
			say("AV1: dav1d %s %s", jfav1::Decoder::version(), useAv1 ? "started with 6 threads" : "would not start");
			if (useAv1) {
				std::lock_guard<std::mutex> lock(textLock);
				infoData.videoCodec += " (dav1d)";
			}
		}
		auto av1Log = Clock::now() + std::chrono::seconds(5);
		long long av1Last = 0;
		double av1LastMs = 0.0;
		bool useHw = !useAv1 && hw.open(vctx);
		if (useHw) {
			std::lock_guard<std::mutex> lock(textLock);
			infoData.videoCodec += " (hardware)";
		}
		double lastPts = 0.0;
		int lateStreak = 0;
		int reportedPeak = 0;
		bool reportedPass = false;
		auto perfStart = Clock::now();
		double convertTotalMs = 0.0, convertMaxMs = 0.0;
		long long convertCount = 0, lastDropped = 0;
		while (!stopFlag) {
			PacketItem it;
			{
				std::unique_lock<std::mutex> lock(packetLock);
				packetCv.wait_for(lock, std::chrono::milliseconds(20), [&] { return !packets.empty() || stopFlag; });
				if (packets.empty()) {
					continue;
				}
				it = packets.front();
				packets.pop_front();
				if (it.pkt) {
					packetBytes -= static_cast<size_t>(it.pkt->size);
				}
			}
			const auto perfNow = Clock::now();
			if (std::chrono::duration<double>(perfNow - perfStart).count() >= 10.0) {
				if (phase.load() == 2 && !pausedFlag) {
					size_t queuedBytes = 0, queuedFrames = 0;
					{
						std::lock_guard<std::mutex> lock(packetLock);
						queuedBytes = packetBytes;
					}
					{
						std::lock_guard<std::mutex> lock(videoLock);
						queuedFrames = frames.size();
					}
					say("PLAYER PERF: %s, %lld conversions avg %.1f ms max %.1f ms, %lld late drops, video queue %zu frames %zu KiB",
						useHw ? "hardware" : useAv1 ? "dav1d" : "software", convertCount,
						convertCount ? convertTotalMs / static_cast<double>(convertCount) : 0.0, convertMaxMs,
						lateTotal.load() - lastDropped, queuedFrames, queuedBytes / 1024);
				}
				perfStart = perfNow;
				convertTotalMs = convertMaxMs = 0.0;
				convertCount = 0;
				lastDropped = lateTotal.load();
			}
			if (it.flush) {
				avcodec_flush_buffers(vctx);
				if (useAv1) {
					av1.flush();
				}
				if (useHw) {
					hw.flush();
				}
				clearFrames();
				av_packet_free(&it.pkt);
				continue;
			}
			if (it.gen != generation.load()) {
				av_packet_free(&it.pkt);
				continue;
			}
			if (useAv1) {
				av1.send(it.pkt);
			} else if (useHw) {
				if (!hw.send(it.pkt)) {
					// the hardware decoder gave up: the software decoder starts again from the key frame before here
					useHw = false;
					hw.close();
					say("PLAYER: hardware decoding failed, continuing in software");
					{
						std::lock_guard<std::mutex> lock(textLock);
						infoData.videoCodec = std::string(avcodec_get_name(vctx->codec_id)) + " (software)";
					}
					av_packet_free(&it.pkt);
					requestSeek(position());
					continue;
				}
			} else {
				avcodec_send_packet(vctx, it.pkt); // a null packet drains the decoder at the end of the file
			}
			av_packet_free(&it.pkt);
			if (useAv1 && Clock::now() > av1Log) {
				av1Log = Clock::now() + std::chrono::seconds(5);
				const long long n = av1.pictures() - av1Last;
				const double ms = av1.decodeMs() - av1LastMs;
				av1Last = av1.pictures();
				av1LastMs = av1.decodeMs();
				say("AV1: %lld pictures in 5 s (%.1f a second), %.1f ms of decoding a picture, %lld dropped late so far", n,
					static_cast<double>(n) / 5.0, n > 0 ? ms / static_cast<double>(n) : 0.0, lateTotal.load());
			}
			while (!stopFlag && (useAv1 ? av1.receive(fr) : useHw ? hw.receive(fr) : avcodec_receive_frame(vctx, fr) == 0)) {
				if (it.gen != generation.load()) {
					av_frame_unref(fr);
					break;
				}
				double pts;
				if (fr->best_effort_timestamp != AV_NOPTS_VALUE) {
					pts = static_cast<double>(fr->best_effort_timestamp) * av_q2d(tb) - startOffset;
				} else {
					pts = lastPts + (fps > 0.0 ? 1.0 / fps : 0.04);
				}
				lastPts = pts;
				if (pts < skipUntil.load() - 0.001) {
					av_frame_unref(fr);
					continue;
				}
				// a picture the sound has already passed is not worth converting: keep one in four so the screen still moves
				if (phase.load() == 2 && ready() && pts < position() - 0.3 && lateStreak < 3) {
					lateStreak++;
					lateTotal++;
					av_frame_unref(fr);
					continue;
				}
				lateStreak = 0;
				VFrame f;
				{
					std::lock_guard<std::mutex> lock(videoLock);
					if (!pool.empty()) {
						f = std::move(pool.back());
						pool.pop_back();
					}
				}
				const auto convertStart = Clock::now();
				const bool ok = conv.convert(fr, f, hdrOut.load());
				const double convertMs = std::chrono::duration<double, std::milli>(Clock::now() - convertStart).count();
				convertTotalMs += convertMs;
				convertMaxMs = std::max(convertMaxMs, convertMs);
				++convertCount;
				av_frame_unref(fr);
				if (ok && (conv.peakNits != reportedPeak || conv.passThrough != reportedPass)) {
					reportedPeak = conv.peakNits;
					reportedPass = conv.passThrough;
					std::lock_guard<std::mutex> lock(textLock);
					infoData.peakNits = reportedPeak;
					infoData.toneMapped = conv.transfer != 0 && !conv.passThrough;
					infoData.hdrOutput = conv.passThrough;
				}
				if (!ok) {
					continue;
				}
				f.pts = pts;
				// wait for room, unless a seek made this picture stale
				for (;;) {
					{
						std::lock_guard<std::mutex> lock(videoLock);
						if (it.gen != generation.load()) {
							break;
						}
						if (frames.size() < kQueueFrames) {
							frames.push_back(std::move(f));
							gate = true;
							break;
						}
					}
					if (stopFlag) {
						break;
					}
					sleepMs(3);
				}
			}
		}
		av_frame_free(&fr);
		hw.close();
		av1.close();
	}

	void queuePacket(AVPacket *pkt, unsigned gen, bool flush)
	{
		std::lock_guard<std::mutex> lock(packetLock);
		PacketItem it;
		it.pkt = pkt;
		it.gen = gen;
		it.flush = flush;
		packets.push_back(it);
		if (pkt) {
			packetBytes += static_cast<size_t>(pkt->size);
		}
		packetCv.notify_one();
	}

	// A seek from any thread: the sound and pictures of the old place are dropped, the demux thread jumps
	void requestSeek(double seconds)
	{
		if (phase.load() != 2 && phase.load() != 3) {
			return;
		}
		seconds = std::max(0.0, seconds);
		const double dur = durationSec.load();
		if (dur > 0.0) {
			seconds = std::min(seconds, std::max(0.0, dur - 2.0));
		}
		{
			std::lock_guard<std::mutex> lock(audioLock);
			baseValid = false;
			consumed = 0;
			ringRead = ringCount = 0;
			seekTarget = seconds;
			wall = 0.0;
			wallTick = Clock::now();
			gate = false;
			skipUntil = seconds;
			generation++;
		}
		clearFrames();
		if (phase.load() == 3) {
			phase = 2;
		}
		seekMs = static_cast<long long>(seconds * 1000.0);
	}

	// ---- subtitles ----
	static bool isBitmapSubtitle(AVCodecID id)
	{
		return id == AV_CODEC_ID_HDMV_PGS_SUBTITLE || id == AV_CODEC_ID_DVD_SUBTITLE || id == AV_CODEC_ID_DVB_SUBTITLE ||
			id == AV_CODEC_ID_XSUB;
	}

	void setSubStatus(const std::string &status)
	{
		std::shared_ptr<SubState> state;
		{
			std::lock_guard<std::mutex> lock(subLock);
			state = sub;
		}
		if (state) {
			std::lock_guard<std::mutex> lock(state->m);
			state->status = status;
		}
	}

	// (Stage 37: picture subtitles are no longer burnt into the frame; the app draws them, see subtitlePictures)

	// One decoded subtitle event from the file: show it from its start until its end, or until the next event
	void takeBitmapEvent(AVSubtitle &s, const AVPacket *pkt, double startOffset, int canvasW, int canvasH)
	{
		double base = 0.0;
		if (s.pts != AV_NOPTS_VALUE) {
			base = static_cast<double>(s.pts) / AV_TIME_BASE - startOffset;
		} else if (pkt->pts != AV_NOPTS_VALUE) {
			base = static_cast<double>(pkt->pts) * 0.001 - startOffset; // unlikely: Matroska is in milliseconds
		}
		const double t0 = base + static_cast<double>(s.start_display_time) / 1000.0;
		// open ended: until the next event clears it, but never for longer than half a minute
		double t1 = t0 + 30.0;
		if (s.end_display_time != 0 && s.end_display_time != 0xFFFFFFFFu && s.end_display_time < 600000u) {
			t1 = base + static_cast<double>(s.end_display_time) / 1000.0;
		} else if (pkt->duration > 0) {
			t1 = t0 + static_cast<double>(pkt->duration) * 0.001;
		}
		std::lock_guard<std::mutex> lock(bitmapLock);
		// a new event replaces what is still showing; an empty one is the "clear" that ends it
		for (PicEvent &old : bitmaps) {
			if (old.end > t0) {
				old.end = std::max(old.start, t0);
			}
		}
		while (!bitmaps.empty() && (bitmaps.front().end < t0 - 6.0 || bitmaps.size() > 48)) {
			bitmaps.pop_front();
		}
		for (unsigned i = 0; i < s.num_rects; i++) {
			const AVSubtitleRect *r = s.rects[i];
			if (r->type != SUBTITLE_BITMAP || r->w <= 0 || r->h <= 0 || !r->data[0] || !r->data[1]) {
				continue;
			}
			jfsub::Bitmap b;
			b.start = t0;
			b.end = t1;
			b.x = r->x;
			b.y = r->y;
			b.w = r->w;
			b.h = r->h;
			b.cw = canvasW;
			b.ch = canvasH;
			b.rgba.resize(static_cast<size_t>(r->w) * r->h * 4);
			const std::uint32_t *palette = reinterpret_cast<const std::uint32_t *>(r->data[1]);
			const int colours = std::min(r->nb_colors > 0 ? r->nb_colors : 256, 256);
			for (int y = 0; y < r->h; y++) {
				const std::uint8_t *row = r->data[0] + static_cast<size_t>(y) * r->linesize[0];
				std::uint8_t *out = &b.rgba[static_cast<size_t>(y) * r->w * 4];
				for (int x = 0; x < r->w; x++) {
					const int idx = row[x];
					if (idx < colours) {
						jfsub::paletteEntry(palette[idx], out + x * 4);
					} else {
						out[x * 4] = out[x * 4 + 1] = out[x * 4 + 2] = out[x * 4 + 3] = 0;
					}
				}
			}
			PicEvent e;
			e.start = b.start;
			e.end = b.end;
			e.x = b.x;
			e.y = b.y;
			e.w = b.w;
			e.h = b.h;
			e.cw = b.cw;
			e.ch = b.ch;
			e.id = ++bitmapSerial;
			e.rgba = std::make_shared<const std::vector<std::uint8_t>>(std::move(b.rgba));
			bitmaps.push_back(std::move(e));
		}
	}

	void startSubtitle(int streamIndex)
	{
		std::shared_ptr<SubState> state(new SubState());
		{
			std::lock_guard<std::mutex> lock(subLock);
			if (sub) {
				sub->stop = true;
			}
			sub = state;
		}
		curSub = streamIndex;
		std::string id, source;
		{
			std::lock_guard<std::mutex> lock(textLock);
			id = itemId;
			source = mediaSource;
		}
		if (streamIndex < 0) {
			return;
		}
		JfSession session;
		std::memset(&session, 0, sizeof(session));
		if (id.empty() || source.empty() || !jf_gate_session(&session)) {
			std::lock_guard<std::mutex> lock(state->m);
			state->status = "Subtitles unavailable";
			return;
		}
		{
			std::lock_guard<std::mutex> lock(state->m);
			state->status = "Loading subtitles ...";
		}
		char path[512];
		std::snprintf(path, sizeof(path), "/Videos/%s/%s/Subtitles/%d/0/Stream.srt", id.c_str(), source.c_str(), streamIndex);
		char auth[4096];
		std::snprintf(auth, sizeof(auth),
			"Authorization: MediaBrowser Client=\"%s\", Device=\"%s\", DeviceId=\"%s\", Version=\"%s\", Token=\"%s\"\r\n",
			JF_CLIENT_NAME, JF_DEVICE_NAME, JF_DEVICE_ID, JF_CLIENT_VERSION, session.access_token);
		const std::string request = path;
		const std::string headers = auth;
		std::thread([state, request, headers] {
			HttpResponse response;
			std::memset(&response, 0, sizeof(response));
			std::vector<Cue> cues;
			bool ok = false;
			if (http_get(jfconf::host(), jfconf::port(), request.c_str(), headers.c_str(), &response) == 0) {
				if (response.status == 200 && response.body) {
					parseSrt(std::string(response.body), cues);
					ok = !cues.empty();
				}
				say("PLAYER: subtitles HTTP %d, %zu lines", response.status, cues.size());
				http_response_free(&response);
			} else {
				say("PLAYER: subtitle request failed");
			}
			if (state->stop) {
				return;
			}
			std::lock_guard<std::mutex> lock(state->m);
			state->cues.swap(cues);
			state->status = ok ? "" : "Subtitles unavailable";
		}).detach();
	}

	void noteSet(const std::string &text)
	{
		std::lock_guard<std::mutex> lock(textLock);
		noteText = text;
	}

	// What FFmpeg says about the sound track that plays
	void audioInfo(const AVStream *as)
	{
		std::lock_guard<std::mutex> lock(textLock);
		infoData.audioCodec = avcodec_get_name(as->codecpar->codec_id);
		infoData.audioChannels = as->codecpar->ch_layout.nb_channels;
		// the "Atmos" flag FFmpeg sets on E-AC-3 (JOC) and TrueHD streams is profile 30
		infoData.audioProfile.clear();
		if (as->codecpar->codec_id == AV_CODEC_ID_DTS) {
			switch (as->codecpar->profile) {
			case 60: infoData.audioProfile = "DTS-HD Master Audio"; break;
			case 61: infoData.audioProfile = "DTS:X"; break;
			case 50: infoData.audioProfile = "DTS-HD High Resolution"; break;
			case 30: infoData.audioProfile = "DTS-ES"; break;
			case 40: infoData.audioProfile = "DTS 96/24"; break;
			default: break;
			}
		}
		infoData.atmos = as->codecpar->profile == 30 &&
			(as->codecpar->codec_id == AV_CODEC_ID_TRUEHD || as->codecpar->codec_id == AV_CODEC_ID_EAC3);
	}

	// ---- the demux thread ----
	void runDemux(int gateIndex, double resume)
	{
		phase = 1;
		JfSession session;
		std::memset(&session, 0, sizeof(session));
		if (!jf_gate_session(&session)) {
			fail("Not signed in");
			return;
		}
		const char *idc = jf_gate_item_id(gateIndex);
		if (!idc || !*idc) {
			fail("Unknown film");
			return;
		}
		const std::string id = idc;
		JfGateMeta meta{};
		if (jf_gate_item_meta(gateIndex, &meta)) {
			std::lock_guard<std::mutex> lock(textLock);
			titleText = meta.name ? meta.name : "";
		}
		say("PLAYER: opening %s (%s)", id.c_str(), titleText.c_str());

		JfPlayback pb;
		std::memset(&pb, 0, sizeof(pb));
		if (jf_get_playback_info(&session, id.c_str(), &pb) != 0) {
			fail(std::string("The server would not start the film: ") + jf_last_error());
			return;
		}
		{
			std::lock_guard<std::mutex> lock(textLock);
			itemId = id;
			mediaSource = pb.media_source_id;
		}
		jf_gate_playback_extras_request(gateIndex, id.c_str(), pb.media_source_id);
		char path[1024];
		std::snprintf(path, sizeof(path), "/Videos/%s/stream?Static=true&MediaSourceId=%s&PlaySessionId=%s", id.c_str(),
			pb.media_source_id, pb.play_session_id);
		char headers[4096];
		std::snprintf(headers, sizeof(headers),
			"Authorization: MediaBrowser Client=\"%s\", Device=\"%s\", DeviceId=\"%s\", Version=\"%s\", Token=\"%s\"\r\n",
			JF_CLIENT_NAME, JF_DEVICE_NAME, JF_DEVICE_ID, JF_CLIENT_VERSION, session.access_token);

		int64_t fileSize = 0;
		AVIOContext *io = jf_avio_open(jfconf::host(), jfconf::port(), path, headers, &stopFlag, &fileSize);
		if (!io) {
			fail("Could not reach the server");
			return;
		}
		AVFormatContext *fc = avformat_alloc_context();
		fc->pb = io;
		fc->flags |= AVFMT_FLAG_CUSTOM_IO;
		AVCodecContext *vctx = nullptr;
		AVCodecContext *actx = nullptr;
		SwrContext *swr = nullptr;
		bool videoStarted = false;
		struct Cleanup
		{
			Impl &self;
			AVFormatContext *&fc;
			AVIOContext *&io;
			AVCodecContext *&vctx;
			AVCodecContext *&actx;
			SwrContext *&swr;
			bool &videoStarted;
			~Cleanup()
			{
				self.stopFlag = true; // whatever ended this, the video thread must go too
				self.packetCv.notify_all();
				if (videoStarted && self.video.joinable()) {
					self.video.join();
				}
				swr_free(&swr);
				avcodec_free_context(&self.subctx);
				avcodec_free_context(&vctx);
				avcodec_free_context(&actx);
				if (fc) {
					avformat_close_input(&fc);
				}
				jf_avio_close(&io);
			}
		} cleanup{ *this, fc, io, vctx, actx, swr, videoStarted };

		AVDictionary *opts = nullptr;
		av_dict_set(&opts, "probesize", "5000000", 0);
		av_dict_set(&opts, "analyzeduration", "5000000", 0);
		int rc = avformat_open_input(&fc, "jellyfin-stream", nullptr, &opts);
		av_dict_free(&opts);
		if (rc < 0) {
			fail("Could not read the film");
			return;
		}
		if (avformat_find_stream_info(fc, nullptr) < 0) {
			fail("Could not read the film's streams");
			return;
		}
		const int vi = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
		int ai = av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
		{
			const int wanted = wantAudio.load();
			if (wanted >= 0 && wanted < static_cast<int>(fc->nb_streams) &&
				fc->streams[wanted]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
				ai = wanted;
			} else if (wanted < 0) {
				// Stage 36: nothing was picked for this film: the language chosen in Settings, when the file has it
				const int byLanguage = streamByLanguage(fc, AVMEDIA_TYPE_AUDIO, prefText("/data/jellyzoom-lang-audio"));
				if (byLanguage >= 0) {
					ai = byLanguage;
					say("PLAYER: sound track %d chosen by the language setting", ai);
				}
			}
		}
		if (vi < 0) {
			fail("The film has no picture");
			return;
		}
		const double startOffset = fc->start_time != AV_NOPTS_VALUE ? fc->start_time / static_cast<double>(AV_TIME_BASE) : 0.0;
		const double duration = fc->duration > 0 ? fc->duration / static_cast<double>(AV_TIME_BASE) : 0.0;
		durationSec = duration;
		{
			std::lock_guard<std::mutex> lock(textLock);
			for (unsigned i = 0; i < fc->nb_chapters; i++) {
				const AVChapter *ch = fc->chapters[i];
				Chapter c;
				c.time = static_cast<float>(static_cast<double>(ch->start) * av_q2d(ch->time_base) - startOffset);
				AVDictionaryEntry *t = av_dict_get(ch->metadata, "title", nullptr, 0);
				c.title = t ? t->value : "";
				chapterList.push_back(c);
			}
		}

		// Stage 17: whatever happens below, the compressed port is closed and the output put back on PCM
		struct BsGuard
		{
			Impl *p;
			~BsGuard() { p->bsStop(); }
		} bsGuard{ this };
		// decoders
		AVStream *vs = fc->streams[vi];
		say("PLAYER: ffmpeg %s, built with: %s", av_version_info(), avcodec_configuration());
		const AVCodec *vcodec = nullptr;
		if (vs->codecpar->codec_id == AV_CODEC_ID_AV1) {
			// Stage 16: AV1 is software decoding (the console has no AV1 hardware decoder that we know of). FFmpeg's own
			// "av1" decoder only works with a hardware decoder behind it, so it is never started here.
			const AVCodec *dav1d = avcodec_find_decoder_by_name("libdav1d");
			const AVCodec *aom = avcodec_find_decoder_by_name("libaom-av1");
			const AVPixFmtDescriptor *adesc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(vs->codecpar->format));
			say("AV1: stream %dx%d, %d-bit; decoders in this build: libdav1d=%s libaom=%s (built-in av1 is not used)",
				vs->codecpar->width, vs->codecpar->height, adesc ? adesc->comp[0].depth : 0, dav1d ? "yes" : "no",
				aom ? "yes" : "no");
			vcodec = dav1d ? dav1d : aom;
#ifdef JF_HAVE_DAV1D
			if (!vcodec) {
				// Stage 18: dav1d is called directly (jf_av1.hpp); FFmpeg's built-in decoder only holds the stream's parameters
				vcodec = avcodec_find_decoder(AV_CODEC_ID_AV1);
				say("AV1: this build decodes AV1 with dav1d %s, called directly", jfav1::Decoder::version());
			}
#endif
			if (!vcodec) {
				say("AV1: no software AV1 decoder here, not opening the file");
				fail("AV1 needs the dav1d decoder, which this build does not have yet");
				return;
			}
		} else {
			vcodec = avcodec_find_decoder(vs->codecpar->codec_id);
		}
		if (!vcodec) {
			fail(std::string("No decoder for ") + avcodec_get_name(vs->codecpar->codec_id));
			return;
		}
		vctx = avcodec_alloc_context3(vcodec);
		avcodec_parameters_to_context(vctx, vs->codecpar);
		vctx->thread_count = 5;
		vctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
		if (avcodec_open2(vctx, vcodec, nullptr) < 0) {
			fail("Could not start the video decoder");
			return;
		}
		if (ai >= 0) {
			AVStream *as = fc->streams[ai];
			const AVCodec *acodec = avcodec_find_decoder(as->codecpar->codec_id);
			if (acodec) {
				actx = avcodec_alloc_context3(acodec);
				avcodec_parameters_to_context(actx, as->codecpar);
				if (avcodec_open2(actx, acodec, nullptr) < 0) {
					avcodec_free_context(&actx);
				}
			}
			if (!actx) {
				say("PLAYER: no usable sound track (%s), playing silent", avcodec_get_name(as->codecpar->codec_id));
				noteSet(std::string("No decoder for ") + avcodec_get_name(as->codecpar->codec_id) + ": playing without sound");
			}
		}
		hasAudio = actx != nullptr;
		curAudio = actx ? ai : -1;
		if (actx) {
			chooseSound(fc->streams[ai]);
		}
		{
			// what is really in the file
			std::lock_guard<std::mutex> lock(textLock);
			infoData.container = fc->iformat && fc->iformat->name ? fc->iformat->name : "";
			infoData.videoCodec = avcodec_get_name(vs->codecpar->codec_id);
			infoData.width = vs->codecpar->width;
			infoData.height = vs->codecpar->height;
			infoData.fps = vs->avg_frame_rate.den ? av_q2d(vs->avg_frame_rate) : 0.0;
			infoData.bitrate = fc->bit_rate;
			const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(vs->codecpar->format));
			infoData.bitDepth = desc ? desc->comp[0].depth : 8;
			infoData.range = vs->codecpar->color_trc == AVCOL_TRC_SMPTE2084 ? "HDR10"
				: vs->codecpar->color_trc == AVCOL_TRC_ARIB_STD_B67 ? "HLG" : "SDR";
			const AVPacketSideData *dv = av_packet_side_data_get(vs->codecpar->coded_side_data, vs->codecpar->nb_coded_side_data,
				AV_PKT_DATA_DOVI_CONF);
			if (dv && dv->size >= static_cast<size_t>(sizeof(AVDOVIDecoderConfigurationRecord))) {
				const AVDOVIDecoderConfigurationRecord *conf = reinterpret_cast<const AVDOVIDecoderConfigurationRecord *>(dv->data);
				infoData.dovi = true;
				infoData.doviProfile = conf->dv_profile;
				infoData.doviLevel = conf->dv_level;
				infoData.doviEnhancement = conf->el_present_flag != 0;
			}
		}
		if (actx) {
			audioInfo(fc->streams[ai]);
		}
		if (infoData.dovi && infoData.doviProfile == 5) {
			noteSet("Dolby Vision profile 5: colours will look wrong until it is supported");
		}
		{
			// which of the usual sound formats this FFmpeg can decode: one line for the log
			static const char *const names[] = { "aac", "ac3", "eac3", "truehd", "mlp", "dca", "flac", "opus", "vorbis", "mp3", "pcm_s16le", "pcm_s24le" };
			std::string have, lack;
			for (const char *n : names) {
				(avcodec_find_decoder_by_name(n) ? have : lack) += std::string(n) + " ";
			}
			say("PLAYER: sound decoders here: %s| missing: %s", have.c_str(), lack.empty() ? "none" : lack.c_str());
		}
		const double fps = vs->avg_frame_rate.den ? av_q2d(vs->avg_frame_rate) : 0.0;
		say("PLAYER: %s %dx%d %.3f fps, sound %s, %.0f s", avcodec_get_name(vs->codecpar->codec_id), vs->codecpar->width,
			vs->codecpar->height, fps, actx ? avcodec_get_name(fc->streams[ai]->codecpar->codec_id) : "none", duration);

		// A subtitle track the file carries as pictures is decoded here; a text track comes from the server as SRT
		const int canvasFallbackW = vs->codecpar->width, canvasFallbackH = vs->codecpar->height;
		auto selectSub = [&](int idx) {
			avcodec_free_context(&subctx);
			bitmapStream = -1;
			{
				std::lock_guard<std::mutex> lock(bitmapLock);
				bitmaps.clear();
			}
			if (idx >= 0 && idx < static_cast<int>(fc->nb_streams) &&
				fc->streams[idx]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE &&
				isBitmapSubtitle(fc->streams[idx]->codecpar->codec_id)) {
				startSubtitle(-1); // no SRT for this one
				const AVCodecParameters *cp = fc->streams[idx]->codecpar;
				const AVCodec *dec = avcodec_find_decoder(cp->codec_id);
				if (dec) {
					subctx = avcodec_alloc_context3(dec);
					avcodec_parameters_to_context(subctx, cp);
					if (avcodec_open2(subctx, dec, nullptr) < 0) {
						avcodec_free_context(&subctx);
					}
				}
				curSub = idx;
				if (subctx) {
					bitmapStream = idx;
					say("PLAYER: picture subtitles on stream %d (%s)", idx, avcodec_get_name(cp->codec_id));
				} else {
					say("PLAYER: no decoder for picture subtitles (%s)", avcodec_get_name(cp->codec_id));
					setSubStatus("Picture subtitles: no decoder in this build");
				}
			} else {
				startSubtitle(idx);
			}
		};
		auto bitmapCanvasW = [&]() { return subctx && subctx->width > 0 ? subctx->width : canvasFallbackW; };
		auto bitmapCanvasH = [&]() { return subctx && subctx->height > 0 ? subctx->height : canvasFallbackH; };

		video = std::thread([this, vctx, vs, startOffset, fps] { runVideo(vctx, vs->time_base, startOffset, fps); });
		videoStarted = true;
		phase = 2;
		{
			int subPick = wantSub.load();
			if (subPick < 0) {
				// Stage 36: Settings > Subtitles: off, auto (when the sound is not in the subtitle language) or on
				const std::string mode = prefText("/data/jellyzoom-sub-mode");
				const std::string language = prefText("/data/jellyzoom-lang-sub");
				if ((mode == "on" || mode == "auto") && !language.empty()) {
					const bool soundMatches = ai >= 0 && sameLanguage(streamLanguage(fc->streams[ai]), language);
					if (mode == "on" || !soundMatches) {
						subPick = streamByLanguage(fc, AVMEDIA_TYPE_SUBTITLE, language);
						if (subPick >= 0) {
							say("PLAYER: subtitle track %d chosen by the language setting", subPick);
						}
					}
				}
			}
			if (subPick >= 0) {
				wantSub = subPick;
				selectSub(subPick);
			}
		}

		if (resume > 3.0) {
			seekMs = static_cast<long long>(resume * 1000.0);
			std::lock_guard<std::mutex> lock(audioLock);
			seekTarget = resume;
			skipUntil = resume;
			generation++;
		}
		jf_report_playing(&session, id.c_str(), &pb, static_cast<long long>(resume * 10000000.0));

		AVPacket *pkt = av_packet_alloc();
		AVFrame *fr = av_frame_alloc();
		std::vector<std::int16_t> pcm;
		int outCh = 2; // channels of pcm: 8 for 5.1 / 7.1 sound when surround is on
		double audioNext = 0.0;
		auto lastReport = Clock::now();
		AVRational atb = ai >= 0 ? fc->streams[ai]->time_base : AVRational{ 1, kRate };

		while (!stopFlag) {
			// a subtitle track picked while the film plays
			const int wantedSub = pendingSub.exchange(-2);
			if (wantedSub != -2) {
				selectSub(wantedSub);
				if (bitmapStream.load() >= 0) {
					// the picture track starts mid-film: read on from here so its next picture is not already behind us
					requestSeek(position());
					continue;
				}
			}
			// another sound track: open its decoder, then read on from here with it
			const int chosen = pendingAudio.exchange(-1);
			if (chosen >= 0 && chosen != ai && chosen < static_cast<int>(fc->nb_streams) &&
				fc->streams[chosen]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
				AVStream *as = fc->streams[chosen];
				const AVCodec *acodec = avcodec_find_decoder(as->codecpar->codec_id);
				AVCodecContext *next = nullptr;
				if (acodec) {
					next = avcodec_alloc_context3(acodec);
					avcodec_parameters_to_context(next, as->codecpar);
					if (avcodec_open2(next, acodec, nullptr) < 0) {
						avcodec_free_context(&next);
					}
				}
				if (next) {
					avcodec_free_context(&actx);
					actx = next;
					swr_free(&swr);
					ai = chosen;
					atb = as->time_base;
					curAudio = chosen;
					audioInfo(as);
					chooseSound(as);
					say("PLAYER: sound track %d (%s)", chosen, avcodec_get_name(as->codecpar->codec_id));
					if (!hasAudio) {
						hasAudio = true;
					}
					requestSeek(position());
					continue;
				}
				say("PLAYER: no decoder for sound track %d (%s)", chosen, avcodec_get_name(as->codecpar->codec_id));
				noteSet(std::string("This device cannot decode ") + avcodec_get_name(as->codecpar->codec_id) + " yet");
			}
			// a seek: the request holds the newest target
			const long long req = seekMs.exchange(-1);
			if (req >= 0) {
				double target = static_cast<double>(req) / 1000.0;
				if (duration > 0.0) {
					target = std::min(target, std::max(0.0, duration - 2.0));
				}
				target = std::max(0.0, target);
				const unsigned gen = generation.load();
				say("PLAYER: seek to %.1f s", target);
				av_seek_frame(fc, -1, static_cast<int64_t>((target + startOffset) * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
				if (actx) {
					avcodec_flush_buffers(actx);
				}
				swr_free(&swr);
				if (subctx) {
					avcodec_flush_buffers(subctx);
				}
				{
					std::lock_guard<std::mutex> lock(bitmapLock);
					bitmaps.clear();
				}
				skipUntil = target;
				clearPackets();
				queuePacket(nullptr, gen, true);
				eof = false;
				continue;
			}
			if (eof) {
				// the end: wait for the sound to run out, or for a seek
				bool soundDone;
				{
					std::lock_guard<std::mutex> lock(audioLock);
					soundDone = !hasAudio || ringCount == 0;
				}
				bool picturesDone;
				{
					std::lock_guard<std::mutex> lock(packetLock);
					picturesDone = packets.empty();
				}
				if (soundDone && picturesDone && phase == 2) {
					phase = 3;
					say("PLAYER: the film ended");
				}
				sleepMs(30);
				continue;
			}
			if (phase == 2 && Clock::now() - lastReport > std::chrono::seconds(10)) {
				lastReport = Clock::now();
				jf_report_progress(&session, id.c_str(), &pb, static_cast<long long>(position() * 10000000.0),
					pausedFlag ? 1 : 0);
			}
			{
				std::lock_guard<std::mutex> lock(packetLock);
				if (packetBytes > kPacketBytes) {
					sleepMs(5);
					continue;
				}
			}
			const int got = av_read_frame(fc, pkt);
			if (got < 0) {
				if (got == AVERROR_EXIT || stopFlag) {
					break;
				}
				say("PLAYER: read ended (%d)", got);
				eof = true;
				queuePacket(nullptr, generation.load(), false);
				continue;
			}
			const unsigned gen = generation.load();
			if (pkt->stream_index == vi) {
				queuePacket(pkt, gen, false);
				pkt = av_packet_alloc();
			} else if (subctx && pkt->stream_index == bitmapStream.load()) {
				AVSubtitle event;
				std::memset(&event, 0, sizeof(event));
				int got = 0;
				if (avcodec_decode_subtitle2(subctx, &event, &got, pkt) >= 0 && got) {
					takeBitmapEvent(event, pkt, startOffset, bitmapCanvasW(), bitmapCanvasH());
					avsubtitle_free(&event);
				}
			} else if (bsKind.load() != 0 && pkt->stream_index == ai) {
				// Stage 17: the sound track goes to the soundbar as it is, one burst a packet
				int samples = 0;
				size_t n = 0;
				bool collecting = false;
				if (bsKind.load() == kMat) {
					// Stage 39: twenty-four access units make one MAT frame; the frame carries the time of its first
					if (matPacker.count == 0) {
						matFirstPts = pkt->pts != AV_NOPTS_VALUE ? static_cast<double>(pkt->pts) * av_q2d(atb) - startOffset : audioNext;
					}
					const int got = matPacker.add(pkt->data, pkt->size, bsBuf);
					if (got > 0) {
						n = static_cast<size_t>(got);
						samples = 960;
					} else if (got == 0) {
						collecting = true;
					}
				} else {
					n = jfiec::pack(bsKind.load(), pkt->data, pkt->size, bsBuf, &samples);
				}
				if (collecting) {
					// more units to come before there is a burst to send
				} else if (n == 0) {
					bsBad++;
					if (bsBad.load() <= 3) {
						char hex[100];
						int at = 0;
						for (int i = 0; i < 24 && i < pkt->size && at < 90; i++) {
							at += std::snprintf(hex + at, sizeof(hex) - static_cast<size_t>(at), " %02x", pkt->data[i]);
						}
						say("AUDIO: a packet of %d bytes the packer cannot send (kind %d); first bytes:%s", pkt->size, bsKind.load(), hex);
					}
					if (++bsBadRun >= 8 && bsBursts.load() == 0) {
						say("AUDIO: nothing in this track can be sent as it is: the sound is decoded here instead");
						noteSet("Sound: this track cannot be sent to the soundbar as it is, so it is decoded here");
						bsStop();
					}
				} else {
					bsBadRun = 0;
					double pts = bsKind.load() == kMat ? matFirstPts : (pkt->pts != AV_NOPTS_VALUE ? static_cast<double>(pkt->pts) * av_q2d(atb) - startOffset : audioNext);
					audioNext = pts + static_cast<double>(samples) / kRate;
					if (!bsFirstLogged) {
						bsFirstLogged = true;
						say("AUDIO: first burst, %zu bytes for a %d byte packet, header %02x %02x %02x %02x %02x %02x %02x %02x, pts %.3f",
							n, pkt->size, bsBuf[0], bsBuf[1], bsBuf[2], bsBuf[3], bsBuf[4], bsBuf[5], bsBuf[6], bsBuf[7], pts);
					}
					if (audioNext > skipUntil.load()) {
						pushAudio(reinterpret_cast<const std::int16_t *>(bsBuf.data()), static_cast<int>(n / 4), 2, pts, gen);
						bsBursts++;
					}
				}
			} else if (actx && pkt->stream_index == ai) {
				avcodec_send_packet(actx, pkt);
				while (!stopFlag && avcodec_receive_frame(actx, fr) == 0) {
					if (!swr) {
						AVChannelLayout out = AV_CHANNEL_LAYOUT_STEREO;
						outCh = 2;
						if (surround.load() && fr->ch_layout.nb_channels > 2) {
							const AVChannelLayout surround71 = AV_CHANNEL_LAYOUT_7POINT1;
							out = surround71; // FL FR FC LFE BL BR SL SR: the console's own order
							outCh = 8;
						}
						{
							std::lock_guard<std::mutex> lock(textLock);
							infoData.outChannels = outCh == 8 ? std::min(8, fr->ch_layout.nb_channels) : 2;
						}
						if (swr_alloc_set_opts2(&swr, &out, AV_SAMPLE_FMT_S16, kRate, &fr->ch_layout,
								static_cast<AVSampleFormat>(fr->format), fr->sample_rate, 0, nullptr) < 0 ||
							(av_opt_set_double(swr, "center_mix_level", 0.7071, 0),   // -3 dB into both sides
							 av_opt_set_double(swr, "surround_mix_level", 0.7071, 0),
							 av_opt_set_double(swr, "lfe_mix_level", 0.35, 0),         // a little of the bass channel
							 av_opt_set_double(swr, "rematrix_maxval", 1.0, 0),        // scaled down, never clipped
							 swr_init(swr) < 0)) {
							say("PLAYER: sound converter failed");
							swr_free(&swr);
							av_frame_unref(fr);
							break;
						}
					}
					double pts = fr->best_effort_timestamp != AV_NOPTS_VALUE
						? static_cast<double>(fr->best_effort_timestamp) * av_q2d(atb) - startOffset
						: audioNext;
					audioNext = pts + static_cast<double>(fr->nb_samples) / fr->sample_rate;
					const int cap = swr_get_out_samples(swr, fr->nb_samples);
					int frames = 0;
					if (cap > 0) {
						pcm.resize(static_cast<size_t>(cap) * static_cast<size_t>(outCh));
						std::uint8_t *o[1] = { reinterpret_cast<std::uint8_t *>(pcm.data()) };
						frames = swr_convert(swr, o, cap, const_cast<const std::uint8_t **>(fr->extended_data), fr->nb_samples);
					}
					av_frame_unref(fr);
					if (frames <= 0) {
						continue;
					}
					// after a seek the keyframe is earlier than the target: drop the sound before it
					const double skip = skipUntil.load();
					int from = 0;
					if (pts + static_cast<double>(frames) / kRate <= skip) {
						continue;
					}
					if (pts < skip) {
						from = static_cast<int>((skip - pts) * kRate);
						pts = skip;
					}
					if (from >= frames) {
						continue;
					}
					pushAudio(pcm.data() + static_cast<size_t>(from) * static_cast<size_t>(outCh), frames - from, outCh, pts, gen);
				}
			}
			av_packet_unref(pkt);
		}

		// the end of the session: tell the server where it stopped
		jf_report_stopped(&session, id.c_str(), &pb, static_cast<long long>(position() * 10000000.0));
		av_packet_free(&pkt);
		av_frame_free(&fr);
		say("PLAYER: closed");
	}
};

Player::Player() : impl_(new Impl()) {}

Player::~Player()
{
	close();
}

void Player::open(int gateIndex, double resumeSeconds, int audioStream, int subtitleStream, bool waitForPresentation)
{
	close();
	Impl &p = *impl_;
	p.resetSession();
	p.presentationReady = !waitForPresentation;
	p.wantAudio = audioStream;
	p.wantSub = subtitleStream;
	p.phase = 1;
	p.demux = std::thread([&p, gateIndex, resumeSeconds] { p.runDemux(gateIndex, resumeSeconds); });
}

void Player::close()
{
	Impl &p = *impl_;
	if (p.demux.joinable()) {
		p.stopFlag = true;
		p.packetCv.notify_all();
		p.demux.join();
	}
	p.phase = 0;
}

bool Player::closeAndMarkWatched()
{
	Impl &p = *impl_;
	std::string id;
	{
		std::lock_guard<std::mutex> lock(p.textLock);
		id = p.itemId;
	}
	close();
	JfSession session{};
	if (id.empty() || !jf_gate_session(&session)) {
		say("PLAYER: could not mark watched: missing item or session");
		return false;
	}
	if (jf_mark_played(&session, id.c_str()) != 0) {
		say("PLAYER: could not mark watched: %s", jf_last_error());
		return false;
	}
	return true;
}

State Player::state() const
{
	const int phase = impl_->phase.load();
	switch (phase) {
	case 1: return State::opening;
	case 2:
		if (!impl_->ready()) {
			return State::buffering;
		}
		return impl_->pausedFlag ? State::paused : State::playing;
	case 3: return State::ended;
	case 4: return State::failed;
	default: return State::idle;
	}
}

std::string Player::error() const
{
	std::lock_guard<std::mutex> lock(impl_->textLock);
	return impl_->errorText;
}

std::string Player::title() const
{
	std::lock_guard<std::mutex> lock(impl_->textLock);
	return impl_->titleText;
}

double Player::position() const
{
	return impl_->position();
}

double Player::duration() const
{
	return impl_->durationSec.load();
}

double Player::buffered() const
{
	std::lock_guard<std::mutex> lock(impl_->audioLock);
	return static_cast<double>(impl_->ringCount / 2) / (static_cast<double>(kRate) * impl_->ringMul.load());
}

bool Player::paused() const
{
	return impl_->pausedFlag.load();
}

void Player::setPaused(bool paused)
{
	impl_->pausedFlag = paused;
}

void Player::seek(double seconds)
{
	impl_->requestSeek(seconds);
}

void Player::setAudio(int streamIndex)
{
	Impl &p = *impl_;
	if (streamIndex < 0 || p.phase.load() != 2) {
		return;
	}
	p.pendingAudio = streamIndex;
}

int Player::audioStream() const
{
	return impl_->curAudio.load();
}

void Player::setSubtitle(int streamIndex)
{
	Impl &p = *impl_;
	p.wantSub = streamIndex;
	if (p.phase.load() == 2) {
		p.pendingSub = streamIndex; // the demux thread knows what the file holds
	}
}

int Player::subtitleStream() const
{
	return impl_->wantSub.load();
}

std::string Player::subtitleAt(double seconds) const
{
	Impl &p = *impl_;
	std::shared_ptr<SubState> state;
	{
		std::lock_guard<std::mutex> lock(p.subLock);
		state = p.sub;
	}
	if (!state) {
		return std::string();
	}
	std::lock_guard<std::mutex> lock(state->m);
	const std::vector<Cue> &cues = state->cues;
	if (cues.empty()) {
		return std::string();
	}
	// the last cue that has started, then a few before it in case they overlap
	size_t lo = 0, hi = cues.size();
	while (lo < hi) {
		const size_t mid = (lo + hi) / 2;
		if (cues[mid].start <= seconds) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	for (size_t i = lo, n = 0; i > 0 && n < 6; i--, n++) {
		const Cue &c = cues[i - 1];
		if (c.start <= seconds && seconds < c.end) {
			return c.text;
		}
	}
	return std::string();
}

std::vector<SubPicture> Player::subtitlePictures(double seconds) const
{
	std::vector<SubPicture> out;
	Impl &p = *impl_;
	std::lock_guard<std::mutex> lock(p.bitmapLock);
	for (const Impl::PicEvent &b : p.bitmaps) {
		if (b.start <= seconds && seconds < b.end && b.rgba) {
			SubPicture s;
			s.id = b.id;
			s.x = b.x;
			s.y = b.y;
			s.w = b.w;
			s.h = b.h;
			s.cw = b.cw;
			s.ch = b.ch;
			s.rgba = b.rgba;
			out.push_back(s);
		}
	}
	return out;
}

std::string Player::subtitleStatus() const
{
	Impl &p = *impl_;
	std::shared_ptr<SubState> state;
	{
		std::lock_guard<std::mutex> lock(p.subLock);
		state = p.sub;
	}
	if (!state) {
		return std::string();
	}
	std::lock_guard<std::mutex> lock(state->m);
	return state->status;
}

Info Player::info() const
{
	std::lock_guard<std::mutex> lock(impl_->textLock);
	Info copy = impl_->infoData;
	copy.dropped = impl_->lateTotal.load();
	return copy;
}

std::string Player::takeNote()
{
	std::lock_guard<std::mutex> lock(impl_->textLock);
	std::string text;
	text.swap(impl_->noteText);
	return text;
}

void Player::setVolume(float level, bool muted)
{
	impl_->volume = std::min(1.0f, std::max(0.0f, level));
	impl_->muted = muted;
}

std::vector<Chapter> Player::chapters() const
{
	std::lock_guard<std::mutex> lock(impl_->textLock);
	return impl_->chapterList;
}

bool Player::present(Frame *out)
{
	Impl &p = *impl_;
	if (!p.ready()) {
		p.presentMetrics.reset();
		return false;
	}
	const double now = p.position();
	bool have = false;
	std::uint64_t skipped = 0;
	{
		std::lock_guard<std::mutex> lock(p.videoLock);
		VFrame candidate;
		while (!p.frames.empty() && p.frames.front().pts <= now + 0.05) {
			if (have) {
				p.recycle(std::move(candidate));
				++skipped;
			}
			candidate = std::move(p.frames.front());
			p.frames.pop_front();
			have = true;
		}
		if (have) {
			p.recycle(std::move(p.shown));
			p.shown = std::move(candidate);
			p.serial++;
			out->rgba = p.shown.rgba.data();
			out->width = p.shown.w;
			out->height = p.shown.h;
			out->serial = p.serial;
			out->hdr = p.shown.hdr;
		}
	}
	const unsigned gen = p.generation.load();
	if (p.presentGeneration != gen || p.pausedFlag || p.phase != 2) {
		p.presentMetrics.reset();
		p.presentGeneration = gen;
	} else {
		const double nowMs = std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count();
		PresentReport report;
		if (p.presentMetrics.observe(nowMs, have, skipped, report)) {
			say("PLAYER PRESENT: %.1f s, %llu render calls, %llu pictures selected, %llu presentation skips, "
				"render gaps >25ms %llu >40ms %llu max %.1f ms, max picture gap %.1f ms",
				report.seconds, static_cast<unsigned long long>(report.calls), static_cast<unsigned long long>(report.pictures),
				static_cast<unsigned long long>(report.skipped), static_cast<unsigned long long>(report.gapsOver25ms),
				static_cast<unsigned long long>(report.gapsOver40ms), report.maxCallGapMs, report.maxPictureGapMs);
		}
	}
	return have;
}

void Player::mixInto(std::int16_t *out, int frames)
{
	impl_->mixChannels(2, out, frames);
}

void Player::mixInto8(std::int16_t *out, int frames)
{
	impl_->mixChannels(8, out, frames);
}

int Player::outputChannels() const
{
	return impl_->outChannels.load();
}

void Player::setSurround(bool on)
{
	impl_->surround = on;
}

void Player::setHdrOutput(bool on)
{
	impl_->hdrOut = on;
}

bool Player::hdrOutput() const
{
	return impl_->hdrOut.load();
}

void Player::setPresentationReady()
{
	std::lock_guard<std::mutex> lock(impl_->audioLock);
	impl_->wallTick = Clock::now();
	impl_->presentationReady = true;
}

} // namespace jfp
