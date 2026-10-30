#pragma once

#include <algorithm>
#include <cstdint>

namespace jfp
{

struct PresentReport
{
	double seconds{ 0.0 };
	std::uint64_t calls{ 0 }, pictures{ 0 }, skipped{ 0 };
	std::uint64_t gapsOver25ms{ 0 }, gapsOver40ms{ 0 };
	double maxCallGapMs{ 0.0 }, maxPictureGapMs{ 0.0 };
};

class PresentMetrics
{
public:
	void reset()
	{
		*this = PresentMetrics();
	}

	bool observe(double nowMs, bool picture, std::uint64_t skipped, PresentReport &report)
	{
		if (!started_) {
			started_ = true;
			startMs_ = nowMs;
		} else {
			const double gap = nowMs - lastCallMs_;
			window_.maxCallGapMs = std::max(window_.maxCallGapMs, gap);
			window_.gapsOver25ms += gap > 25.0;
			window_.gapsOver40ms += gap > 40.0;
		}
		lastCallMs_ = nowMs;
		++window_.calls;
		window_.skipped += skipped;
		if (picture) {
			++window_.pictures;
			if (hadPicture_) {
				window_.maxPictureGapMs = std::max(window_.maxPictureGapMs, nowMs - lastPictureMs_);
			}
			hadPicture_ = true;
			lastPictureMs_ = nowMs;
		}
		if (nowMs - startMs_ < 10000.0) {
			return false;
		}
		window_.seconds = (nowMs - startMs_) / 1000.0;
		report = window_;
		window_ = PresentReport();
		startMs_ = nowMs;
		return true;
	}

private:
	bool started_{ false }, hadPicture_{ false };
	double startMs_{ 0.0 }, lastCallMs_{ 0.0 }, lastPictureMs_{ 0.0 };
	PresentReport window_;
};

} // namespace jfp
