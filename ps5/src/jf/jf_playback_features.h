#pragma once

#include <string>

struct json_object;

namespace jfp
{

struct Segment
{
	double start{ 0.0 };
	double end{ 0.0 };
};

struct TrickplayInfo
{
	int width{ 0 };
	int height{ 0 };
	int tileWidth{ 0 };
	int tileHeight{ 0 };
	int thumbnailCount{ 0 };
	double interval{ 0.0 };
	std::string mediaSourceId;

	bool valid() const;
};

struct ThumbnailLocation
{
	int index{ 0 };
	int sheet{ 0 };
	int x{ 0 };
	int y{ 0 };
};

bool parseMediaSegment(json_object *response, const char *type, Segment *out);
bool parseTrickplayInfo(json_object *item, const std::string &mediaSourceId, TrickplayInfo *out);
bool parseTrickplayResponse(json_object *response, const std::string &itemId,
	const std::string &mediaSourceId, TrickplayInfo *out);
bool mapThumbnail(const TrickplayInfo &info, double seconds, ThumbnailLocation *out);
double clampSkipTarget(double segmentEnd, double duration);
bool segmentEndsAtDuration(double segmentEnd, double duration);

inline bool skipPromptPressed(bool visible, bool panelOpen, bool hidingControls, bool triangle, bool remoteOk)
{
	return visible && !panelOpen && !hidingControls && (triangle || remoteOk);
}

struct PlaybackControlWake
{
	bool clearManualHide;
	bool wake;
};

inline PlaybackControlWake playbackControlWake(bool skipHandled, bool swallowed, bool inputActivity)
{
	return { !skipHandled && swallowed, !skipHandled && inputActivity };
}

float skipPromptTop(float controlsTop, float controlsVisibility);
float subtitleLiftForPrompt(float left, float top, float right, float bottom,
	float promptLeft, float promptTop, float promptRight, float promptBottom);

template <typename MarkWatched, typename RequestNext>
bool finishOutro(bool episode, bool autoplay, MarkWatched markWatched, RequestNext requestNext)
{
	if (!markWatched() || !episode || !autoplay) {
		return false;
	}
	requestNext();
	return true;
}

class SegmentPrompt
{
public:
	void reset();
	void setSegment(const Segment &segment);
	bool visible(double position, double duration);
	void observeSeek(double target, double duration);
	void dismiss();
	double skipTarget(double duration) const;

private:
	Segment segment_{};
	bool hasSegment_{ false };
	bool dismissed_{ false };
};

class IntroPromptTimer
{
public:
	static constexpr double kSeconds = 8.0;
	void reset();
	void update(bool eligible, bool playing, double seconds);
	bool visible(bool controlsVisible) const;
	bool expired() const;
	double progress() const;

private:
	bool active_{ false };
	double elapsed_{ 0.0 };
};

inline bool wakeIntroControls(bool introEligible, bool timerExpired, bool controlsHidden, bool confirm)
{
	return introEligible && timerExpired && controlsHidden && confirm;
}

class SeekPreview
{
public:
	void reset();
	void show(double target);
	void ready();
	void tick(double seconds);
	bool visible() const;
	double target() const;

private:
	double target_{ 0.0 };
	double remaining_{ 0.0 };
	bool waiting_{ false };
};

} // namespace jfp
