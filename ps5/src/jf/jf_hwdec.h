/* The console's hardware video decoder (sceVideodec2) for the film player: H.264 and HEVC (Main, Main10) up to 4K.
 * One decoder per film, used from the video thread only. A film it cannot take stays with the software decoder.
 * Every log line starts with HWDEC. */
#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

namespace jfhw
{
class Decoder
{
public:
	Decoder();
	~Decoder();
	Decoder(const Decoder &) = delete;
	Decoder &operator=(const Decoder &) = delete;

	// Brings the decoder up for the stream ctx describes. false: use the software decoder (the reason is in klog).
	bool open(const AVCodecContext *ctx);
	bool active() const;
	// One compressed packet; a null packet means the end of the file and drains what is still inside.
	// false: the decoder has failed and the software decoder must take over.
	bool send(const AVPacket *pkt);
	// The next picture in display order, as an NV12 (8-bit) or P010 (10-bit) frame whose planes stay in the decoder's
	// memory until eight more pictures have come out. true: *fr is filled.
	bool receive(AVFrame *fr);
	// A seek: forget everything in flight and start again at the next key frame.
	void flush();
	void close();

private:
	struct Impl;
	Impl *impl_;
};
} // namespace jfhw
