/* AV1 pictures from dav1d (BSD-2-Clause), handed to the player as ordinary AVFrames. FFmpeg's own AV1 decoder wants
 * hardware the console does not offer, so the player feeds AV1 packets here and takes YUV pictures back: dav1d's
 * planes are used where they are (no copy); the AVFrame's buffer holds the picture until the frame is unreferenced.
 * Only built when the project has vendor/dav1d-ps5 (ps5/tools/build-dav1d.sh makes it): JF_HAVE_DAV1D. */
#pragma once
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>

extern "C" {
#ifdef JF_HAVE_DAV1D
#include <dav1d/dav1d.h>
#endif
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixfmt.h>
}

namespace jfav1
{
using LogFn = void (*)(const char *line);

#ifdef JF_HAVE_DAV1D

class Decoder
{
public:
	~Decoder() { close(); }

	bool open(int threads, LogFn log)
	{
		close();
		log_ = log;
		Dav1dSettings s;
		dav1d_default_settings(&s);
		s.n_threads = threads;
		s.max_frame_delay = 0; // dav1d chooses
		s.apply_grain = 1;
		s.all_layers = 0;
		s.logger.cookie = this;
		s.logger.callback = &Decoder::logTrampoline;
		if (dav1d_open(&ctx_, &s) < 0) {
			ctx_ = nullptr;
			return false;
		}
		pictures_ = 0;
		ms_ = 0.0;
		return true;
	}

	void close()
	{
		for (Dav1dPicture &p : ready_) {
			dav1d_picture_unref(&p);
		}
		ready_.clear();
		if (ctx_) {
			dav1d_close(&ctx_);
		}
	}

	void flush()
	{
		for (Dav1dPicture &p : ready_) {
			dav1d_picture_unref(&p);
		}
		ready_.clear();
		if (ctx_) {
			dav1d_flush(ctx_);
		}
	}

	// A packet (a null one: the end of the file, only drain). False: dav1d refused the data (it is dropped, not fatal).
	bool send(const AVPacket *pkt)
	{
		if (!ctx_ || !pkt || pkt->size <= 0) {
			return true;
		}
		const auto t0 = std::chrono::steady_clock::now();
		Dav1dData d;
		std::memset(&d, 0, sizeof(d));
		std::uint8_t *buf = dav1d_data_create(&d, static_cast<size_t>(pkt->size));
		if (!buf) {
			return false;
		}
		std::memcpy(buf, pkt->data, static_cast<size_t>(pkt->size));
		d.m.timestamp = pkt->pts;
		bool ok = true;
		while (d.sz > 0) {
			const int r = dav1d_send_data(ctx_, &d);
			if (r == DAV1D_ERR(EAGAIN)) {
				// its queue is full: take pictures out first
				if (!pull()) {
					break;
				}
				continue;
			}
			if (r < 0) {
				ok = false;
				break;
			}
		}
		if (d.sz > 0) {
			dav1d_data_unref(&d);
		}
		ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		return ok;
	}

	// The next picture as an AVFrame; false when there is none yet
	bool receive(AVFrame *fr)
	{
		if (!ctx_) {
			return false;
		}
		if (ready_.empty()) {
			const auto t0 = std::chrono::steady_clock::now();
			pull();
			ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		}
		if (ready_.empty()) {
			return false;
		}
		Dav1dPicture pic = ready_.front();
		ready_.pop_front();
		return wrap(pic, fr);
	}

	static const char *version() { return dav1d_version(); }
	long long pictures() const { return pictures_; }
	double decodeMs() const { return ms_; }

private:
	Dav1dContext *ctx_{ nullptr };
	LogFn log_{ nullptr };
	std::deque<Dav1dPicture> ready_;
	long long pictures_{ 0 };
	double ms_{ 0.0 };

	static void logTrampoline(void *cookie, const char *fmt, va_list ap)
	{
		char line[300];
		std::vsnprintf(line, sizeof(line), fmt, ap);
		size_t n = std::strlen(line);
		while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
			line[--n] = 0;
		}
		Decoder *self = static_cast<Decoder *>(cookie);
		if (self && self->log_) {
			char out[330];
			std::snprintf(out, sizeof(out), "AV1: dav1d: %s", line);
			self->log_(out);
		}
	}

	// All the pictures dav1d has ready go to ready_. False on a hard error.
	bool pull()
	{
		for (;;) {
			Dav1dPicture pic;
			std::memset(&pic, 0, sizeof(pic));
			const int r = dav1d_get_picture(ctx_, &pic);
			if (r == DAV1D_ERR(EAGAIN)) {
				return true;
			}
			if (r < 0) {
				return false;
			}
			ready_.push_back(pic);
		}
	}

	static void releasePicture(void *opaque, std::uint8_t *)
	{
		Dav1dPicture *p = static_cast<Dav1dPicture *>(opaque);
		dav1d_picture_unref(p);
		delete p;
	}

	bool wrap(Dav1dPicture &pic, AVFrame *fr)
	{
		AVPixelFormat fmt = AV_PIX_FMT_NONE;
		const int bpc = pic.p.bpc;
		switch (pic.p.layout) {
		case DAV1D_PIXEL_LAYOUT_I400:
			fmt = bpc == 8 ? AV_PIX_FMT_GRAY8 : bpc == 10 ? AV_PIX_FMT_GRAY10LE : AV_PIX_FMT_GRAY12LE;
			break;
		case DAV1D_PIXEL_LAYOUT_I420:
			fmt = bpc == 8 ? AV_PIX_FMT_YUV420P : bpc == 10 ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P12LE;
			break;
		case DAV1D_PIXEL_LAYOUT_I422:
			fmt = bpc == 8 ? AV_PIX_FMT_YUV422P : bpc == 10 ? AV_PIX_FMT_YUV422P10LE : AV_PIX_FMT_YUV422P12LE;
			break;
		case DAV1D_PIXEL_LAYOUT_I444:
			fmt = bpc == 8 ? AV_PIX_FMT_YUV444P : bpc == 10 ? AV_PIX_FMT_YUV444P10LE : AV_PIX_FMT_YUV444P12LE;
			break;
		}
		if (fmt == AV_PIX_FMT_NONE) {
			dav1d_picture_unref(&pic);
			return false;
		}
		Dav1dPicture *keep = new Dav1dPicture(pic); // the AVFrame owns the picture from here
		fr->buf[0] = av_buffer_create(static_cast<std::uint8_t *>(keep->data[0]), 1, &Decoder::releasePicture, keep, 0);
		if (!fr->buf[0]) {
			dav1d_picture_unref(keep);
			delete keep;
			return false;
		}
		fr->format = fmt;
		fr->width = keep->p.w;
		fr->height = keep->p.h;
		fr->data[0] = static_cast<std::uint8_t *>(keep->data[0]);
		fr->linesize[0] = static_cast<int>(keep->stride[0]);
		if (keep->p.layout != DAV1D_PIXEL_LAYOUT_I400) {
			fr->data[1] = static_cast<std::uint8_t *>(keep->data[1]);
			fr->data[2] = static_cast<std::uint8_t *>(keep->data[2]);
			fr->linesize[1] = fr->linesize[2] = static_cast<int>(keep->stride[1]);
		}
		fr->extended_data = fr->data;
		fr->pts = fr->best_effort_timestamp = keep->m.timestamp; // INT64_MIN is AV_NOPTS_VALUE in both
		if (keep->seq_hdr) {
			const Dav1dSequenceHeader *sh = keep->seq_hdr;
			fr->color_primaries = static_cast<AVColorPrimaries>(sh->pri);
			fr->color_trc = static_cast<AVColorTransferCharacteristic>(sh->trc);
			fr->colorspace = static_cast<AVColorSpace>(sh->mtrx);
			fr->color_range = sh->color_range ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
			fr->chroma_location = sh->chr == 1 ? AVCHROMA_LOC_LEFT : sh->chr == 2 ? AVCHROMA_LOC_TOPLEFT : AVCHROMA_LOC_UNSPECIFIED;
		}
		if (keep->mastering_display) {
			const Dav1dMasteringDisplay *m = keep->mastering_display;
			if (AVMasteringDisplayMetadata *md = av_mastering_display_metadata_create_side_data(fr)) {
				for (int i = 0; i < 3; i++) {
					md->display_primaries[i][0] = av_make_q(m->primaries[i][0], 65536);
					md->display_primaries[i][1] = av_make_q(m->primaries[i][1], 65536);
				}
				md->white_point[0] = av_make_q(m->white_point[0], 65536);
				md->white_point[1] = av_make_q(m->white_point[1], 65536);
				md->max_luminance = av_make_q(static_cast<int>(m->max_luminance), 256);
				md->min_luminance = av_make_q(static_cast<int>(m->min_luminance), 16384);
				md->has_primaries = md->has_luminance = 1;
			}
		}
		if (keep->content_light) {
			if (AVContentLightMetadata *cl = av_content_light_metadata_create_side_data(fr)) {
				cl->MaxCLL = keep->content_light->max_content_light_level;
				cl->MaxFALL = keep->content_light->max_frame_average_light_level;
			}
		}
		pictures_++;
		return true;
	}
};
#else
// Without vendor/dav1d-ps5 the same interface does nothing, and AV1 files are refused as before
class Decoder
{
public:
	bool open(int, LogFn) { return false; }
	void close() {}
	void flush() {}
	bool send(const AVPacket *) { return false; }
	bool receive(AVFrame *) { return false; }
	static const char *version() { return "none"; }
	long long pictures() const { return 0; }
	double decodeMs() const { return 0.0; }
};
#endif
} // namespace jfav1
