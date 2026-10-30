#include "../src/jf/jf_playback_features.h"
#include "../src/jf/jf_player.h"
#include "../src/jf/jf_navigation.h"
#include "../src/jf/jf_present_metrics.h"

#include <cassert>
#include <cmath>
#include <limits>
#include <string>

#include <json-c/json.h>

int main()
{
	assert(!jfp::playbackReady(false, false));
	assert(!jfp::playbackReady(true, false));
	assert(!jfp::playbackReady(false, true));
	assert(jfp::playbackReady(true, true));
	for (bool overlay : { false, true }) {
		for (bool details : { false, true }) {
			for (bool browsing : { false, true }) {
				for (bool transitioning : { false, true }) {
					for (bool previous : { false, true }) {
						for (bool next : { false, true }) {
							const int expected = overlay || details || browsing || transitioning || previous == next
								? 0 : next ? 1 : -1;
							assert(jf::sectionShortcut(overlay, details, browsing, transitioning, previous, next) == expected);
						}
						using jf::SectionDestination;
						assert(jf::sectionStep(4, 3, false, false, false, 1).destination == SectionDestination::profile);
						assert(jf::sectionStep(4, 3, false, false, true, -1).index == 3);
						assert(jf::sectionStep(4, 3, true, false, false, 1).destination == SectionDestination::discover);
						assert(jf::sectionStep(4, 0, true, true, false, 1).destination == SectionDestination::profile);
						assert(jf::sectionStep(4, 0, true, true, false, -1).index == 3);
						assert(jf::sectionStep(4, 0, true, true, true, -1).destination == SectionDestination::discover);
						assert(jf::sectionStep(4, 3, true, false, true, 1).destination == SectionDestination::none);
						assert(jf::sectionStep(4, 1, false, false, false, 1).index == 2);
					}
				}
			}
		}
	}
	for (bool visible : { false, true }) {
		for (bool panel : { false, true }) {
			for (bool hiding : { false, true }) {
				for (bool triangle : { false, true }) {
					for (bool remoteOk : { false, true }) {
						assert(jfp::skipPromptPressed(visible, panel, hiding, triangle, remoteOk) ==
							(visible && !panel && !hiding && (triangle || remoteOk)));
					}
					for (bool skip : { false, true }) {
						for (bool swallowed : { false, true }) {
							for (bool activity : { false, true }) {
								const jfp::PlaybackControlWake wake = jfp::playbackControlWake(skip, swallowed, activity);
								assert(wake.clearManualHide == (!skip && swallowed));
								assert(wake.wake == (!skip && activity));
							}
						}
					}
					assert(jfp::skipPromptTop(830.0f, 0.0f) == 830.0f);
					assert(jfp::skipPromptTop(830.0f, 1.0f) == 724.0f);
					assert(jfp::skipPromptTop(830.0f, 0.5f) == 777.0f);
					assert(jfp::skipPromptTop(830.0f, -1.0f) == 830.0f);
					assert(jfp::skipPromptTop(830.0f, 2.0f) == 724.0f);
					assert(jfp::subtitleLiftForPrompt(700, 700, 1500, 760, 1390, 724, 1810, 806) == 48.0f);
					assert(jfp::subtitleLiftForPrompt(700, 700, 1350, 760, 1390, 724, 1810, 806) == 0.0f);
					assert(jfp::subtitleLiftForPrompt(700, 620, 1500, 700, 1390, 724, 1810, 806) == 0.0f);
					assert(jfp::subtitleLiftForPrompt(700, 900, 1500, 960, 1390, 724, 1810, 806) == 0.0f);
				}
			}
		}
	}

	jfp::PresentMetrics metrics;
	jfp::PresentReport report;
	assert(!metrics.observe(0.0, true, 0, report));
	assert(!metrics.observe(16.0, false, 0, report));
	assert(!metrics.observe(46.0, true, 2, report));
	assert(metrics.observe(10000.0, false, 0, report));
	assert(report.seconds == 10.0 && report.calls == 4 && report.pictures == 2 && report.skipped == 2);
	assert(report.gapsOver25ms == 2 && report.gapsOver40ms == 1);
	assert(report.maxCallGapMs == 9954.0 && report.maxPictureGapMs == 46.0);
	assert(!metrics.observe(10016.0, true, 0, report));
	assert(metrics.observe(20000.0, true, 0, report));
	assert(report.calls == 2 && report.pictures == 2 && report.skipped == 0);
	assert(report.maxPictureGapMs == 9984.0);
	metrics.reset();
	assert(!metrics.observe(30000.0, true, 0, report));
	assert(metrics.observe(40000.0, false, 0, report));
	assert(report.calls == 2 && report.pictures == 1 && report.maxPictureGapMs == 0.0);

	for (bool episode : { false, true }) {
		for (bool autoplay : { false, true }) {
			for (bool marked : { false, true }) {
				std::string calls;
				const bool queued = jfp::finishOutro(episode, autoplay,
					[&] { calls += "mark"; return marked; },
					[&] { calls += "next"; });
				assert(queued == (episode && autoplay && marked));
				assert(calls == (queued ? "marknext" : "mark"));
			}
		}
	}

	jfp::SegmentPrompt prompt;
	prompt.setSegment({ 10.0, 30.0 });
	assert(!prompt.visible(9.99, 120.0));
	assert(prompt.visible(10.0, 120.0));
	assert(prompt.visible(29.99, 120.0));
	assert(!prompt.visible(30.0, 120.0));
	assert(prompt.visible(15.0, 120.0));
	assert(!prompt.visible(5.0, 120.0));
	assert(prompt.visible(10.0, 120.0));

	prompt.dismiss();
	assert(!prompt.visible(15.0, 120.0));
	prompt.observeSeek(15.0, 120.0);
	assert(prompt.visible(15.0, 120.0));
	prompt.dismiss();
	assert(!prompt.visible(30.0, 120.0));
	assert(prompt.visible(15.0, 120.0));
	prompt.dismiss();
	prompt.observeSeek(5.0, 120.0);
	assert(!prompt.visible(5.0, 120.0));
	assert(prompt.visible(10.0, 120.0));
	assert(!prompt.visible(std::numeric_limits<double>::quiet_NaN(), 120.0));
	assert(!prompt.visible(15.0, 0.0));
	assert(prompt.skipTarget(25.0) == 23.0);
	prompt.reset();
	assert(!prompt.visible(15.0, 120.0));

	jfp::IntroPromptTimer timer;
	assert(!timer.visible(false) && !timer.visible(true) && !timer.expired());
	timer.update(true, true, 0.1);
	assert(timer.visible(false) && timer.progress() == 0.0);
	timer.update(true, true, 2.0);
	assert(timer.progress() == 0.25 && timer.visible(false));
	timer.update(true, false, 20.0);
	assert(timer.progress() == 0.25);
	timer.update(true, true, std::numeric_limits<double>::quiet_NaN());
	assert(timer.progress() == 0.25);
	timer.update(true, true, 5.999);
	assert(!timer.expired() && timer.visible(false));
	timer.update(true, true, 0.001);
	assert(timer.expired() && timer.progress() == 1.0);
	assert(!timer.visible(false) && timer.visible(true));
	timer.update(false, true, 1.0);
	assert(!timer.visible(true) && !timer.expired());
	timer.update(true, true, 1.0);
	assert(timer.visible(false) && timer.progress() == 0.0);
	timer.update(true, true, 8.0);
	timer.reset(); // seeking into the intro re-arms the prompt
	timer.update(true, true, 0.0);
	assert(timer.visible(false) && !timer.expired());
	for (bool eligible : { false, true }) {
		for (bool expired : { false, true }) {
			for (bool hidden : { false, true }) {
				for (bool confirm : { false, true }) {
					assert(jfp::wakeIntroControls(eligible, expired, hidden, confirm) ==
						(eligible && expired && hidden && confirm));
				}
			}
		}
	}
	jfp::SegmentPrompt liveIntro;
	liveIntro.setSegment({ 10.0, 60.0 });
	jfp::IntroPromptTimer liveTimer;
	liveTimer.update(liveIntro.visible(10.0, 120.0), true, 0.0);
	assert(jfp::skipPromptPressed(liveTimer.visible(false), false, false, false, true));
	liveTimer.update(liveIntro.visible(18.0, 120.0), true, 8.0);
	assert(!jfp::skipPromptPressed(liveTimer.visible(false), false, false, false, true));
	assert(jfp::wakeIntroControls(true, liveTimer.expired(), true, true));
	assert(jfp::skipPromptPressed(liveTimer.visible(true), false, false, false, true));
	assert(jfp::skipPromptPressed(liveTimer.visible(true), false, false, true, false));
	liveIntro.dismiss();
	liveTimer.update(liveIntro.visible(18.0, 120.0), true, 0.1);
	assert(!liveTimer.visible(true));
	liveIntro.observeSeek(12.0, 120.0);
	liveTimer.reset();
	liveTimer.update(liveIntro.visible(12.0, 120.0), true, 0.0);
	assert(liveTimer.visible(false));

	json_object *segments = json_tokener_parse(R"json({"Items":[
		{"Type":"Intro","StartTicks":100000000,"EndTicks":300000000},
		{"Type":"Outro","StartTicks":900000000,"EndTicks":1200000000},
		{"Type":"Outro","StartTicks":1200000000,"EndTicks":1200000000}
	]})json");
	assert(segments);
	jfp::Segment segment;
	assert(jfp::parseMediaSegment(segments, "Intro", &segment));
	assert(segment.start == 10.0 && segment.end == 30.0);
	assert(jfp::parseMediaSegment(segments, "Outro", &segment));
	assert(segment.start == 90.0 && segment.end == 120.0);
	assert(!jfp::parseMediaSegment(segments, "Recap", &segment));
	assert(jfp::segmentEndsAtDuration(120.0, 120.0));
	assert(jfp::segmentEndsAtDuration(119.0, 120.0));
	assert(!jfp::segmentEndsAtDuration(118.99, 120.0));
	assert(!jfp::segmentEndsAtDuration(120.0, 0.0));
	assert(!jfp::segmentEndsAtDuration(std::numeric_limits<double>::quiet_NaN(), 120.0));
	json_object_put(segments);

	jfp::SeekPreview preview;
	assert(!preview.visible());
	preview.show(42.0);
	assert(preview.visible() && preview.target() == 42.0);
	preview.tick(4.5);
	assert(preview.visible());
	preview.ready();
	preview.tick(1.0);
	preview.ready();
	assert(preview.visible());
	preview.tick(1.0);
	assert(!preview.visible());
	preview.show(55.0);
	preview.tick(5.0);
	assert(!preview.visible());
	preview.ready();
	assert(!preview.visible());
	preview.show(5.0);
	preview.reset();
	assert(!preview.visible());

	json_object *response = json_tokener_parse(R"json({
		"Items": [
			{"Id":"other","Trickplay":{"other-source":{"320":{"Width":320,"Height":180,"TileWidth":10,"TileHeight":10,"ThumbnailCount":200,"Interval":10000}}}},
			{"Id":"episode","Trickplay":{"source":{
				"160":{"Width":160,"Height":90,"TileWidth":10,"TileHeight":10,"ThumbnailCount":201,"Interval":10000},
				"320":{"Width":320,"Height":180,"TileWidth":10,"TileHeight":10,"ThumbnailCount":201,"Interval":10000}
			}}}
		]
	})json");
	assert(response);
	jfp::TrickplayInfo info;
	assert(jfp::parseTrickplayResponse(response, "episode", "source", &info));
	assert(info.width == 320 && info.height == 180 && info.mediaSourceId == "source");
	assert(info.thumbnailCount == 201 && info.interval == 10.0);
	assert(!jfp::parseTrickplayResponse(response, "missing", "source", &info));

	jfp::ThumbnailLocation location;
	assert(jfp::mapThumbnail(info, 0.0, &location));
	assert(location.sheet == 0 && location.x == 0 && location.y == 0);
	assert(jfp::mapThumbnail(info, 999.0, &location));
	assert(location.index == 99 && location.sheet == 0 && location.x == 9 && location.y == 9);
	assert(jfp::mapThumbnail(info, 1000.0, &location));
	assert(location.index == 100 && location.sheet == 1 && location.x == 0 && location.y == 0);
	assert(jfp::mapThumbnail(info, 9999.0, &location));
	assert(location.index == 200 && location.sheet == 2 && location.x == 0 && location.y == 0);
	assert(!jfp::mapThumbnail(info, std::numeric_limits<double>::infinity(), &location));
	json_object_put(response);

	response = json_tokener_parse(R"json({"Items":[{"Id":"episode","Trickplay":{}}]})json");
	assert(response);
	assert(!jfp::parseTrickplayResponse(response, "episode", "source", &info));
	json_object_put(response);
	return 0;
}
