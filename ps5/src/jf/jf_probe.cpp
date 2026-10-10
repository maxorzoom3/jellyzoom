/* Stage 1 probe: can FFmpeg run inside a title?
 * Opens one real stream from the Jellyfin server, lists its streams, decodes a
 * few seconds of video (one thread, then four) and audio, and times it.
 * Every log line starts with FFPROBE. A crash leaves the last "step:" line as the
 * place it happened. */
#include "jf_probe.h"
#include "jf_avio.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" {
#include "platform.h"
#include "jf_config.h"
#include "jf_types.h"
#include "jellyfin.h"
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

namespace {
using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t)
{
	return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

struct Guard {
	std::atomic<bool> *stop;
	Clock::time_point deadline;
};

int interrupt(void *p)
{
	Guard *g = static_cast<Guard *>(p);
	return (g->stop && g->stop->load()) || Clock::now() > g->deadline;
}

void avLog(void *, int level, const char *fmt, va_list vl)
{
	if (level > AV_LOG_WARNING) {
		return;
	}
	char buf[512];
	vsnprintf(buf, sizeof(buf), fmt, vl);
	size_t n = std::strlen(buf);
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
		buf[--n] = 0;
	}
	if (n) {
		say("FFPROBE: av: %s", buf);
	}
}

void listStreams(AVFormatContext *fc)
{
	for (unsigned i = 0; i < fc->nb_streams; i++) {
		AVStream *s = fc->streams[i];
		AVCodecParameters *p = s->codecpar;
		AVDictionaryEntry *lang = av_dict_get(s->metadata, "language", nullptr, 0);
		const char *l = lang ? lang->value : "-";
		if (p->codec_type == AVMEDIA_TYPE_VIDEO) {
			say("FFPROBE: stream %u video %s %dx%d %s %.2f fps, %lld kb/s", i, avcodec_get_name(p->codec_id), p->width,
				p->height, av_get_pix_fmt_name(static_cast<AVPixelFormat>(p->format)) ?: "?",
				s->avg_frame_rate.den ? av_q2d(s->avg_frame_rate) : 0.0, static_cast<long long>(p->bit_rate / 1000));
		} else if (p->codec_type == AVMEDIA_TYPE_AUDIO) {
			say("FFPROBE: stream %u audio %s %d ch %d Hz lang %s, %lld kb/s", i, avcodec_get_name(p->codec_id),
				p->ch_layout.nb_channels, p->sample_rate, l, static_cast<long long>(p->bit_rate / 1000));
		} else if (p->codec_type == AVMEDIA_TYPE_SUBTITLE) {
			say("FFPROBE: stream %u subtitle %s lang %s", i, avcodec_get_name(p->codec_id), l);
		} else {
			say("FFPROBE: stream %u other (type %d)", i, static_cast<int>(p->codec_type));
		}
	}
}

AVCodecContext *openDecoder(AVStream *st, int threads)
{
	const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
	if (!codec) {
		say("FFPROBE: no decoder for %s", avcodec_get_name(st->codecpar->codec_id));
		return nullptr;
	}
	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	if (!ctx) {
		return nullptr;
	}
	avcodec_parameters_to_context(ctx, st->codecpar);
	ctx->thread_count = threads;
	ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
	int rc = avcodec_open2(ctx, codec, nullptr);
	if (rc < 0) {
		char err[128];
		av_strerror(rc, err, sizeof(err));
		say("FFPROBE: avcodec_open2(%s) failed: %s", codec->name, err);
		avcodec_free_context(&ctx);
		return nullptr;
	}
	return ctx;
}

struct PassResult {
	int frames{ 0 };
	double decodeMs{ 0 };
	double scaleMs{ 0 };
	long long audioFrames{ 0 };
	long long audioSamples{ 0 };
};

PassResult runPass(AVFormatContext *fc, int vi, int ai, int threads, int maxFrames, bool scale, bool audio,
	std::atomic<bool> &stop, Guard &guard)
{
	PassResult r;
	AVCodecContext *vctx = openDecoder(fc->streams[vi], threads);
	AVCodecContext *actx = nullptr;
	if (audio && ai >= 0) {
		actx = openDecoder(fc->streams[ai], 1);
	}
	if (!vctx) {
		return r;
	}
	say("FFPROBE: step: pass with %d thread(s): decoder %s open", threads, vctx->codec->name);

	AVPacket *pkt = av_packet_alloc();
	AVFrame *fr = av_frame_alloc();
	SwsContext *sws = nullptr;
	uint8_t *rgba = nullptr;
	SwrContext *swr = nullptr;
	std::vector<int16_t> pcm;
	const int dstW = 1920, dstH = 1080;
	int packets = 0;

	while (r.frames < maxFrames && !stop && packets < 6000 && av_read_frame(fc, pkt) >= 0) {
		packets++;
		guard.deadline = Clock::now() + std::chrono::seconds(30);
		if (pkt->stream_index == vi) {
			auto t0 = Clock::now();
			avcodec_send_packet(vctx, pkt);
			r.decodeMs += msSince(t0);
			while (true) {
				t0 = Clock::now();
				int got = avcodec_receive_frame(vctx, fr);
				r.decodeMs += msSince(t0);
				if (got != 0) {
					break;
				}
				if (r.frames == 0) {
					say("FFPROBE: first video frame %dx%d %s", fr->width, fr->height,
						av_get_pix_fmt_name(static_cast<AVPixelFormat>(fr->format)) ?: "?");
				}
				if (scale) {
					auto t1 = Clock::now();
					sws = sws_getCachedContext(sws, fr->width, fr->height, static_cast<AVPixelFormat>(fr->format), dstW,
						dstH, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
					if (!rgba) {
						rgba = static_cast<uint8_t *>(std::malloc(static_cast<size_t>(dstW) * dstH * 4));
					}
					if (sws && rgba) {
						uint8_t *dst[1] = { rgba };
						int dstStride[1] = { dstW * 4 };
						sws_scale(sws, fr->data, fr->linesize, 0, fr->height, dst, dstStride);
					}
					r.scaleMs += msSince(t1);
				}
				r.frames++;
				if (r.frames >= maxFrames) {
					break;
				}
			}
		} else if (actx && pkt->stream_index == ai) {
			avcodec_send_packet(actx, pkt);
			while (avcodec_receive_frame(actx, fr) == 0) {
				if (!swr) {
					AVChannelLayout out = AV_CHANNEL_LAYOUT_STEREO;
					int rc = swr_alloc_set_opts2(&swr, &out, AV_SAMPLE_FMT_S16, 48000, &fr->ch_layout,
						static_cast<AVSampleFormat>(fr->format), fr->sample_rate, 0, nullptr);
					if (rc < 0 || swr_init(swr) < 0) {
						say("FFPROBE: swresample init failed");
						swr_free(&swr);
					} else {
						say("FFPROBE: first audio frame %s %d Hz %d ch %d samples, converting to s16 stereo 48000",
							av_get_sample_fmt_name(static_cast<AVSampleFormat>(fr->format)), fr->sample_rate,
							fr->ch_layout.nb_channels, fr->nb_samples);
					}
				}
				if (swr) {
					int cap = swr_get_out_samples(swr, fr->nb_samples);
					if (cap > 0) {
						pcm.resize(static_cast<size_t>(cap) * 2);
						uint8_t *o[1] = { reinterpret_cast<uint8_t *>(pcm.data()) };
						int got = swr_convert(swr, o, cap, const_cast<const uint8_t **>(fr->extended_data),
							fr->nb_samples);
						if (got > 0) {
							r.audioSamples += got;
						}
					}
				}
				r.audioFrames++;
			}
		}
		av_packet_unref(pkt);
	}
	say("FFPROBE: read %d packets", packets);

	std::free(rgba);
	sws_freeContext(sws);
	swr_free(&swr);
	av_frame_free(&fr);
	av_packet_free(&pkt);
	avcodec_free_context(&vctx);
	avcodec_free_context(&actx);
	return r;
}

void report(const char *tag, const PassResult &r, double streamFps)
{
	if (r.frames == 0) {
		say("FFPROBE: %s: no video frames decoded", tag);
		return;
	}
	double fps = r.frames / (r.decodeMs / 1000.0);
	say("FFPROBE: %s: %d frames, decode %.0f ms (%.1f fps; stream needs %.1f)", tag, r.frames, r.decodeMs, fps,
		streamFps);
	if (r.scaleMs > 0) {
		say("FFPROBE: %s: swscale to 1920x1080 RGBA %.1f ms a frame", tag, r.scaleMs / r.frames);
	}
	if (r.audioFrames > 0) {
		say("FFPROBE: %s: audio %lld frames, %lld samples = %.2f s", tag, r.audioFrames, r.audioSamples,
			r.audioSamples / 48000.0);
	}
}
} // namespace

void jf_probe(const JfSession *session, const char *itemId, const char *itemName,
	std::atomic<bool> *stopFlag)
{
	std::atomic<bool> never{ false };
	std::atomic<bool> &stop = stopFlag ? *stopFlag : never;

	say("FFPROBE: ===== start, probing \"%s\" =====", itemName);
	say("FFPROBE: libavformat %u libavcodec %u libavutil %u (%s)", avformat_version(), avcodec_version(),
		avutil_version(), av_version_info());
	av_log_set_callback(avLog);

	say("FFPROBE: step: PlaybackInfo");
	JfPlayback pb;
	std::memset(&pb, 0, sizeof(pb));
	if (jf_get_playback_info(session, itemId, &pb) != 0) {
		say("FFPROBE: PlaybackInfo FAILED: %s", jf_last_error());
		return;
	}
	say("FFPROBE: server says container %s, %lld kb/s, directplay %d, %d streams", pb.container,
		static_cast<long long>(pb.bitrate / 1000), pb.direct_play, pb.stream_count);
	for (int i = 0; i < pb.stream_count && i < 24; i++) {
		const JfMediaStream &s = pb.streams[i];
		say("FFPROBE: server stream %d %s %s lang %s \"%s\"%s%s", s.index, s.type, s.codec, s.language, s.display_title,
			s.is_default ? " default" : "", s.is_text_subtitle ? " text" : "");
	}

	char path[1024];
	std::snprintf(path, sizeof(path), "/Videos/%s/stream?Static=true&MediaSourceId=%s&PlaySessionId=%s", itemId,
		pb.media_source_id, pb.play_session_id);
	char headers[4096];
	std::snprintf(headers, sizeof(headers),
		"Authorization: MediaBrowser Client=\"%s\", Device=\"%s\", DeviceId=\"%s\", Version=\"%s\", Token=\"%s\"\r\n",
		JF_CLIENT_NAME, JF_DEVICE_NAME, JF_DEVICE_ID, JF_CLIENT_VERSION, session->access_token);

	say("FFPROBE: step: connect (own sockets)");
	int64_t fileSize = 0;
	AVIOContext *io = jf_avio_open(JF_SERVER_HOST, JF_SERVER_PORT, path, headers, &stop, &fileSize);
	if (!io) {
		say("FFPROBE: could not open the stream over sockets");
		return;
	}
	say("FFPROBE: file is %lld bytes", static_cast<long long>(fileSize));

	Guard guard{ &stop, Clock::now() + std::chrono::seconds(30) };
	AVFormatContext *fc = avformat_alloc_context();
	fc->interrupt_callback.callback = interrupt;
	fc->interrupt_callback.opaque = &guard;
	fc->pb = io;
	fc->flags |= AVFMT_FLAG_CUSTOM_IO;
	struct Closer {
		AVFormatContext *&fc;
		AVIOContext *&io;
		~Closer()
		{
			if (fc) {
				avformat_close_input(&fc);
			}
			jf_avio_close(&io);
		}
	} closer{ fc, io };

	AVDictionary *opts = nullptr;
	av_dict_set(&opts, "probesize", "5000000", 0);
	av_dict_set(&opts, "analyzeduration", "5000000", 0);

	say("FFPROBE: step: avformat_open_input");
	auto t0 = Clock::now();
	int rc = avformat_open_input(&fc, "jellyfin-stream", nullptr, &opts);
	av_dict_free(&opts);
	if (rc < 0) {
		char err[128];
		av_strerror(rc, err, sizeof(err));
		say("FFPROBE: avformat_open_input FAILED: %s (%d)", err, rc);
		return;
	}
	say("FFPROBE: opened in %.0f ms, format %s", msSince(t0), fc->iformat ? fc->iformat->name : "?");

	say("FFPROBE: step: avformat_find_stream_info");
	guard.deadline = Clock::now() + std::chrono::seconds(30);
	t0 = Clock::now();
	rc = avformat_find_stream_info(fc, nullptr);
	if (rc < 0) {
		say("FFPROBE: find_stream_info FAILED (%d)", rc);
		return;
	}
	say("FFPROBE: stream info in %.0f ms, duration %.1f s", msSince(t0), fc->duration / double(AV_TIME_BASE));
	listStreams(fc);

	int vi = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	int ai = av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
	if (vi < 0) {
		say("FFPROBE: no video stream");
		return;
	}
	double fps = fc->streams[vi]->avg_frame_rate.den ? av_q2d(fc->streams[vi]->avg_frame_rate) : 0.0;

	PassResult a = runPass(fc, vi, ai, 1, 90, true, true, stop, guard);
	report("pass A (1 thread, with audio and scale)", a, fps);

	if (!stop) {
		say("FFPROBE: step: seek back to start");
		guard.deadline = Clock::now() + std::chrono::seconds(30);
		if (av_seek_frame(fc, -1, 0, AVSEEK_FLAG_BACKWARD) < 0) {
			say("FFPROBE: seek failed");
		} else {
			PassResult b = runPass(fc, vi, -1, 4, 120, false, false, stop, guard);
			report("pass B (4 threads, decode only)", b, fps);
		}
	}

	say("FFPROBE: ===== done =====");
}
