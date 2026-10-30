// The profile menu: opened from the profile picture at the end of the tabs.
// Settings (Playback, Seerr, About), Switch server, Sign out. Needs kit.hpp and jf_gate.h before it.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace jfprofile {

inline std::string readText(const char *path)
{
	std::string out;
	if (FILE *f = std::fopen(path, "rb")) {
		char buf[160];
		const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
		out.assign(buf, n);
		std::fclose(f);
	}
	while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) {
		out.pop_back();
	}
	return out;
}

inline void writeText(const char *path, const std::string &text)
{
	if (FILE *f = std::fopen(path, "wb")) {
		std::fwrite(text.data(), 1, text.size(), f);
		std::fclose(f);
	}
}

inline bool fileExists(const char *path)
{
	if (FILE *f = std::fopen(path, "rb")) {
		std::fclose(f);
		return true;
	}
	return false;
}

// The program's version: 1.<the stage of the last update>
inline const char *version()
{
#ifdef JF_APP_VERSION
	return JF_APP_VERSION;
#else
	return "dev";
#endif
}

// Seerr on or off (the Discover tab). Not chosen yet: on when Seerr has been set up before
inline bool &seerrFlag()
{
	static bool value = false;
	static bool loaded = false;
	if (!loaded) {
		loaded = true;
		const std::string choice = readText("/data/jellyzoom-seerr-on");
		value = choice.empty() ? true : choice[0] == '1';
	}
	return value;
}
inline bool seerrEnabled() { return seerrFlag(); }
inline void setSeerrEnabled(bool on)
{
	seerrFlag() = on;
	writeText("/data/jellyzoom-seerr-on", on ? "1" : "0");
}

struct Language {
	const char *name;
	const char *code; // ISO 639-2, as files tag their tracks ("" = the file's own pick)
};
inline const Language *languages(int *count)
{
	static const Language list[] = { { "Auto", "" }, { "English", "eng" }, { "French", "fre" }, { "German", "ger" },
		{ "Spanish", "spa" }, { "Italian", "ita" }, { "Portuguese", "por" }, { "Dutch", "dut" }, { "Japanese", "jpn" },
		{ "Korean", "kor" }, { "Chinese", "chi" }, { "Russian", "rus" }, { "Polish", "pol" }, { "Swedish", "swe" } };
	*count = static_cast<int>(sizeof(list) / sizeof(list[0]));
	return list;
}
inline int languageIndex(const std::string &code)
{
	int n = 0;
	const Language *l = languages(&n);
	for (int i = 0; i < n; i++) {
		if (code == l[i].code) {
			return i;
		}
	}
	return 0;
}

class Menu
{
  public:
	enum Result { none = 0, signOut = 1, switchServer = 2 };

	bool is_open() const { return open_ || fade_ > 0.01f; }

	void open()
	{
		open_ = true;
		page_ = 0;
		sel_ = 0;
		char buf[128];
		jf_gate_auth_text(2, buf, sizeof(buf));
		name_ = buf;
		jf_gate_auth_text(1, buf, sizeof(buf));
		server_ = buf;
		load();
	}

	// Returns a Result when the person chose something that changes the account
	template <class Feedback>
	int update(const hui::InputFrame &in, float dt, Feedback &fb)
	{
		clock_ += dt;
		fade_ += ((open_ ? 1.0f : 0.0f) - fade_) * std::min(1.0f, dt * 14.0f);
		if (!open_) {
			return none;
		}
		if (page_ == 1) {
			settings(in, fb);
			return none;
		}
		const int count = 4;
		if (in.is_pressed(hui::Action::back)) {
			fb.play(hui::audio::Cue::back);
			open_ = false;
			return none;
		}
		if (in.nav == hui::Direction::up) {
			sel_ = (sel_ + count - 1) % count;
			fb.play(hui::audio::Cue::focus);
		} else if (in.nav == hui::Direction::down) {
			sel_ = (sel_ + 1) % count;
			fb.play(hui::audio::Cue::focus);
		}
		if (!in.is_pressed(hui::Action::confirm)) {
			return none;
		}
		fb.play(hui::audio::Cue::open);
		switch (sel_) {
		case 0:
			page_ = 1;
			cat_ = 0;
			row_ = 0;
			inRows_ = false;
			load();
			return none;
		case 1:
			open_ = false;
			return switchServer;
		case 2:
			open_ = false;
			return signOut;
		default:
			open_ = false;
			return none;
		}
	}

	void draw(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts) const
	{
		using hui::gfx::Align;
		using hui::gfx::Color;
		using hui::gfx::Rect;
		if (fade_ < 0.01f) {
			return;
		}
		list.push_opacity(std::min(1.0f, fade_ * 1.4f));
		list.rounded_rect(Rect{ 0, 0, 1920, 1080 }, 0.0f, Color{ 0.01f, 0.012f, 0.03f, 0.72f });
		const Color white{ 1, 1, 1, 1 };
		if (page_ == 1) {
			drawSettings(list, fonts);
			list.pop_opacity();
			return;
		}
		const Rect panel{ 560.0f, 200.0f + (1.0f - fade_) * 40.0f, 800.0f, 660.0f };
		list.shadow(Rect{ panel.x, panel.y + 18.0f, panel.w, panel.h }, 34.0f, 54.0f, Color{ 0, 0, 0, 0.55f });
		list.rounded_rect(panel, 34.0f, Color{ 0.035f, 0.04f, 0.075f, 0.97f });
		list.bordered_rect(panel, 34.0f, Color{ 0, 0, 0, 0 }, 1.5f, Color{ 1, 1, 1, 0.2f });
		hui::ui::text(list, fonts.display, name_.empty() ? "Profile" : name_.c_str(), panel.x + 48.0f, panel.y + 86.0f, 52, white);
		if (!server_.empty()) {
			hui::ui::text(list, fonts.regular, server_.c_str(), panel.x + 48.0f, panel.y + 128.0f, 26, white.with_alpha(0.55f));
		}
		const char *const labels[4] = { "Settings", "Switch server", "Sign out", "Close" };
		float y = panel.y + 170.0f;
		for (int i = 0; i < 4; i++) {
			const Rect box{ panel.x + 40.0f, y, panel.w - 80.0f, 84.0f };
			const bool on = i == sel_;
			pill(list, box, on, Color{ 0.04f, 0.05f, 0.09f, 1.0f });
			hui::ui::text(list, fonts.semibold, labels[i], box.x + 36.0f, box.y + 55.0f, 34,
				on ? Color{ 0.04f, 0.05f, 0.09f, 1.0f } : Color{ 1, 1, 1, 0.85f });
			if (i == 0) {
				hui::ui::text(list, fonts.regular, ">", box.x + box.w - 36.0f, box.y + 54.0f, 30,
					on ? Color{ 0.04f, 0.05f, 0.09f, 0.7f } : Color{ 1, 1, 1, 0.6f }, Align::right);
			}
			y += 98.0f;
		}
		list.pop_opacity();
	}

  private:
	struct Row {
		std::string label, value, hint;
		int kind{ 0 }; // 0 shows a fact, 1 changes with left / right (or cross), 2 does something on cross
	};

	bool open_{ false };
	float fade_{ 0.0f }, clock_{ 0.0f };
	int page_{ 0 }, sel_{ 0 };
	int cat_{ 0 }, row_{ 0 };
	bool inRows_{ false };
	// playback
	int audio_{ 0 }, delay_{ 0 }, langAudio_{ 0 }, subMode_{ 0 }, langSub_{ 0 }, subSize_{ 1 }, hdr_{ 0 };
	bool autoNext_{ true }, seerr_{ false };
	std::string name_, server_, seerrAddress_;
	bool seerrSession_{ false };

	void load()
	{
		const std::string mode = readText("/data/jellyzoom-audio-mode");
		audio_ = mode.rfind("pcm", 0) == 0 ? 2 : mode.rfind("bit", 0) == 0 ? 1 : 0;
		delay_ = std::max(0, std::min(500, std::atoi(readText("/data/jellyzoom-audio-delay").c_str())));
		langAudio_ = languageIndex(readText("/data/jellyzoom-lang-audio"));
		langSub_ = languageIndex(readText("/data/jellyzoom-lang-sub"));
		const std::string sm = readText("/data/jellyzoom-sub-mode");
		subMode_ = sm == "on" ? 2 : sm == "auto" ? 1 : 0;
		const std::string ss = readText("/data/jellyzoom-sub-size");
		subSize_ = ss == "s" ? 0 : ss == "l" ? 2 : 1;
		autoNext_ = !fileExists("/data/jellyzoom-no-autoplay");
		const int gain = std::atoi(readText("/data/jellyzoom-hdr-gain").c_str());
		hdr_ = gain >= 100 ? std::min(20, (gain - 100 + 2) / 5) : 0;
		seerr_ = seerrEnabled();
		seerrAddress_ = readText("/data/jellyzoom-seerr");
		seerrSession_ = !readText("/data/jellyzoom-seerr-cookie").empty();
	}

	static int wrap(int value, int count) { return ((value % count) + count) % count; }

	std::vector<Row> rows(int cat) const
	{
		static const char *const modes[3] = { "Auto", "Bitstream", "PCM" };
		static const char *const subModes[3] = { "Off", "Auto", "On" };
		static const char *const sizes[3] = { "Small", "Medium", "Large" };
		int n = 0;
		const Language *lang = languages(&n);
		std::vector<Row> out;
		if (cat == 0) {
			out.push_back({ "Sound output", modes[audio_], "Auto sends Dolby and DTS to the soundbar when it lists the format.", 1 });
			out.push_back({ "Soundbar delay", std::to_string(delay_) + " ms", "How late the soundbar plays sound. Raise it until voices match lips.", 1 });
			out.push_back({ "Audio language", lang[langAudio_].name, "The sound track a film starts with. Auto uses the file's own pick.", 1 });
			out.push_back({ "Subtitles", subModes[subMode_],
				"Off shows none. Auto shows them when the sound is not in your subtitle language. On always shows them.", 1 });
			out.push_back({ "Subtitle language", lang[langSub_].name, "The language subtitles are picked in when Subtitles is Auto or On.", 1 });
			out.push_back({ "Subtitle size", sizes[subSize_], "How big the words are. On a wide film they sit in the black bar.", 1 });
			out.push_back({ "Next episode automatically", autoNext_ ? "On" : "Off", "An episode that ends goes on to the next one.", 1 });
			out.push_back({ "HDR brightness", hdr_ == 0 ? std::string("As mastered") : "+" + std::to_string(hdr_ * 5) + "%",
				"Where HDR films start. L1 / R1 change it while a film plays.", 1 });
		} else if (cat == 1) {
			out.push_back({ "Seerr (Discover tab)", seerr_ ? "On" : "Off", "On adds a Discover tab for browsing and requesting through Seerr.", 1 });
			out.push_back({ "Address", seerrAddress_.empty() ? std::string("Not set") : seerrAddress_,
				"Press Cross to type a new address; you will be asked for your password after it.", 2 });
			out.push_back({ "Account", seerrSession_ ? "Signed in" : "Not signed in",
				"Seerr uses your Jellyfin account. Press Cross to sign in again with your password.", 2 });
			out.push_back({ "Disconnect", "", "Forgets the Seerr sign-in on this console.", 2 });
		}
		return out;
	}

	template <class Feedback>
	void settings(const hui::InputFrame &in, Feedback &fb)
	{
		if (in.is_pressed(hui::Action::back)) {
			fb.play(hui::audio::Cue::back);
			if (inRows_) {
				inRows_ = false;
			} else {
				page_ = 0;
				sel_ = 0;
			}
			return;
		}
		const int step = in.nav == hui::Direction::right ? 1 : in.nav == hui::Direction::left ? -1 : 0;
		const bool press = in.is_pressed(hui::Action::confirm);
		if (!inRows_) {
			if (in.nav == hui::Direction::up) {
				cat_ = wrap(cat_ - 1, 3);
				fb.play(hui::audio::Cue::focus);
			} else if (in.nav == hui::Direction::down) {
				cat_ = wrap(cat_ + 1, 3);
				fb.play(hui::audio::Cue::focus);
			} else if ((press || step > 0) && cat_ < 2) {
				inRows_ = true;
				row_ = 0;
				fb.play(hui::audio::Cue::open);
			}
			return;
		}
		const std::vector<Row> list = rows(cat_);
		const int count = static_cast<int>(list.size());
		if (in.nav == hui::Direction::up) {
			row_ = wrap(row_ - 1, count);
			fb.play(hui::audio::Cue::focus);
			return;
		}
		if (in.nav == hui::Direction::down) {
			row_ = wrap(row_ + 1, count);
			fb.play(hui::audio::Cue::focus);
			return;
		}
		const Row &r = list[static_cast<size_t>(row_)];
		if (step < 0 && r.kind != 1) {
			inRows_ = false;
			fb.play(hui::audio::Cue::back);
			return;
		}
		if (!press && step == 0) {
			return;
		}
		const int by = step != 0 ? step : 1;
		int n = 0;
		const Language *lang = languages(&n);
		if (cat_ == 0) {
			switch (row_) {
			case 0:
				audio_ = wrap(audio_ + by, 3);
				writeText("/data/jellyzoom-audio-mode", audio_ == 0 ? "auto" : audio_ == 1 ? "bitstream" : "pcm");
				break;
			case 1:
				if (step == 0) {
					return;
				}
				delay_ = std::max(0, std::min(500, delay_ + 10 * step));
				writeText("/data/jellyzoom-audio-delay", std::to_string(delay_));
				fb.play(hui::audio::Cue::tick);
				return;
			case 2:
				langAudio_ = wrap(langAudio_ + by, n);
				writeText("/data/jellyzoom-lang-audio", lang[langAudio_].code);
				break;
			case 3:
				subMode_ = wrap(subMode_ + by, 3);
				writeText("/data/jellyzoom-sub-mode", subMode_ == 0 ? "off" : subMode_ == 1 ? "auto" : "on");
				break;
			case 4:
				langSub_ = wrap(langSub_ + by, n);
				writeText("/data/jellyzoom-lang-sub", lang[langSub_].code);
				break;
			case 5:
				subSize_ = wrap(subSize_ + by, 3);
				writeText("/data/jellyzoom-sub-size", subSize_ == 0 ? "s" : subSize_ == 1 ? "m" : "l");
				break;
			case 6:
				autoNext_ = !autoNext_;
				if (autoNext_) {
					std::remove("/data/jellyzoom-no-autoplay");
				} else {
					writeText("/data/jellyzoom-no-autoplay", "1");
				}
				break;
			case 7: {
				hdr_ = step != 0 ? std::max(0, std::min(20, hdr_ + step)) : (hdr_ + 1) % 21;
				writeText("/data/jellyzoom-hdr-gain", std::to_string(100 + 5 * hdr_));
				fb.play(hui::audio::Cue::tick);
				return;
			}
			}
			fb.play(hui::audio::Cue::tab);
			return;
		}
		// Seerr
		switch (row_) {
		case 0:
			seerr_ = !seerr_;
			setSeerrEnabled(seerr_);
			fb.play(hui::audio::Cue::tab);
			break;
		case 1:
		case 2:
			if (!press) {
				return;
			}
			if (!seerr_) {
				seerr_ = true;
				setSeerrEnabled(true);
			}
			::jf::discoverEnabled() = true;
			::jf::discoverGo() = row_ == 1 ? 1 : 2; // the Discover tab opens at the address or the password
			open_ = false;
			fb.play(hui::audio::Cue::open);
			break;
		case 3:
			if (!press) {
				return;
			}
			writeText("/data/jellyzoom-seerr-cookie", "");
			::jf::discoverGo() = 3;
			seerrSession_ = false;
			fb.play(hui::audio::Cue::back);
			break;
		}
	}

	static void pill(hui::gfx::DrawList &list, const hui::gfx::Rect &box, bool on, const hui::gfx::Color &)
	{
		using hui::gfx::Color;
		const float round = box.h * 0.5f;
		if (on) {
			list.glow(box, round, 20.0f, Color{ 0.67f, 0.36f, 0.76f, 0.30f });
			list.rounded_rect(box, round, Color{ 1, 1, 1, 0.96f });
		} else {
			list.rounded_rect(box, round, Color{ 1, 1, 1, 0.08f });
			list.gradient_rect(box, round, Color{ 1, 1, 1, 0.10f }, Color{ 1, 1, 1, 0.0f });
			list.bordered_rect(box, round, Color{ 0, 0, 0, 0 }, 1.5f, Color{ 1, 1, 1, 0.16f });
		}
	}

	void drawSettings(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts) const
	{
		using hui::gfx::Align;
		using hui::gfx::Color;
		using hui::gfx::Rect;
		const Color white{ 1, 1, 1, 1 };
		const Color ink{ 0.04f, 0.05f, 0.09f, 1.0f };
		const Rect panel{ 260.0f, 90.0f + (1.0f - fade_) * 40.0f, 1400.0f, 900.0f };
		list.shadow(Rect{ panel.x, panel.y + 22.0f, panel.w, panel.h }, 40.0f, 60.0f, Color{ 0, 0, 0, 0.5f });
		list.rounded_rect(panel, 40.0f, Color{ 0.035f, 0.04f, 0.075f, 0.97f });
		list.bordered_rect(panel, 40.0f, Color{ 0, 0, 0, 0 }, 1.5f, Color{ 1, 1, 1, 0.2f });
		hui::ui::text(list, fonts.display, "Settings", panel.x + 56.0f, panel.y + 92.0f, 56, white);
		// the categories
		static const char *const cats[3] = { "Playback", "Seerr", "About" };
		float cy = panel.y + 150.0f;
		for (int i = 0; i < 3; i++) {
			const Rect box{ panel.x + 40.0f, cy, 330.0f, 76.0f };
			const bool here = i == cat_;
			const bool focused = here && !inRows_;
			if (here && inRows_) {
				// the category the rows belong to, with the pad elsewhere
				list.rounded_rect(box, box.h * 0.5f, Color{ 1, 1, 1, 0.18f });
				list.bordered_rect(box, box.h * 0.5f, Color{ 0, 0, 0, 0 }, 1.5f, Color{ 1, 1, 1, 0.4f });
			} else {
				pill(list, box, focused, ink);
			}
			hui::ui::text(list, fonts.semibold, cats[i], box.x + 36.0f, box.y + 50.0f, 32,
				focused ? ink : Color{ 1, 1, 1, here ? 1.0f : 0.8f });
			cy += 92.0f;
		}
		const float rx = panel.x + 420.0f, rw = panel.w - 420.0f - 40.0f;
		if (cat_ == 2) {
			drawAbout(list, fonts, rx, panel.y + 130.0f, rw);
			hui::ui::text(list, fonts.regular, "Circle: back", panel.x + panel.w * 0.5f, panel.y + panel.h - 34.0f, 22,
				white.with_alpha(0.45f), Align::center);
			return;
		}
		const std::vector<Row> list2 = rows(cat_);
		float y = panel.y + 120.0f;
		const float rowH = 72.0f, pitch = list2.size() > 6 ? 80.0f : 92.0f;
		for (int i = 0; i < static_cast<int>(list2.size()); i++) {
			const Row &r = list2[static_cast<size_t>(i)];
			const Rect box{ rx, y, rw, rowH };
			const bool on = inRows_ && i == row_;
			pill(list, box, on, ink);
			hui::ui::text(list, fonts.semibold, r.label.c_str(), box.x + 34.0f, box.y + 48.0f, 31, on ? ink : Color{ 1, 1, 1, 0.88f });
			if (!r.value.empty()) {
				const std::string value(fonts.regular.font->fit(r.value.c_str(), 29, rw * 0.5f));
				const std::string shown = r.kind == 1 && on ? "<  " + value + "  >" : value;
				hui::ui::text(list, fonts.regular, shown.c_str(), box.x + box.w - 34.0f, box.y + 47.0f, 29,
					on ? Color{ 0.04f, 0.05f, 0.09f, 0.8f } : Color{ 1, 1, 1, 0.62f }, Align::right);
			}
			y += pitch;
		}
		const char *hint = inRows_ ? list2[static_cast<size_t>(row_)].hint.c_str() : "Cross or Right: open    Circle: back";
		hui::ui::text(list, fonts.regular, hint, panel.x + panel.w * 0.5f + 190.0f, panel.y + panel.h - 34.0f, 22,
			white.with_alpha(0.5f), Align::center);
	}

	void drawAbout(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, float x, float y, float w) const
	{
		using hui::gfx::Align;
		using hui::gfx::Color;
		const Color white{ 1, 1, 1, 1 };
		hui::ui::text(list, fonts.display, "Jellyzoom", x, y + 40.0f, 60, white);
		char line[160];
		std::snprintf(line, sizeof(line), "Version %s    built %s %s", version(), __DATE__, __TIME__);
		hui::ui::text(list, fonts.regular, line, x, y + 90.0f, 28, white.with_alpha(0.7f));
		hui::ui::text(list, fonts.semibold, "SOFTWARE UPDATES", x, y + 160.0f, 16, white.with_alpha(0.5f), Align::left, 3.0f);
		hui::ui::text(list, fonts.regular, "Updates arrive as a new build from your computer (build, then deploy).", x, y + 196.0f, 26,
			white.with_alpha(0.85f));
		hui::ui::text(list, fonts.regular, "Close the app on the console first, then deploy.", x, y + 232.0f, 26, white.with_alpha(0.6f));
		hui::ui::text(list, fonts.semibold, "CREDITS", x, y + 304.0f, 16, white.with_alpha(0.5f), Align::left, 3.0f);
		static const char *const credits[] = {
			"Jellyfin: the media server this app talks to (jellyfin.org)",
			"Seerr: requests and discovery (the Discover tab)",
			"FFmpeg and dav1d: video and sound decoding",
			"Vulkan and the PS5 payload SDK: drawing and running on the console",
			"hui: the interface kit behind the wheel, the buttons and the glass",
			"stb_image, json-c: pictures and data",
		};
		float cy = y + 340.0f;
		for (const char *c : credits) {
			hui::ui::text(list, fonts.regular, c, x, cy, 24, white.with_alpha(0.8f));
			cy += 40.0f;
		}
		(void)w;
	}
};

} // namespace jfprofile
