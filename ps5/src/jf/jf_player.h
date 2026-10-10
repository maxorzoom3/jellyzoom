#pragma once
/* The film player: FFmpeg demux, decode, convert, and an audio clock. No Vulkan and no UI in here;
 * the program asks it for the next picture and mixes its sound into the console's output. */
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jfp
{

enum class State
{
	idle,
	opening,   // connecting and reading the stream's headers
	buffering, // waiting for the first picture at this position
	playing,
	paused,
	ended,
	failed
};

struct Chapter
{
	float time;
	std::string title;
};

// A picture subtitle (Blu-ray PGS, DVD, DVB) showing now: straight RGBA, placed on a canvas of cw x ch
struct SubPicture
{
	std::uint64_t id = 0; // the same picture keeps its id while it shows
	int x = 0, y = 0, w = 0, h = 0;
	int cw = 0, ch = 0;
	std::shared_ptr<const std::vector<std::uint8_t>> rgba;
};

// What FFmpeg found in the file (the real thing, not the server's word for it)
struct Info
{
	std::string container;
	std::string videoCodec;
	int width = 0;
	int height = 0;
	double fps = 0.0;
	int bitDepth = 0;
	std::string range;     // SDR, HDR10, HLG
	bool dovi = false;     // a Dolby Vision configuration is in the stream
	int doviProfile = 0;
	int doviLevel = 0;
	bool doviEnhancement = false; // profile 7: a second (enhancement) layer is present
	std::string audioCodec;
	int audioChannels = 0;
	bool atmos = false;
	std::string audioProfile; // DTS-HD Master Audio, DTS:X ...
	int peakNits = 0;         // the brightest level of an HDR picture
	bool toneMapped = false;  // an HDR picture is squeezed into SDR
	bool hdrOutput = false;   // an HDR10 picture goes to the display as it is (PQ, BT.2020)
	int outChannels = 0;      // channels of the sound that reaches the console: 2, or 6 / 7 / 8 with surround on
	long long dropped = 0;    // pictures skipped because the decoder fell behind
	long long bitrate = 0; // of the whole file, bits a second
	std::string bitstream;    // "Dolby Digital Plus" and the like when the sound goes to the soundbar as it is, else empty
	std::string audioText() const;
};

// A picture whose time has come. The pixels stay valid until the next present() that returns true.
struct Frame
{
	const std::uint8_t *rgba{ nullptr }; // RGBA8; with hdr set: RGBA16, little endian, BT.2020, PQ coded
	int width{ 0 };
	int height{ 0 };
	std::uint64_t serial{ 0 };
	bool hdr{ false };
};

class Player
{
public:
	Player();
	~Player();
	Player(const Player &) = delete;
	Player &operator=(const Player &) = delete;

	// gateIndex is the film's place in jf_gate's list. audioStream / subtitleStream are the server's
	// stream numbers (-1: the file's own pick / no subtitles). Returns at once; watch state().
	void open(int gateIndex, double resumeSeconds, int audioStream = -1, int subtitleStream = -1);
	// Stops, tells the server where it stopped, and waits for the threads. Safe to call when idle.
	void close();

	State state() const;
	std::string error() const;
	std::string title() const;
	double position() const; // seconds: the audio clock
	double duration() const;
	double buffered() const; // seconds of decoded sound ahead of the position
	bool paused() const;
	void setPaused(bool paused);
	void seek(double seconds);
	void setVolume(float level, bool muted);
	std::vector<Chapter> chapters() const;

	// Another sound track, from the current position (the file is read on from here with the new one).
	void setAudio(int streamIndex);
	int audioStream() const;
	// Subtitles: the server turns a text track into SRT, which is fetched and shown by subtitleAt().
	void setSubtitle(int streamIndex); // -1 off
	int subtitleStream() const;
	std::string subtitleAt(double seconds) const; // the line to show now, "" for none
	std::vector<SubPicture> subtitlePictures(double seconds) const; // picture subtitles due now (Stage 37)
	std::string subtitleStatus() const;           // "", "Loading subtitles ...", "Subtitles unavailable"
	Info info() const;
	// A short message about something that did not work (a sound track with no decoder), shown once.
	std::string takeNote();

	// The newest picture due at the clock, if there is a newer one than the last returned.
	bool present(Frame *out);
	// Adds the film's sound to out (interleaved stereo s16, 48 kHz). Called from the audio thread.
	void mixInto(std::int16_t *out, int frames);
	// The same for the console's 7.1 output (eight s16 channels to a frame: FL FR FC LFE BL BR SL SR).
	void mixInto8(std::int16_t *out, int frames);
	// 2, or 8 when this film's sound is 5.1 / 7.1 and goes out as 7.1: what the output must be opened with.
	int outputChannels() const;
	// false: mix every film down to stereo (a file /data/jf-stereo does the same at start-up)
	void setSurround(bool on);
	// true: HDR10 pictures are delivered as 16-bit PQ (Frame::hdr) instead of being tone-mapped to SDR. Only for a display
	// that is in HDR10 mode; pictures converted before the change stay as they were.
	void setHdrOutput(bool on);
	bool hdrOutput() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace jfp
