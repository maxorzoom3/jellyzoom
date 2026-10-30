/*
 * Mortalfin — Jellyfin playback metadata and prompt behavior
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "jf_playback_features.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <utility>

#include <json-c/json.h>

namespace jfp
{
namespace
{
constexpr std::int64_t kTicksPerSecond = 10000000;
constexpr int kMaxMetadataDimension = 2048;
constexpr int kMaxTilesPerAxis = 32;
constexpr std::int64_t kMaxThumbnailCount = 100000;
constexpr double kMinIntervalMs = 100.0;
constexpr double kMaxIntervalMs = 86400000.0;
constexpr double kMaxSheetPixels = 6000000.0;

json_object *member(json_object *object, const char *name)
{
	json_object *value = nullptr;
	return object && json_object_object_get_ex(object, name, &value) ? value : nullptr;
}

bool integer(json_object *object, const char *name, std::int64_t *out)
{
	json_object *value = member(object, name);
	if (!value || !json_object_is_type(value, json_type_int)) {
		return false;
	}
	*out = json_object_get_int64(value);
	return true;
}

bool number(json_object *object, const char *name, double *out)
{
	json_object *value = member(object, name);
	if (!value || (!json_object_is_type(value, json_type_int) && !json_object_is_type(value, json_type_double))) {
		return false;
	}
	*out = json_object_get_double(value);
	return std::isfinite(*out);
}

bool trickplayInfo(json_object *info, const std::string &sourceId, TrickplayInfo *out)
{
	std::int64_t width = 0, height = 0, tileWidth = 0, tileHeight = 0, count = 0;
	double intervalMs = 0.0;
	if (!integer(info, "Width", &width) || !integer(info, "Height", &height) ||
		!integer(info, "TileWidth", &tileWidth) || !integer(info, "TileHeight", &tileHeight) ||
		!integer(info, "ThumbnailCount", &count) || !number(info, "Interval", &intervalMs) ||
		width < 1 || width > kMaxMetadataDimension || height < 1 || height > kMaxMetadataDimension ||
		tileWidth < 1 || tileWidth > kMaxTilesPerAxis || tileHeight < 1 || tileHeight > kMaxTilesPerAxis ||
		count < 1 || count > kMaxThumbnailCount || intervalMs < kMinIntervalMs || intervalMs > kMaxIntervalMs ||
		width * tileWidth > 8192 || height * tileHeight > 8192 ||
		static_cast<double>(width) * static_cast<double>(height) * static_cast<double>(tileWidth) *
			static_cast<double>(tileHeight) > kMaxSheetPixels) {
		return false;
	}

	out->width = static_cast<int>(width);
	out->height = static_cast<int>(height);
	out->tileWidth = static_cast<int>(tileWidth);
	out->tileHeight = static_cast<int>(tileHeight);
	out->thumbnailCount = static_cast<int>(count);
	out->interval = intervalMs / 1000.0;
	out->mediaSourceId = sourceId;
	return out->valid();
}
} // namespace

bool TrickplayInfo::valid() const
{
	return width > 0 && height > 0 && tileWidth > 0 && tileHeight > 0 && thumbnailCount > 0 &&
		std::isfinite(interval) && interval >= kMinIntervalMs / 1000.0 && interval <= kMaxIntervalMs / 1000.0 &&
		static_cast<std::int64_t>(width) * tileWidth <= 8192 &&
		static_cast<std::int64_t>(height) * tileHeight <= 8192 &&
		static_cast<double>(width) * static_cast<double>(height) * static_cast<double>(tileWidth) *
			static_cast<double>(tileHeight) <= kMaxSheetPixels;
}

bool parseMediaSegment(json_object *response, const char *segmentType, Segment *out)
{
	if (!response || !segmentType || !out) {
		return false;
	}
	json_object *items = member(response, "Items");
	if (!items || !json_object_is_type(items, json_type_array)) {
		return false;
	}

	bool found = false;
	Segment earliest{};
	const size_t count = json_object_array_length(items);
	for (size_t i = 0; i < count; ++i) {
		json_object *item = json_object_array_get_idx(items, i);
		json_object *type = member(item, "Type");
		if (!type || !json_object_is_type(type, json_type_string) ||
			std::string(json_object_get_string(type)) != segmentType) {
			continue;
		}
		std::int64_t startTicks = 0, endTicks = 0;
		if (!integer(item, "StartTicks", &startTicks) || !integer(item, "EndTicks", &endTicks) ||
			startTicks < 0 || endTicks <= startTicks) {
			continue;
		}
		const Segment candidate{
			static_cast<double>(startTicks) / static_cast<double>(kTicksPerSecond),
			static_cast<double>(endTicks) / static_cast<double>(kTicksPerSecond)
		};
		if (!std::isfinite(candidate.start) || !std::isfinite(candidate.end)) {
			continue;
		}
		if (!found || candidate.start < earliest.start) {
			earliest = candidate;
			found = true;
		}
	}
	if (found) {
		*out = earliest;
	}
	return found;
}

bool parseTrickplayInfo(json_object *item, const std::string &mediaSourceId, TrickplayInfo *out)
{
	if (!item || !out) {
		return false;
	}
	json_object *sources = member(item, "Trickplay");
	if (!sources || !json_object_is_type(sources, json_type_object)) {
		return false;
	}

	json_object *source = nullptr;
	std::string selectedSourceId = mediaSourceId;
	if (!mediaSourceId.empty()) {
		source = member(sources, mediaSourceId.c_str());
	}
	if (!source) {
		json_object_object_foreach(sources, key, value)
		{
			source = value;
			selectedSourceId = key;
			break;
		}
	}
	if (!source || !json_object_is_type(source, json_type_object)) {
		return false;
	}

	TrickplayInfo best{};
	bool found = false;
	int bestDistance = std::numeric_limits<int>::max();
	json_object_object_foreach(source, key, value)
	{
		(void)key;
		TrickplayInfo candidate{};
		if (!trickplayInfo(value, selectedSourceId, &candidate)) {
			continue;
		}
		const int distance = std::abs(candidate.width - 320);
		if (!found || distance < bestDistance) {
			best = std::move(candidate);
			bestDistance = distance;
			found = true;
		}
	}
	if (found) {
		*out = std::move(best);
	}
	return found;
}

bool parseTrickplayResponse(json_object *response, const std::string &itemId,
	const std::string &mediaSourceId, TrickplayInfo *out)
{
	json_object *items = member(response, "Items");
	if (!items || !json_object_is_type(items, json_type_array) || itemId.empty() || !out) {
		return false;
	}
	const size_t count = json_object_array_length(items);
	for (size_t i = 0; i < count; ++i) {
		json_object *item = json_object_array_get_idx(items, i);
		json_object *id = member(item, "Id");
		if (id && json_object_is_type(id, json_type_string) && itemId == json_object_get_string(id)) {
			return parseTrickplayInfo(item, mediaSourceId, out);
		}
	}
	return false;
}

bool mapThumbnail(const TrickplayInfo &info, double seconds, ThumbnailLocation *out)
{
	if (!out || !info.valid() || !std::isfinite(seconds)) {
		return false;
	}
	const double rawIndex = std::floor(std::max(0.0, seconds) / info.interval);
	const int index = !std::isfinite(rawIndex) || rawIndex >= info.thumbnailCount
		? info.thumbnailCount - 1
		: static_cast<int>(rawIndex);
	const int perSheet = info.tileWidth * info.tileHeight;
	const int inSheet = index % perSheet;
	out->index = index;
	out->sheet = index / perSheet;
	out->x = inSheet % info.tileWidth;
	out->y = inSheet / info.tileWidth;
	return true;
}

double clampSkipTarget(double segmentEnd, double duration)
{
	if (!std::isfinite(segmentEnd) || !std::isfinite(duration) || duration <= 0.0) {
		return 0.0;
	}
	return std::clamp(segmentEnd, 0.0, std::max(0.0, duration - 2.0));
}

bool segmentEndsAtDuration(double segmentEnd, double duration)
{
	return std::isfinite(segmentEnd) && std::isfinite(duration) && duration > 1.0 &&
		segmentEnd >= duration - 1.0;
}

void SegmentPrompt::reset()
{
	segment_ = {};
	hasSegment_ = false;
	dismissed_ = false;
}

void SegmentPrompt::setSegment(const Segment &segment)
{
	segment_ = segment;
	hasSegment_ = std::isfinite(segment.start) && std::isfinite(segment.end) &&
		segment.start >= 0.0 && segment.end > segment.start;
	dismissed_ = false;
}

bool SegmentPrompt::visible(double position, double duration)
{
	if (!hasSegment_ || !std::isfinite(position) || !std::isfinite(duration) || duration <= 0.0) {
		return false;
	}
	const double end = std::min(segment_.end, duration);
	if (segment_.start >= end || position < segment_.start || position >= end) {
		dismissed_ = false;
		return false;
	}
	return !dismissed_;
}

void SegmentPrompt::observeSeek(double target, double duration)
{
	if (hasSegment_ && std::isfinite(target) && std::isfinite(duration) && duration > 0.0 &&
		target < std::min(segment_.end, duration)) {
		dismissed_ = false;
	}
}

void SegmentPrompt::dismiss()
{
	dismissed_ = true;
}

double SegmentPrompt::skipTarget(double duration) const
{
	return hasSegment_ ? clampSkipTarget(segment_.end, duration) : 0.0;
}

void IntroPromptTimer::reset()
{
	active_ = false;
	elapsed_ = 0.0;
}

float skipPromptTop(float controlsTop, float controlsVisibility)
{
	const float low = 830.0f;
	const float aboveControls = std::min(low, controlsTop - 106.0f);
	const float shown = std::clamp(controlsVisibility, 0.0f, 1.0f);
	return low + (aboveControls - low) * shown;
}

float subtitleLiftForPrompt(float left, float top, float right, float bottom,
	float promptLeft, float promptTop, float promptRight, float promptBottom)
{
	constexpr float gap = 12.0f;
	if (right <= promptLeft - gap || left >= promptRight + gap ||
	    bottom <= promptTop - gap || top >= promptBottom + gap)
		return 0.0f;
	return std::min(std::max(0.0f, top), bottom - promptTop + gap);
}

void IntroPromptTimer::update(bool eligible, bool playing, double seconds)
{
	if (!eligible) {
		reset();
		return;
	}
	if (!active_) {
		active_ = true;
		elapsed_ = 0.0;
		return;
	}
	if (playing && std::isfinite(seconds) && seconds > 0.0)
		elapsed_ = std::min(kSeconds, elapsed_ + seconds);
}

bool IntroPromptTimer::visible(bool controlsVisible) const
{
	return active_ && (!expired() || controlsVisible);
}

bool IntroPromptTimer::expired() const
{
	return active_ && elapsed_ >= kSeconds;
}

double IntroPromptTimer::progress() const
{
	return active_ ? elapsed_ / kSeconds : 0.0;
}

void SeekPreview::reset()
{
	target_ = 0.0;
	remaining_ = 0.0;
	waiting_ = false;
}

void SeekPreview::show(double target)
{
	if (std::isfinite(target) && target >= 0.0) {
		target_ = target;
		remaining_ = 5.0;
		waiting_ = true;
	}
}

void SeekPreview::ready()
{
	if (waiting_ && remaining_ > 0.0) {
		waiting_ = false;
		remaining_ = 2.0;
	}
}

void SeekPreview::tick(double seconds)
{
	if (std::isfinite(seconds) && seconds > 0.0) {
		remaining_ = std::max(0.0, remaining_ - seconds);
	}
}

bool SeekPreview::visible() const
{
	return remaining_ > 0.0;
}

double SeekPreview::target() const
{
	return target_;
}

} // namespace jfp
