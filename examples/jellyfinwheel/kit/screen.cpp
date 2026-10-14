// ps5-homebrew-ui - Design "Cover Wheel": a carousel of covers you can spin.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copied into Jellyfin Wheel by PS5_VulkanTemplate's new-title.py from PS5_VKHomebrewUI
// 2188642 (src/concepts/carousel.cpp), in a namespace of its own: change it at
// will. Its program (../jellyfinwheel.cpp) draws it through ps5ui::make_screen, below.
//
// Jellyfin Wheel's changes to the kit's design:
//   - one wheel (a Shelf) for each Jellyfin library, and a row of tabs above them: press UP to
//     move to the tabs, LEFT and RIGHT to change library, DOWN or CROSS to go back to the wheel.
//     The old wheel slides up and out while the new one slides up from below;
//   - L1 and R1 jump to the first title of the previous or next letter, using the A-Z offsets the
//     server gave; a big letter shows what you landed on, and the scrub bar carries the letters;
//   - the wheel has the library's real length at once; titles and posters arrive as the worker
//     thread fetches them around the wheel, and a dark card stands in until then;
//   - the tabs also hold Continue Watching, Next Up, Recently Added and Favourites, and a Search tab:
//     CROSS on it opens an on-screen keyboard and the results appear on the wheel as you type;
//   - SQUARE opens an A-Z picker, OPTIONS a menu to sort and filter the library;
//   - stepping from the tabs back down to the wheel makes the row hop and its ring flash;
//   - covers carry a progress bar, a NEW or WATCHED tag, and the favourite star (kept on the server);
//

// The whole collection stands in one row on a dark, reflecting floor. The
// carousel is not a list with an index: it is a wheel with a position and a
// velocity, and everything on screen is drawn from that one number. What
// makes it feel finished:
//
//   - the left stick scrubs the wheel, releasing it lets it coast on its own
//     inertia, and a spring then pulls it onto the nearest cover; the D-pad
//     and the triggers only move the spring's target, so every way of moving
//     blends into every other;
//   - depth without 3D: a cover's place, size and brightness are smooth
//     functions of its distance from the wheel position, and covers are drawn
//     far to near so the near ones overlap the far ones;
//   - each cover is mirrored in the floor: the same texture with its v range
//     reversed, squashed, darkened and veiled by a gradient of the floor's
//     own colour;
//   - the two ends are rubber: scrubbing into one stretches a little, coasting
//     into one bounces back, and pressing against one gives a soft refusal;
//   - the caption follows the wheel but only shows once it settles, so fast
//     scrubbing blurs past titles instead of strobing them;
//   - Cross lifts the centred cover out of the row toward the player while
//     the rest slide away, and a frosted panel with its details fades in.

#include "concepts/concepts.hpp"

// jellyfin: the Cover Wheel reads jf::lists()
#include "jf_library.hpp"
#include "jf_gate.h"
#include "jf_qr.hpp"

#include "core/tween.hpp"
#include "ui/glyphs.hpp"
#include "ui/motion.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#define JFW_TOUCH_GLYPH 1

namespace hui::screen
{

namespace
{

using gfx::Color;
using gfx::Rect;

const Color kWhite = Color::rgb(0xffffff);
const Color kClear = Color::rgb(0x000000, 0.0f);
const Color kBlack = Color::rgb(0x000000);
const Color kNight = Color::rgb(0x030408); // what every dark tone is mixed toward
const Color kInk = Color::rgb(0x0b0d16);   // text on white buttons
const Color kGold = Color::rgb(0xffd166);

// ---- the row: where a cover sits at distance d from the wheel position ----
constexpr float kCentreX = 960.0f;
constexpr float kCover = 440.0f;   // size of the centred cover
constexpr float kPosterTall = 1.10f;   // a poster's height, in units of the cover size
constexpr float kPosterSpacing = 0.86f; // posters are narrower than squares: the row closes up
constexpr float kRadius = 28.0f;   // its corner radius; scales with the cover
constexpr float kBaseY = 636.0f;   // the floor line under the centred cover
constexpr float kVanishY = 560.0f; // far covers stand closer to this height
constexpr float kSpreadA = 560.0f; // x = A * (1 - exp(-k |d|)) + B * |d|: the first
constexpr float kSpreadK = 0.95f;  //   step is wide, later ones crowd together
constexpr float kSpreadB = 40.0f;
constexpr float kFarScale = 0.36f; // size of the farthest covers
constexpr float kScaleFall = 1.1f;
constexpr float kFarLight = 0.26f; // brightness of the farthest covers
constexpr float kLightFall = 0.8f;
constexpr float kSoftAbs = 0.3f; // rounds |d| at 0 so size has no sharp peak
constexpr float kReach = 6.0f;   // covers farther than this are not drawn

// ---- the floor and the reflections ----
constexpr float kFloorY = 580.0f;      // the floor is opaque from here down
constexpr float kHorizon = 120.0f;     // ... and fades in over this height
constexpr float kReflectPart = 0.42f;  // how much of a cover is mirrored
constexpr float kReflectSquash = 0.8f; // the mirror image is a little flatter
constexpr float kReflectLight = 0.3f;  // and much darker
constexpr float kReflectHaze = 0.18f;  // veil opacity right under the cover

// ---- the wheel: units are covers and seconds ----
constexpr int kStart = 3;
constexpr int kJump = 5;               // L2 / R2
constexpr float kStickDead = 0.12f;    // below this the stick does not scrub
constexpr float kScrubSpeed = 11.0f;   // covers per second at full deflection
constexpr float kScrubResponse = 5.0f; // how quickly the wheel takes the stick's speed
constexpr float kFriction = 1.8f;      // coasting: speed decays by e every 1/1.8 s
constexpr float kSnapSpeed = 1.4f;     // below this a coasting wheel is caught
constexpr float kSnapLook = 0.18f;     // ... on the cover it would reach in this time
constexpr float kSnapOmega = 13.0f;
constexpr float kSnapDamping = 0.8f; // a touch under critical: weight, not wobble
constexpr float kRubber = 0.3f;      // how far a held stick stretches past an end
constexpr float kRubberRate = 12.0f;
constexpr float kRefuseKick = 3.2f;     // speed given to the wheel by a refused press
constexpr float kMaxEndSpeed = 8.0f;    // a coasting wheel hits an end no faster
constexpr float kEndSlack = 0.02f;      // this far past an end counts as hitting it
constexpr float kTickInterval = 0.085f; // at most about twelve ticks a second

// ---- the tabs, and the slide between two wheels ----
constexpr float kTabY = 56.0f;       // baseline of the labels
constexpr float kTabSize = 30.0f;
constexpr float kTabGap = 52.0f;
constexpr float kTabsMax = 1500.0f;  // the row is shrunk to fit this
constexpr float kSlide = 900.0f;     // how far a wheel travels sideways while it leaves or arrives
constexpr float kSlideTime = 0.55f;
constexpr float kCueLift = 30.0f;    // the row's hop when the wheel gets the focus back
constexpr float kLetterX = 140.0f;   // the big letter after an L1 / R1 jump
constexpr float kLetterSize = 150.0f;

// ---- the caption and the scrub bar ----
constexpr float kHeaderY = 110.0f;
constexpr float kTitleY = 822.0f;
constexpr float kMetaY = 868.0f;
constexpr float kBarX = 560.0f;
constexpr float kBarW = 800.0f;
constexpr float kBarY = 926.0f;
constexpr int kDotsMax = 120;     // up to this many covers the bar shows one dot each
constexpr float kCalmFrom = 5.5f; // faster than this the caption starts to hide
constexpr float kCalmSpan = 2.5f;

// ---- the opened cover and its panel ----
const Rect kOpenRect{206.0f, 230.0f, 400.0f, 600.0f}; // a 2:3 poster beside the panel
const Rect kPanel{760.0f, 160.0f, 1064.0f, 740.0f};
constexpr float kPanelRadius = 40.0f;
constexpr float kPanelPad = 56.0f;
constexpr float kOpenSlide = 520.0f; // how far the other covers move away

constexpr const char *kTechniques[] = {
    "A wheel with position and velocity: stick scrubbing, inertia, spring snap to a cover",
    "Depth without 3D: place, size and light from distance; covers drawn far to near",
    "Floor reflections: the cover texture with reversed v, squashed and veiled by a gradient",
    "Rubber-band ends: stretch while scrubbing, bounce when coasting, soft refusal on a press",
    "A caption that waits for the wheel to settle, so fast scrubbing never strobes text",
    "The centred cover lifts out of the row beside a frosted details panel (Frame::glass)",
};

constexpr app::TourStep kTour[] = {
    {0.4f, 0, Direction::right},
    {0.4f, 0, Direction::right},
    {0.85f, 0, Direction::none, "scrub", 1.0f}, // the stick is held, then the picture
    {2.2f, action_bit(Action::north)},          // let go: coast, settle, then favourite
    {0.9f, action_bit(Action::confirm), Direction::none, "settled"},
    {1.1f, action_bit(Action::confirm), Direction::none, "details"},
    {1.3f, action_bit(Action::back)},
    {0.7f, action_bit(Action::jump_next)},
    {1.4f, 0, Direction::none, "end", 1.0f}, // leaning on the last cover
    {0.9f, action_bit(Action::jump_prev)},
    {0.4f, action_bit(Action::jump_prev)},
};

// How the wheel is being driven right now.
enum class Drive
{
    snap,  // a spring pulls it onto target_
    scrub, // the stick sets its speed
    coast, // nothing touches it: friction only
};

// Where one cover is drawn.
struct Pose
{
    Rect rect;
    float light = 1.0f; // brightness of the artwork
    float alpha = 1.0f;
    float reflect = 1.0f; // strength of its mirror image
};

// '#' for bucket 0, then A..Z
char letter_char(int bucket)
{
    return bucket <= 0 ? '#' : static_cast<char>('A' + bucket - 1);
}

// One wheel: the titles of one Jellyfin library.

// ---- helpers for the film's page ----
bool has_text(const char *hay, const char *needle)
{
    return hay && std::strstr(hay, needle) != nullptr;
}

// Breaks text into lines no wider than `width`; a newline ends a line, two of them leave a gap.
template <class Font>
std::vector<std::string> wrap_lines(const Font &font, const char *text, float size, float width)
{
    std::vector<std::string> lines;
    std::string line, word;
    const auto flush = [&]() {
        if (word.empty())
            return;
        const std::string trial = line.empty() ? word : line + " " + word;
        if (!line.empty() && font.measure(trial.c_str(), size) > width)
        {
            lines.push_back(line);
            line = word;
        }
        else
            line = trial;
        word.clear();
    };
    for (const char *c = text; c && *c; ++c)
    {
        if (*c == '\n')
        {
            flush();
            lines.push_back(line);
            line.clear();
        }
        else if (*c == ' ' || *c == '\r' || *c == '\t')
            flush();
        else
            word += *c;
    }
    flush();
    if (!line.empty())
        lines.push_back(line);
    return lines;
}

std::string upper_text(std::string s)
{
    for (char &c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string channel_name(int channels)
{
    switch (channels)
    {
    case 1:
        return "1.0";
    case 2:
        return "2.0";
    case 6:
        return "5.1";
    case 8:
        return "7.1";
    default:
        return channels > 0 ? std::to_string(channels) + " ch" : std::string();
    }
}

std::string track_name(const JfGateTrack &t)
{
    if (t.title[0])
        return t.title;
    if (t.lang[0])
        return upper_text(t.lang);
    return "Track " + std::to_string(t.index);
}

std::string track_note(const JfGateTrack &t)
{
    std::string n;
    const auto add = [&n](const std::string &s) {
        if (s.empty())
            return;
        if (!n.empty())
            n += "  \xC2\xB7  ";
        n += s;
    };
    if (t.lang[0])
        add(upper_text(t.lang));
    if (t.codec[0])
        add(upper_text(t.codec));
    if (t.kind == 1)
        add(channel_name(t.channels));
    if (t.atmos)
        add("Atmos");
    if (t.is_default)
        add("Default");
    if (t.forced)
        add("Forced");
    if (t.external)
        add("External");
    if (t.kind == 2 && !t.text)
        add("Picture subtitles in a separate file: not supported");
    return n;
}

std::string url_escape(const std::string &s)
{
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : s)
    {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += static_cast<char>(c);
        else if (c == ' ')
            out += '+';
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

// The picture and sound flags a cover carries
int badges_of(const JfGateDetails &d)
{
    int b = 0;
    if (d.width >= 3000 || d.height >= 2000)
        b |= ::jf::Item::kBadge4K;
    else if (d.width >= 1800 || d.height >= 1000)
        b |= ::jf::Item::kBadgeHD;
    if (has_text(d.range, "Dolby"))
        b |= ::jf::Item::kBadgeDV;
    else if (has_text(d.range, "HDR") || has_text(d.range, "HLG"))
        b |= ::jf::Item::kBadgeHDR;
    for (int i = 0; i < d.track_count; ++i)
        if (d.tracks[i].kind == 1 && d.tracks[i].atmos)
            b |= ::jf::Item::kBadgeAtmos;
    return b;
}

const Color kDolby = Color::rgb(0xb98cff);
const Color kTomato = Color::rgb(0xfa5a3c);

class Shelf
{
  public:
    // The spotlight colour the backdrop is showing (it glides between covers): the controller's light follows it
    gfx::Color light_colour() const
    {
        return palette_[2].value();
    }

    Shelf(app::Context &context, ::jf::List &list) : context_(context), list_(list)
    {
        epoch_ = list_.epoch;
        if (list_.size() > 0)
            init();
    }

    // The list was thrown away and made again since the wheel last looked: what it holds is gone
    bool stale() const
    {
        return epoch_ != list_.epoch;
    }

    bool ready() const
    {
        return count_ > 0 && !stale();
    }

    int size() const
    {
        return ready() ? count_ : 0;
    }

    bool is_open() const
    {
        return open_;
    }

    // The cover the player means, for the worker thread to fetch around.
    int centre() const
    {
        return ready() ? current() : 0;
    }

    int category() const
    {
        return list_.category;
    }

    // Set before drawing: whether the tabs have the focus (and how much), and whether an overlay is up
    void set_chrome(bool tabs_focus, float tabs_amount, bool overlay) const
    {
        tabs_focus_ = tabs_focus;
        tabs_amount_ = tabs_amount;
        overlay_ = overlay;
    }

    // The wheel gets the focus back from the tabs: the row hops, its ring flashes
    void nudge()
    {
        if (!ready())
            return;
        cue_t_ = 0.0f;
    }

    // Moves to a position the player chose (the A-Z picker)
    void jump_to(int position, app::Feedback &feedback)
    {
        if (!ready())
            return;
        const int to = std::clamp(position, 0, count_ - 1);
        open_ = false;
        settle_on(to);
        quiet_ = true;
        letter_flash_ = 1.0f;
        letter_shown_ = letter_char(bucket_of(to));
        feedback.play(audio::Cue::tab, 1.0f, 0.0f);
    }

    // The letter a position falls under, for the picker
    int letter_bucket() const
    {
        return ready() ? bucket_of(current()) : 0;
    }

    bool lettered() const
    {
        return ready() && list_.lettersKnown;
    }

    const ::jf::List &list() const
    {
        return list_;
    }

    // Coming in from another category: the backdrop starts from the colours the old wheel was showing and glides to
    // this wheel's own, instead of jumping to whatever this wheel last had (that jump was a flash of brightness)
    void take_palette_from(const Shelf &old)
    {
        if (count_ <= 0 || old.count_ <= 0)
            return;
        for (int i = 0; i < 5; ++i)
            palette_[i].snap(old.palette_[i].value());
        apply_palette(false);
    }

    void enter()
    {
        sync_epoch();
        age_ = 0.0f;
        cue_t_ = -1.0f; // no hop or ring left over from the last visit
        cue_y_ = cue_flash_ = 0.0f;
        open_ = false;
        launch_.running = false;
        if (count_ > 0)
        {
            // Coming back mid-spin would be odd: the wheel is at rest on a cover.
            settle_on(current());
        }
        quiet_ = true;
    }

    void update(const InputFrame &input, float dt, app::Feedback &feedback)
    {
        age_ += dt;
        clock_ += dt;
        sync_epoch();
        if (count_ == 0)
        {
            if (list_.size() > 0)
                init();
            return;
        }
        if (cue_t_ >= 0.0f)
        {
            cue_t_ += dt;
            if (cue_t_ > 1.4f)
                cue_t_ = -1.0f;
        }
        cue_y_ = (cue_t_ >= 0.0f && !reduced()) ? -kCueLift * std::exp(-6.0f * cue_t_) * std::sin(14.0f * cue_t_) : 0.0f;
        cue_flash_ = cue_t_ >= 0.0f ? std::exp(-3.5f * cue_t_) : 0.0f;
        tick_wait_ = std::max(0.0f, tick_wait_ - dt);
        letter_flash_ = std::max(0.0f, letter_flash_ - dt / 1.1f);
        if (open_)
            update_details(input, dt, feedback);
        else if (episode_mode())
            update_episodes(input, dt, feedback);
        else
            update_wheel(input, feedback);
        spin(dt, feedback);

        // ---- animation state ----
        const float speed = std::fabs(wheel_.velocity);
        calm_.target = 1.0f - tween::clamp01((speed - kCalmFrom) / kCalmSpan);
        calm_.update(dt, 14.0f);
        for (ui::SpringColor &colour : palette_)
            colour.update(dt, 4.5f);
        lift_.target = open_ ? 1.0f : 0.0f;
        lift_.update(dt, reduced() ? 40.0f : 11.0f);
        // the program puts the opened title's backdrop behind its page (a wheel that is closed says nothing)
        if (open_ || lift_.value > 0.01f)
            ::jf::openedTitle() = open_ ? item(opened_).gateIndex : -1;
        launch_.update(dt);
        star_.update(dt, 5.0f);
        poll_badges(dt);
        ease_modal(dt);
    }

    // What lies behind everything: the spotlight, the floor, the header.
    void draw_base(app::Frame &frame) const
    {
        gfx::DrawList &list = frame.scene;
        if (count_ == 0 || stale())
        {
            draw_waiting(list);
            return;
        }
        // A spotlight in the centred cover's body colour, behind the row.
        frame.backdrop.mode = gfx::BackdropMode::gradient;
        frame.backdrop.colors[0] = palette_[0].value();
        frame.backdrop.colors[1] = palette_[1].value();
        frame.backdrop.colors[2] = palette_[2].value();
        frame.backdrop.params[0] = 0.5f;
        frame.backdrop.params[1] = 0.34f;
        frame.backdrop.params[2] = 0.5f + (reduced() ? 0.0f : 0.06f * ui::breathe(clock_, 5.0f));
        frame.backdrop.time = clock_;
        draw_floor(list);
        draw_header(list);
    }

    // The row, the caption and the bar, moved `dy` down and faded to `fade`: the slide between two
    // wheels. The active wheel also draws its details panel and the hints.
    void draw_front(app::Frame &frame, bool primary, float dy, float fade, float dx = 0.0f) const
    {
        if (count_ == 0 || stale())
            return;
        dy_ = dy;
        dx_ = dx;
        gfx::DrawList &list = frame.scene;
        const float lift = primary ? tween::clamp01(lift_.value) : 0.0f;
        // While the tabs have the focus the wheel steps back a little; it comes forward with a hop
        // (the wheel that is sliding out is dimmed just as much, or it would pop brighter for a moment as the slide begins)
        list.push_opacity(fade * (1.0f - 0.24f * tween::clamp01(tabs_amount_)));
        if (episode_mode())
            draw_episodes(list, lift);
        else
        {
            draw_row(list, lift);
            draw_caption(list, lift);
            draw_scrub_bar(list, lift);
            draw_letter(list);
            if (primary)
                draw_corner_qr(list, lift);
        }
        list.pop_opacity();
        if (primary && lift > 0.01f)
        {
            // The row steps back: a scrim over everything but the lifted cover.
            list.rounded_rect({0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0,
                              kNight.with_alpha((0.42f - 0.16f * ::jf::backdropShown()) * lift));
            // Launching: the cover flares in its own accent and the light
            // washes over the whole screen for a moment.
            const float flare = launch_.running ? 1.0f - launch_.progress() : 0.0f;
            draw_cover(list, opened_, pose(opened_), lift * (1.0f + 1.6f * flare * flare));
            frame.glass = true;
            draw_details(frame.overlay, frame.glass_texture, lift);
            if (flare > 0.0f)
                frame.overlay.rounded_rect({0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0,
                                           gfx::mix(item(opened_).accent, kWhite, 0.55f)
                                               .with_alpha(0.26f * lift * flare * flare * flare));
        }
        if (primary)
            draw_hints(lift > 0.5f ? frame.overlay : frame.scene, lift);
    }

  private:
    bool reduced() const
    {
        return context_.settings.reduced_motion;
    }

    // The list was made again: forget the old wheel
    void sync_epoch()
    {
        if (epoch_ == list_.epoch)
            return;
        epoch_ = list_.epoch;
        count_ = 0;
        open_ = false;
        lift_.snap(0.0f);
        launch_.running = false;
        cue_t_ = -1.0f;
        cue_y_ = cue_flash_ = 0.0f;
    }

    // The list has its titles: the wheel gets its length and rests on the first cover it shows.
    void init()
    {
        count_ = static_cast<int>(list_.size());
        target_ = centre_ = opened_ = std::min(kStart, count_ - 1);
        wheel_.snap(static_cast<float>(target_));
        calm_.snap(1.0f);
        apply_palette(true);
        ep_placed_ = false;
        ep_wait_ = 0.0f;
        ep_top_ = 0;
        ep_top_f_ = 0.0f;
    }

    const ::jf::Item &item(int index) const
    {
        return list_[static_cast<std::size_t>(index)];
    }

    float last() const
    {
        return static_cast<float>(count_ - 1);
    }

    // The cover closest to the wheel position.
    int nearest() const
    {
        return std::clamp(static_cast<int>(std::lround(wheel_.value)), 0, count_ - 1);
    }

    // The cover the player means: where the wheel is going, or where it is.
    int current() const
    {
        return drive_ == Drive::snap ? target_ : nearest();
    }

    // How far the wheel is past an end: negative before the first cover,
    // positive after the last, zero in between.
    float beyond() const
    {
        if (wheel_.value < 0.0f)
            return wheel_.value;
        return std::max(0.0f, wheel_.value - last());
    }

    // The stick is scrubbing outward from the first or the last cover.
    bool pushing_at_end() const
    {
        return drive_ == Drive::scrub && ((wheel_.value <= 0.0f && stick_ < 0.0f) ||
                                          (wheel_.value >= last() && stick_ > 0.0f));
    }

    // 1 when the wheel rests on a cover, 0 half-way between two. The caption,
    // the focus ring and the glow all fade with it. A wheel stretched past an
    // end still counts as resting on the end cover.
    float settled() const
    {
        const float inside = std::clamp(wheel_.value, 0.0f, last());
        const float off = std::fabs(inside - static_cast<float>(centre_));
        return 1.0f - tween::smoothstep((off - 0.08f) / 0.34f);
    }

    void settle_on(int index)
    {
        target_ = std::clamp(index, 0, count_ - 1);
        drive_ = Drive::snap;
    }

    // The backdrop, the floor and the accents take the centred cover's colours.
    void apply_palette(bool snap)
    {
        const ::jf::Item &it = item(centre_);
        const Color sky = gfx::mix(it.dark, it.mid, 0.3f);
        const Color ground = gfx::mix(it.dark, it.mid, 0.12f);
        const Color targets[5] = {
            gfx::mix(it.dark, kNight, 0.6f), // backdrop, top
            gfx::mix(sky, kNight, 0.35f),    // backdrop, bottom
            it.mid,                          // the spotlight behind the row
            gfx::mix(ground, kNight, 0.5f),  // the floor at its horizon
            it.accent,                       // the scrub bar's thumb
        };
        for (int i = 0; i < 5; ++i)
        {
            if (snap)
                palette_[i].snap(targets[i]);
            else
                palette_[i].target(targets[i]);
        }
    }

    // The letter a position falls under: the last letter that starts at or before it.
    int bucket_of(int position) const
    {
        if (list_.lettersKnown)
        {
            for (int k = 26; k >= 0; --k)
            {
                if (list_.letters[k] <= position && list_.letters[k] < count_)
                    return k;
            }
            return 0;
        }
        const char c = item(std::clamp(position, 0, count_ - 1)).letter;
        return c >= 'A' && c <= 'Z' ? c - 'A' + 1 : 0;
    }

    // ---- input ----

    // ---- an episode list: a season's episodes stand in a list with a summary, not on a wheel ----
    // The children of a season (browse list, kind 6) whose first titles are episodes
    bool episode_mode() const
    {
        if (count_ <= 0 || list_.kind != 6)
            return false;
        const int look = std::min(count_, 6);
        for (int i = 0; i < look; ++i)
            if (item(i).ready)
                return item(i).kindRaw == "Episode";
        return false;
    }

    // The play request for the episode under the cursor: where it left off, with the server's own audio and subtitles
    void play_episode(app::Feedback &feedback)
    {
        const ::jf::Item &it = item(current());
        if (!it.ready || !it.playable)
        {
            feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
            return;
        }
        ::jf::PlayOptions options;
        JfGateDetails d;
        std::memset(&d, 0, sizeof(d));
        if (::jf_gate_details_get(it.gateIndex, &d) == 1)
        {
            options.audio = d.default_audio;
            options.subtitle = d.default_subtitle;
        }
        feedback.play(audio::Cue::launch);
        ::jf::requestPlay(it.gateIndex, options);
        feedback.rumble(0.7f, 0.18f);
        launch_.start(0.9f);
    }

    void update_episodes(const InputFrame &input, float dt, app::Feedback &feedback)
    {
        const int here = current();
        // The list stands still; only when the choice leaves the seven rows on show does it scroll, by the least it can
        constexpr int kRows = 7;
        if (here < ep_top_)
            ep_top_ = here;
        else if (here > ep_top_ + kRows - 1)
            ep_top_ = here - kRows + 1;
        ep_top_ = std::clamp(ep_top_, 0, std::max(0, count_ - kRows));
        if (!ep_placed_)
            ep_top_f_ = static_cast<float>(ep_top_);
        else
            ep_top_f_ += (static_cast<float>(ep_top_) - ep_top_f_) * std::min(1.0f, dt * 14.0f);
        // On arrival the list walks past the episodes that were watched and rests on the first one that was not
        if (!ep_placed_)
        {
            ep_wait_ += dt;
            const ::jf::Item &it = item(here);
            if (input.nav != Direction::none || input.is_pressed(Action::confirm) || ep_wait_ > 4.0f)
                ep_placed_ = true;
            else if (it.ready)
            {
                if (it.watched && here + 1 < count_)
                {
                    wheel_.snap(static_cast<float>(here + 1));
                    settle_on(here + 1);
                    centre_ = here + 1;
                }
                else
                {
                    if (it.watched) // all watched: start at the top
                    {
                        wheel_.snap(0.0f);
                        settle_on(0);
                        centre_ = 0;
                    }
                    ep_placed_ = true;
                }
                return;
            }
            else
                return;
        }
        int to = here;
        if (input.nav == Direction::down)
            to = std::min(count_ - 1, here + 1);
        else if (input.nav == Direction::up)
            to = std::max(0, here - 1);
        if (input.is_pressed(Action::page_next))
            to = std::min(count_ - 1, here + 5);
        else if (input.is_pressed(Action::page_prev))
            to = std::max(0, here - 5);
        if (to != here)
        {
            settle_on(to);
            ::jf_gate_details_request(item(to).gateIndex); // so Play knows the usual audio and subtitles
            feedback.play(audio::Cue::focus, 1.0f, 0.0f);
        }
        else if ((input.nav == Direction::up || input.nav == Direction::down) && !input.nav_repeat)
            feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
        if (input.is_pressed(Action::north))
            toggle_favorite(feedback);
        if (input.is_pressed(Action::menu))
        {
            // OPTIONS: this episode's own page (audio, subtitles, who is in it)
            if (!item(here).ready)
            {
                feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
                return;
            }
            opened_ = here;
            settle_on(opened_);
            quiet_ = true;
            open_ = true;
            begin_details();
            feedback.play(audio::Cue::open);
            return;
        }
        if (input.is_pressed(Action::confirm))
            play_episode(feedback);
    }

    void update_wheel(const InputFrame &input, app::Feedback &feedback)
    {
        // The stick also produces nav repeats; while it scrubs, those are not
        // steps. A D-pad press arrives with the stick at rest.
        const bool scrubbing = std::fabs(input.stick_x) >= kStickDead && !input.focus_lost;
        if (scrubbing)
        {
            drive_ = Drive::scrub;
            stick_ = input.stick_x;
            quiet_ = false;
        }
        else if (drive_ == Drive::scrub)
        {
            release();
        }

        if (!scrubbing && (input.nav == Direction::left || input.nav == Direction::right))
        {
            const int direction = input.nav == Direction::right ? 1 : -1;
            if (move_target(direction))
            {
                // Left and right sound left and right, and a hair apart in pitch.
                feedback.play(audio::Cue::focus, 1.0f + 0.025f * static_cast<float>(direction),
                              0.3f * static_cast<float>(direction));
            }
            else if (!input.nav_repeat)
            {
                refuse(direction, feedback);
            }
        }
        for (const int direction : {-1, 1})
        {
            if (!input.is_pressed(direction < 0 ? Action::jump_prev : Action::jump_next))
                continue;
            if (move_target(direction * kJump))
                feedback.play(audio::Cue::tab, 1.0f + 0.06f * static_cast<float>(direction),
                              0.4f * static_cast<float>(direction));
            else
                refuse(direction, feedback);
        }
        // L1 and R1: the first title of the previous or next letter
        for (const int direction : {-1, 1})
        {
            if (input.is_pressed(direction < 0 ? Action::page_prev : Action::page_next))
                jump_letter(direction, feedback);
        }
        if (input.is_pressed(Action::north))
            toggle_favorite(feedback);
        if (input.is_pressed(Action::confirm))
        {
            if (!item(current()).ready)
            {
                // Its details have not arrived yet
                feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
                return;
            }
            if (item(current()).kindRaw == "Season")
            {
                // a season has nothing to say that its series did not: straight to its episodes
                ::jf::browseRequest() = item(current()).gateIndex;
                feedback.play(audio::Cue::open);
                return;
            }
            opened_ = current();
            settle_on(opened_);
            quiet_ = true;
            open_ = true;
            begin_details();
            feedback.play(audio::Cue::open);
        }
    }

    // ---- the film's page: four buttons in a row, a summary that scrolls, and the track pickers ----
    enum class Modal
    {
        none,
        audio,
        subs
    };
    struct PickRow
    {
        int stream;
        std::string name;
        std::string note;
        bool usable;
    };
    static constexpr int kSubDefault = -2; // the person has not chosen: the server's default

    // The one QR code of a title: IMDb for a film, TheTVDB for a show, TMDb for a collection, else a web search
    static std::string qr_url_for(const JfGateDetails &d, const ::jf::Item &it)
    {
        const bool series = !std::strcmp(d.kind, "Series") || !std::strcmp(d.kind, "Season");
        const bool episode = !std::strcmp(d.kind, "Episode");
        const bool boxset = !std::strcmp(d.kind, "BoxSet");
        if (series && d.tvdb[0])
            return std::string("https://thetvdb.com/?tab=series&id=") + d.tvdb;
        if (episode && d.tvdb[0])
            return std::string("https://thetvdb.com/?tab=episode&id=") + d.tvdb;
        if (boxset && d.tmdb[0])
            return std::string("https://themoviedb.org/collection/") + d.tmdb;
        if (!boxset && d.imdb[0])
            return std::string("https://imdb.com/title/") + d.imdb;
        if (d.tmdb[0])
            return std::string("https://themoviedb.org/") + ((series || episode) ? "tv/" : "movie/") + d.tmdb;
        std::string query = it.titleText;
        if (query.size() > 60)
            query.resize(60);
        if (it.year > 0)
            query += " " + std::to_string(it.year);
        return "https://duckduckgo.com/?q=" + url_escape(query);
    }

    void begin_details()
    {
        focus_ = 0;
        modal_ = Modal::none;
        modal_shown_ = Modal::none;
        modal_amt_ = 0.0f;
        det_state_ = 0;
        det_wait_ = 0.0f;
        std::memset(&det_, 0, sizeof(det_));
        det_.default_audio = -1;
        det_.default_subtitle = -1;
        const ::jf::Item &it = item(opened_);
        if (pick_gate_ != it.gateIndex)
        {
            pick_gate_ = it.gateIndex;
            pick_audio_ = -1;
            pick_sub_ = kSubDefault;
        }
        wrap_gate_ = -1;
        read_gate_ = -1;
        scroll_ = scroll_target_ = 0.0f;
    }

    const JfGateTrack *find_track(int stream) const
    {
        if (det_state_ != 1)
            return nullptr;
        for (int i = 0; i < det_.track_count; ++i)
            if (det_.tracks[i].index == stream)
                return &det_.tracks[i];
        return nullptr;
    }

    int count_tracks(int kind) const
    {
        int n = 0;
        if (det_state_ == 1)
            for (int i = 0; i < det_.track_count; ++i)
                n += det_.tracks[i].kind == kind ? 1 : 0;
        return n;
    }

    // The sound track that will play: the one picked, else the server's default, else -1 (the player chooses)
    int effective_audio() const
    {
        if (pick_audio_ >= 0)
            return pick_audio_;
        return det_state_ == 1 ? det_.default_audio : -1;
    }

    // The subtitle track that will show: -1 is none
    int effective_sub() const
    {
        if (pick_sub_ != kSubDefault)
            return pick_sub_;
        if (det_state_ == 1 && det_.default_subtitle >= 0)
        {
            const JfGateTrack *t = find_track(det_.default_subtitle);
            if (t && t->text)
                return t->index;
        }
        return -1;
    }

    bool resumable() const
    {
        const ::jf::Item &it = item(opened_);
        return it.playable && it.resumeSeconds > 1.0f;
    }

    // The buttons that are on the page, left to right: 0 play or resume, 1 from start, 2 audio, 3 subtitles
    int slots(int *ids) const
    {
        int n = 0;
        ids[n++] = 0;
        if (resumable())
            ids[n++] = 1;
        ids[n++] = 2;
        ids[n++] = 3;
        return n;
    }

    void poll_details(float dt)
    {
        if (det_state_ != 0)
            return;
        det_wait_ -= dt;
        if (det_wait_ > 0.0f)
            return;
        det_wait_ = 0.2f;
        ::jf::Item &it = *list_.items[static_cast<std::size_t>(opened_)];
        const int state = ::jf_gate_details_request(it.gateIndex);
        if (state == 1 && ::jf_gate_details_get(it.gateIndex, &det_))
        {
            det_state_ = 1;
            it.badges = badges_of(det_);
            it.detailed = true;
            it.qrUrl = qr_url_for(det_, it);
        }
        else if (state < 0)
            det_state_ = -1;
    }

    // The centred cover, once the wheel has rested, asks for its picture and sound flags and its QR link
    void poll_badges(float dt)
    {
        badge_wait_ -= dt;
        if (badge_wait_ > 0.0f || open_ || count_ <= 0 || stale() || calm_.value < 0.95f || tabs_focus_)
            return;
        badge_wait_ = 0.12f;
        ::jf::Item &it = *list_.items[static_cast<std::size_t>(current())];
        if (!it.ready || it.detailed)
            return;
        static JfGateDetails scratch;
        const int state = ::jf_gate_details_request(it.gateIndex);
        if (state == 1 && ::jf_gate_details_get(it.gateIndex, &scratch))
        {
            it.badges = it.playable ? badges_of(scratch) : 0;
            it.qrUrl = qr_url_for(scratch, it);
            it.detailed = true;
        }
        else if (state < 0)
            it.detailed = true;
    }

    void ease_modal(float dt)
    {
        if (!open_)
            modal_ = Modal::none;
        const float target = modal_ != Modal::none ? 1.0f : 0.0f;
        if (reduced())
        {
            modal_amt_ = target;
            scroll_ = scroll_target_;
        }
        else
        {
            modal_amt_ += (target - modal_amt_) * (1.0f - std::exp(-16.0f * dt));
            scroll_ += (scroll_target_ - scroll_) * (1.0f - std::exp(-14.0f * dt));
        }
        if (std::fabs(target - modal_amt_) < 0.004f)
            modal_amt_ = target;
        if (modal_ != Modal::none)
            modal_shown_ = modal_;
    }

    void update_details(const InputFrame &input, float dt, app::Feedback &feedback)
    {
        poll_details(dt);
        if (modal_ != Modal::none)
        {
            update_modal(input, feedback);
            return;
        }
        // The summary scrolls with the right stick, up and down, and L1 / R1 for a page
        float move = 0.0f;
        if (input.nav == Direction::up)
            move = -72.0f;
        else if (input.nav == Direction::down)
            move = 72.0f;
        if (input.is_pressed(Action::page_prev))
            move = -summary_view_h_ * 0.85f;
        else if (input.is_pressed(Action::page_next))
            move = summary_view_h_ * 0.85f;
        const float stick = ::jf::rightStickY();
        if (std::fabs(stick) > 0.2f)
        {
            scroll_target_ += stick * 1000.0f * dt;
            scroll_ = scroll_target_;
        }
        scroll_target_ = std::clamp(scroll_target_ + move, 0.0f, std::max(0.0f, summary_max_));
        if (input.is_pressed(Action::north))
            toggle_favorite(feedback);
        if (input.is_pressed(Action::menu))
            request_cast(feedback);
        if (input.is_pressed(Action::back))
        {
            feedback.play(audio::Cue::back);
            open_ = false;
            return;
        }
        if (input.nav == Direction::left || input.nav == Direction::right)
            move_focus(input.nav, feedback);
        if (input.is_pressed(Action::confirm))
            activate(feedback);
    }

    // Cast and info from Seerr (Discover) for the title that is open: needs its TMDB number and Seerr switched on
    bool cast_available() const
    {
        if (!open_ || det_state_ != 1 || !det_.tmdb[0] || !::jf::discoverEnabled())
            return false;
        return !std::strcmp(det_.kind, "Movie") || !std::strcmp(det_.kind, "Series");
    }

    void request_cast(app::Feedback &feedback)
    {
        if (!cast_available())
        {
            feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
            return;
        }
        ::jf::CastRequest &r = ::jf::castRequest();
        r.tmdb = std::atoll(det_.tmdb);
        std::snprintf(r.type, sizeof(r.type), "%s", !std::strcmp(det_.kind, "Series") ? "tv" : "movie");
        std::snprintf(r.title, sizeof(r.title), "%s", item(opened_).title);
        r.pending = r.tmdb > 0;
        feedback.play(r.pending ? audio::Cue::open : audio::Cue::error);
    }

    void move_focus(Direction direction, app::Feedback &feedback)
    {
        int ids[4];
        const int n = slots(ids);
        int at = 0;
        for (int i = 0; i < n; ++i)
            if (ids[i] == focus_)
                at = i;
        const int to = std::clamp(at + (direction == Direction::right ? 1 : -1), 0, n - 1);
        if (to != at)
        {
            focus_ = ids[to];
            feedback.play(audio::Cue::tab, 1.1f, 0.0f, 0.6f);
        }
    }

    void activate(app::Feedback &feedback)
    {
        const ::jf::Item &it = item(opened_);
        switch (focus_)
        {
        case 0:
        case 1:
            if (!it.playable)
            {
                // A series or a collection: look inside it
                if (focus_ != 0)
                {
                    feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
                    return;
                }
                feedback.play(audio::Cue::open);
                ::jf::browseRequest() = it.gateIndex;
                open_ = false;
                return;
            }
            else
            {
                ::jf::PlayOptions options;
                options.fromStart = focus_ == 1;
                options.audio = effective_audio();
                options.subtitle = effective_sub();
                feedback.play(audio::Cue::launch);
                ::jf::requestPlay(it.gateIndex, options); // jellyfin: play this film
                feedback.rumble(0.7f, 0.18f);
                launch_.start(0.9f);
            }
            break;
        case 2:
            open_picker(Modal::audio, feedback);
            break;
        default:
            open_picker(Modal::subs, feedback);
            break;
        }
    }

    void open_picker(Modal kind, app::Feedback &feedback)
    {
        const bool sound = kind == Modal::audio;
        if (!item(opened_).playable || det_state_ != 1 || count_tracks(sound ? 1 : 2) == 0)
        {
            feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
            return;
        }
        rows_.clear();
        if (!sound)
            rows_.push_back({-1, "Off", "No subtitles", true});
        for (int i = 0; i < det_.track_count; ++i)
        {
            const JfGateTrack &t = det_.tracks[i];
            if (t.kind != (sound ? 1 : 2))
                continue;
            rows_.push_back({t.index, track_name(t), track_note(t), sound ? true : t.text != 0});
        }
        const int current = sound ? effective_audio() : effective_sub();
        cursor_ = 0;
        for (std::size_t i = 0; i < rows_.size(); ++i)
            if (rows_[i].stream == current)
                cursor_ = static_cast<int>(i);
        modal_ = kind;
        feedback.play(audio::Cue::open);
    }

    void update_modal(const InputFrame &input, app::Feedback &feedback)
    {
        if (input.is_pressed(Action::back))
        {
            feedback.play(audio::Cue::back);
            modal_ = Modal::none;
            return;
        }
        if (input.nav == Direction::up || input.nav == Direction::down)
        {
            const int next = std::clamp(cursor_ + (input.nav == Direction::down ? 1 : -1), 0,
                                        static_cast<int>(rows_.size()) - 1);
            if (next != cursor_)
            {
                cursor_ = next;
                feedback.play(audio::Cue::tab, 1.1f, 0.0f, 0.6f);
            }
        }
        if (input.is_pressed(Action::confirm) && cursor_ >= 0 && cursor_ < static_cast<int>(rows_.size()))
        {
            const PickRow &row = rows_[static_cast<std::size_t>(cursor_)];
            if (!row.usable)
            {
                feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
                return;
            }
            if (modal_ == Modal::audio)
                pick_audio_ = row.stream;
            else
                pick_sub_ = row.stream;
            feedback.play(audio::Cue::focus, 1.14f, 0.0f, 0.9f);
            modal_ = Modal::none;
        }
    }

    // Moves the spring's target by `delta` covers, stopping at the ends.
    // Returns false when it could not move at all.
    bool move_target(int delta)
    {
        const int from = current();
        const int to = std::clamp(from + delta, 0, count_ - 1);
        if (to == from)
            return false;
        settle_on(to);
        quiet_ = true; // the press had its own sound: no ticks on the way
        return true;
    }

    // L1 / R1: to the first title of the previous or next letter. Where the server's letter offsets
    // are not known yet the wheel moves ten covers instead.
    void jump_letter(int direction, app::Feedback &feedback)
    {
        const int from = current();
        int to = from;
        if (list_.lettersKnown)
        {
            int best = -1;
            for (int k = 0; k < 27; ++k)
            {
                const int start = list_.letters[k];
                if (start < 0 || start >= count_)
                    continue;
                if (direction > 0 && start > from && (best < 0 || start < best))
                    best = start;
                if (direction < 0 && start < from && start > best)
                    best = start;
            }
            if (best >= 0)
                to = best;
        }
        else
        {
            to = std::clamp(from + direction * 2 * kJump, 0, count_ - 1);
        }
        if (to == from)
        {
            refuse(direction, feedback);
            return;
        }
        settle_on(to);
        quiet_ = true;
        letter_flash_ = 1.0f;
        letter_shown_ = letter_char(bucket_of(to));
        feedback.play(audio::Cue::tab, 1.0f + 0.06f * static_cast<float>(direction),
                      0.4f * static_cast<float>(direction));
    }

    // The end of the row answers: a quiet cue, a light pulse, and the wheel
    // is pushed against its stop so the spring shows the bump.
    void refuse(int direction, app::Feedback &feedback)
    {
        feedback.play(audio::Cue::error, 1.0f, 0.4f * static_cast<float>(direction), 0.6f);
        feedback.rumble(0.25f, 0.05f);
        quiet_ = true;
        if (reduced())
            return;
        settle_on(current());
        wheel_.kick(kRefuseKick * static_cast<float>(direction));
    }

    // The stick was let go.
    void release()
    {
        if (beyond() != 0.0f)
        {
            settle_on(nearest());
        }
        else if (reduced() || std::fabs(wheel_.velocity) < kSnapSpeed)
        {
            catch_wheel();
        }
        else
        {
            drive_ = Drive::coast;
        }
    }

    // Hands a slow wheel to the spring, aimed at the cover it was heading for.
    // The velocity is kept, so the hand-over cannot be seen.
    void catch_wheel()
    {
        settle_on(static_cast<int>(std::lround(wheel_.value + wheel_.velocity * kSnapLook)));
    }

    // Makes the title a favourite or not: here at once, and on the server through the program
    void toggle_favorite(app::Feedback &feedback)
    {
        const int index = open_ ? opened_ : current();
        ::jf::Item &it = *list_.items[static_cast<std::size_t>(index)];
        if (!it.ready)
        {
            feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
            return;
        }
        it.favorite = !it.favorite;
        ::jf::favouriteRequests().push_back({it.gateIndex, it.favorite});
        feedback.play(it.favorite ? audio::Cue::favorite_on : audio::Cue::favorite_off);
        if (it.favorite)
            star_.trigger();
    }

    // ---- physics ----

    // Advances the wheel by dt according to how it is driven, then reports
    // what the movement caused: an end reached, a new cover in the centre.
    void spin(float dt, app::Feedback &feedback)
    {
        switch (drive_)
        {
        case Drive::scrub:
        {
            // A response curve gives fine control near the centre of the stick.
            const float push = std::copysign(std::pow(std::fabs(stick_), 1.6f), stick_);
            if (pushing_at_end())
            {
                // Rubber: past an end the stick no longer sets a speed, it
                // sets how far the row is stretched, and the row eases there.
                const float end = push < 0.0f ? 0.0f : last();
                const float stretch = end + (reduced() ? 0.0f : kRubber * push);
                wheel_.value += (stretch - wheel_.value) * (1.0f - std::exp(-kRubberRate * dt));
                wheel_.velocity = 0.0f;
            }
            else
            {
                // The wheel is heavy: it takes a moment to reach the stick's speed.
                wheel_.velocity += (push * kScrubSpeed - wheel_.velocity) *
                                   (1.0f - std::exp(-kScrubResponse * dt));
                wheel_.value += wheel_.velocity * dt;
            }
            break;
        }
        case Drive::coast:
            wheel_.velocity *= std::exp(-kFriction * dt);
            wheel_.value += wheel_.velocity * dt;
            if (beyond() != 0.0f)
            {
                // Ran into an end: the spring takes the remaining speed and
                // turns it into the bounce.
                wheel_.velocity = std::clamp(wheel_.velocity, -kMaxEndSpeed, kMaxEndSpeed);
                settle_on(nearest());
            }
            else if (std::fabs(wheel_.velocity) < kSnapSpeed)
            {
                catch_wheel();
            }
            break;
        case Drive::snap:
            wheel_.target = static_cast<float>(target_);
            wheel_.update(dt, reduced() ? 18.0f : kSnapOmega, reduced() ? 1.0f : kSnapDamping);
            break;
        }
        if (reduced())
            wheel_.value = std::clamp(wheel_.value, 0.0f, last());

        // Reaching an end by stick or by momentum is refused once per visit.
        // A press that was refused has had its sound already (quiet_), and the
        // last ripples of the spring around an end are not a new visit.
        const bool at_end = std::fabs(beyond()) > kEndSlack || pushing_at_end();
        if (at_end && !against_end_ && !quiet_)
        {
            const float side = wheel_.value < last() * 0.5f ? -1.0f : 1.0f;
            feedback.play(audio::Cue::error, 1.0f, 0.4f * side, 0.6f);
            feedback.rumble(0.25f, 0.05f);
        }
        against_end_ = at_end;

        const int centre = nearest();
        if (centre != centre_)
        {
            const float direction = centre > centre_ ? 1.0f : -1.0f;
            centre_ = centre;
            apply_palette(false);
            // Detents: one soft tick per cover that passes the centre, faster
            // spins a little higher, and never more than the ear can separate.
            if (!quiet_ && tick_wait_ <= 0.0f)
            {
                const float rate = tween::clamp01(std::fabs(wheel_.velocity) / kScrubSpeed);
                feedback.play(audio::Cue::tick, 0.94f + 0.22f * rate, 0.25f * direction, 0.8f);
                tick_wait_ = kTickInterval;
            }
        }
    }

    // ---- geometry ----

    // Everything about one cover follows from d, its distance from the wheel
    // position: there is no "selected" state to switch, so scrubbing, coasting
    // and stepping all look right for free.
    Pose pose(int index) const
    {
        const float d = static_cast<float>(index) - wheel_.value;
        const float away = std::fabs(d);
        const float side = d < 0.0f ? -1.0f : 1.0f;
        const float soft = std::sqrt(d * d + kSoftAbs * kSoftAbs) - kSoftAbs;
        const float lift = tween::clamp01(lift_.value);

        float offset = side * (kSpreadA * (1.0f - std::exp(-away * kSpreadK)) + kSpreadB * away);
        float scale = kFarScale + (1.0f - kFarScale) * std::exp(-soft * kScaleFall);
        Pose result;
        result.light = kFarLight + (1.0f - kFarLight) * std::exp(-soft * kLightFall);
        result.alpha = tween::clamp01(kReach - away);

        // Entrance: the row closes in from both sides, centre first.
        const int rank = std::min(static_cast<int>(away + 0.5f), static_cast<int>(kReach));
        const float in = tween::stagger(age_, rank, 0.055f, 0.55f);
        result.alpha *= in;
        if (!reduced())
        {
            offset *= 1.0f + 0.3f * (1.0f - in);
            scale *= 0.92f + 0.08f * in;
        }
        // While a cover is open the others make room and fade.
        if (index != opened_)
        {
            offset += side * kOpenSlide * tween::cubic_in_out(lift);
            result.alpha *= 1.0f - lift;
        }

        // Bases rise toward the vanishing height as covers shrink, so the row
        // stands on a floor that recedes instead of hanging from a line.
        const float size = kCover * scale;
        const float base = kVanishY + (kBaseY - kVanishY) * scale;
        const float tall = size * kPosterTall;
        const float wide = tall / item(index).aspect;
        result.rect = {kCentreX + dx_ + offset * kPosterSpacing - wide * 0.5f, base - tall + dy_ + cue_y_, wide, tall};

        if (index == opened_ && lift > 0.0f)
        {
            const float t = tween::cubic_in_out(lift);
            Rect open = kOpenRect;
            if (launch_.running && !reduced())
                open = open.inset(-14.0f * tween::ping(tween::cubic_out(launch_.progress())));
            result.rect = {
                tween::lerp(result.rect.x, open.x, t), tween::lerp(result.rect.y, open.y, t),
                tween::lerp(result.rect.w, open.w, t), tween::lerp(result.rect.h, open.h, t)};
            result.light = tween::lerp(result.light, 1.0f, t);
            // It left the floor: its mirror image goes first.
            result.reflect = 1.0f - tween::clamp01(lift * 3.0f);
        }
        return result;
    }

    // The floor's colour at a height: the reflections are veiled with it.
    Color floor_at(float y) const
    {
        const float t = tween::clamp01((y - kFloorY) / (gfx::kVirtualHeight - kFloorY));
        return gfx::mix(palette_[3].value(), kNight, t);
    }

    // ---- drawing ----

    // A library that has not said how long it is yet, or has nothing in it.
    void draw_waiting(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        char text[200];
        if (!list_.message.empty())
            std::snprintf(text, sizeof(text), "%s", list_.message.c_str());
        else if (list_.known)
            std::snprintf(text, sizeof(text), "Nothing in %s", list_.name.c_str());
        else
            return; // nothing is written behind the wheel while a library loads
        const float pulse = list_.known ? 0.0f : 0.5f + 0.5f * ui::breathe(clock_, 1.6f);
        ui::text(list, fonts.display, text, kCentreX, 560.0f, 52, kWhite.with_alpha(0.5f + 0.35f * pulse),
                 gfx::Align::center);
    }

    void draw_floor(gfx::DrawList &list) const
    {
        // a title's backdrop picture shows through the floor while its page is open
        const float see = 1.0f - 0.94f * tween::clamp01(::jf::backdropShown() * lift_.value);
        const Color top = floor_at(kFloorY);
        list.gradient_rect({0, kFloorY - kHorizon, gfx::kVirtualWidth, kHorizon}, 0,
                           top.with_alpha(0.0f), top.with_alpha(see));
        list.gradient_rect({0, kFloorY, gfx::kVirtualWidth, gfx::kVirtualHeight - kFloorY}, 0, top.with_alpha(see),
                           floor_at(gfx::kVirtualHeight).with_alpha(see));
    }

    void draw_header(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float in = tween::stagger(age_, 0, 0.05f, 0.5f);
        char name[64];
        std::snprintf(name, sizeof(name), "%s", list_.name.c_str());
        for (char *c = name; *c; ++c)
            *c = static_cast<char>(std::toupper(static_cast<unsigned char>(*c)));
        char text[160];
        if (list_.kind == 5)
        {
            char term[48];
            std::snprintf(term, sizeof(term), "%s", ::jf::searchText().c_str());
            for (char *c = term; *c; ++c)
                *c = static_cast<char>(std::toupper(static_cast<unsigned char>(*c)));
            std::snprintf(text, sizeof(text), "%d RESULTS FOR \xE2\x80\x9C%s\xE2\x80\x9D", count_, term);
        }
        else if (list_.sortable && (list_.sort != 0 || list_.filter != 0))
        {
            static const char *const sorts[] = {"A-Z", "NEWEST", "RECENTLY ADDED", "TOP RATED", "LONGEST"};
            static const char *const filters[] = {"", "  \xC2\xB7  UNWATCHED", "  \xC2\xB7  FAVOURITES"};
            std::snprintf(text, sizeof(text), "%s  \xC2\xB7  %d TITLES  \xC2\xB7  %s%s", name, count_,
                          sorts[std::clamp(list_.sort, 0, 4)], filters[std::clamp(list_.filter, 0, 2)]);
        }
        else
            std::snprintf(text, sizeof(text), "%s  \xC2\xB7  %d TITLES", name, count_);
        ui::text(list, fonts.semibold, text, kCentreX, kHeaderY - 10.0f * (1.0f - in), 18,
                 kWhite.with_alpha(0.6f * in), gfx::Align::center, 4.0f);
    }

    // One cover with its mirror image, shadow, glow and star. `focus` is how
    // much of the centre's light it gets.
    void draw_cover(gfx::DrawList &list, int index, const Pose &p, float focus) const
    {
        if (p.alpha <= 0.01f)
            return;
        const ::jf::Item &it = item(index);
        const Rect &r = p.rect;
        const float k = r.w / kCover;
        const float radius = kRadius * k;
        list.push_opacity(p.alpha);

        if (p.reflect > 0.01f)
        {
            // The mirror image: the lower part of the artwork with its v range
            // running the other way (a canvas texture's top row is v = 1, so
            // 0 -> part puts the cover's bottom edge at the top). It is drawn
            // opaque and dark so overlapping reflections hide each other like
            // the covers do, then veiled with the floor's own gradient until
            // nothing is left.
            const Rect mirror{r.x, r.y + r.h + 3.0f * k, r.w, r.h * kReflectPart * kReflectSquash};
            const float light = p.light * kReflectLight;
            list.image(it.cover, mirror, {0.0f, 1.0f, 1.0f, -kReflectPart},
                       {light, light, light, 1.0f}, radius);
            const Rect veil{mirror.x - 1.0f, mirror.y, mirror.w + 2.0f, mirror.h + 2.0f};
            const float haze = 1.0f - p.reflect * (1.0f - kReflectHaze);
            list.gradient_rect(veil, 0, floor_at(veil.y).with_alpha(haze),
                               floor_at(veil.y + veil.h));
        }

        // The shadow separates overlapping covers and grounds each one.
        const float height = 1.0f - p.reflect; // 0 on the floor, 1 lifted
        list.shadow({r.x, r.y + (14.0f + 16.0f * height) * k, r.w, r.h}, radius,
                    (30.0f + 20.0f * height) * k, kBlack.with_alpha(0.55f));
        if (focus > 0.01f)
        {
            const float breath = reduced() ? 0.5f : ui::breathe(clock_);
            list.glow(r.inset(24.0f), radius, 80.0f,
                      it.accent.with_alpha((0.24f + 0.1f * breath) * focus));
        }
        list.image(it.cover, r, gfx::kFullUv, {p.light, p.light, p.light, 1.0f}, radius);
        list.bordered_rect(r, radius, kClear, 1.5f, kWhite.with_alpha(0.16f * p.light));

        draw_badges(list, it, r, k, p.light);
        if (it.favorite)
        {
            // A badge on the corner; it pops when it is given.
            const bool fresh = index == (open_ ? opened_ : current());
            const float pop = fresh ? star_.value : 0.0f;
            const float cx = r.x + 40.0f * k;
            const float cy = r.y + 40.0f * k;
            list.circle(cx, cy, (24.0f + 6.0f * pop) * k, kInk.with_alpha(0.72f));
            list.star(cx, cy, (14.0f + 9.0f * pop) * k, gfx::mix(kInk, kGold, p.light));
        }
        list.pop_opacity();
    }

    // What the cover says about the title: how far it was watched, or that it is new or seen
    void draw_badges(gfx::DrawList &list, const ::jf::Item &it, const Rect &r, float k, float light) const
    {
        const float s = r.w / 320.0f; // 1 for the centred poster
        if (s < 0.55f || !it.ready)
            return;
        const ui::Fonts &fonts = context_.fonts;
        const float radius = kRadius * k;
        if (it.progress > 0.01f)
        {
            const float h = 84.0f * s;
            list.gradient_rect({r.x, r.y + r.h - h, r.w, h}, radius, kBlack.with_alpha(0.0f), kBlack.with_alpha(0.62f * light));
            const Rect track{r.x + 20.0f * s, r.y + r.h - 28.0f * s, r.w - 40.0f * s, 7.0f * s};
            list.rounded_rect(track, 3.5f * s, kWhite.with_alpha(0.28f * light));
            list.rounded_rect({track.x, track.y, std::max(7.0f * s, track.w * it.progress), track.h}, 3.5f * s,
                              gfx::mix(it.accent, kWhite, 0.25f).with_alpha(light));
        }
        if (it.badges != 0 && s >= 0.8f)
        {
            // Picture flags in the top corner: Dolby Vision or HDR first, then 4K
            const char *labels[2] = {nullptr, nullptr};
            Color fills[2] = {kWhite, kWhite};
            int n = 0;
            if (it.badges & ::jf::Item::kBadgeDV)
            {
                labels[n] = "DV";
                fills[n++] = kDolby;
            }
            else if (it.badges & ::jf::Item::kBadgeHDR)
            {
                labels[n] = "HDR";
                fills[n++] = kGold;
            }
            if (it.badges & ::jf::Item::kBadge4K)
            {
                labels[n] = "4K";
                fills[n++] = kWhite;
            }
            const int csize = std::max(9, static_cast<int>(16.0f * s));
            float cx = r.x + r.w - 14.0f * s;
            for (int i = 0; i < n; ++i)
            {
                const float w = fonts.semibold.measure(labels[i], csize) + 22.0f * s;
                cx -= w;
                const Rect chip{cx, r.y + 16.0f * s, w, 30.0f * s};
                list.rounded_rect(chip, 8.0f * s, gfx::mix(kInk, fills[i], light).with_alpha(0.92f * light));
                ui::text(list, fonts.semibold, labels[i], chip.x + 11.0f * s, chip.cy() + csize * 0.36f, csize,
                         gfx::mix(kWhite, kInk, light));
                cx -= 8.0f * s;
            }
        }
        if (!it.countText.empty() && s >= 0.8f)
        {
            // "3 seasons": bottom left, above the progress bar when there is one
            const int csize = std::max(9, static_cast<int>(17.0f * s));
            const float cw = fonts.semibold.measure(it.countText.c_str(), csize) + 26.0f * s;
            const Rect count{r.x + 16.0f * s, r.y + r.h - (it.progress > 0.01f ? 126.0f : 56.0f) * s, cw, 34.0f * s};
            list.rounded_rect(count, 17.0f * s, kInk.with_alpha(0.78f * light));
            ui::text(list, fonts.semibold, it.countText.c_str(), count.x + 13.0f * s, count.cy() + csize * 0.36f, csize,
                     gfx::mix(kInk, kWhite, light));
        }
        const char *tag = nullptr;
        Color tint = kWhite;
        if (it.fresh && !it.watched && it.progress <= 0.01f)
        {
            tag = "NEW";
            tint = kGold;
        }
        else if (it.watched && it.progress <= 0.01f)
        {
            tag = "WATCHED";
        }
        if (!tag)
            return;
        const int size = std::max(9, static_cast<int>(17.0f * s));
        const float width = fonts.semibold.measure(tag, size) + 26.0f * s;
        const Rect pill{r.x + 16.0f * s, r.y + (it.favorite ? 82.0f : 16.0f) * s, width, 34.0f * s};
        list.rounded_rect(pill, 17.0f * s, kInk.with_alpha(0.74f * light));
        ui::text(list, fonts.semibold, tag, pill.x + 13.0f * s, pill.cy() + size * 0.36f, size,
                 gfx::mix(kInk, tint, light));
    }

    // A green tick: the episode was watched
    static void draw_check(gfx::DrawList &list, float cx, float cy, float r, float alpha)
    {
        list.circle(cx, cy, r, Color{0.14f, 0.66f, 0.36f, alpha});
        const float u = r / 20.0f;
        for (int k = 0; k <= 4; ++k)
            list.circle(cx - 8.0f * u + 2.0f * u * static_cast<float>(k), cy + 1.0f * u + 2.0f * u * static_cast<float>(k), 2.2f * u,
                        kWhite.with_alpha(alpha));
        for (int k = 1; k <= 9; ++k)
            list.circle(cx - 2.0f * u + 1.2f * u * static_cast<float>(k), cy + 7.0f * u - 1.3f * u * static_cast<float>(k), 2.2f * u,
                        kWhite.with_alpha(alpha));
    }

    // The list on the left (the chosen episode stays at the middle and the list moves), the chosen episode on the right
    void draw_episodes(gfx::DrawList &list, float lift) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float fade = 1.0f - 0.7f * lift;
        const float tabs = tween::clamp01(tabs_amount_);
        const int sel = current();
        const float v = std::clamp(wheel_.value, 0.0f, last());
        const float row_h = 92.0f;
        const float lx = 96.0f, lw = 800.0f;
        char text[96];

        std::snprintf(text, sizeof(text), "%d episode%s", count_, count_ == 1 ? "" : "s");
        ui::text(list, fonts.display, fonts.display.font->fit(list_.name.empty() ? "Episodes" : list_.name.c_str(), 46, lw), lx, 168.0f, 46,
                 kWhite.with_alpha(fade));
        ui::text(list, fonts.regular, text, lx, 206.0f, 22, kWhite.with_alpha(0.55f * fade));

        // The rows stand still (they only scroll when the choice runs off the end); the bar is what moves
        const float list_top = 250.0f;
        list.push_clip(Rect{0.0f, 236.0f, 930.0f, row_h * 7.0f + 20.0f});
        for (int i = std::max(0, static_cast<int>(std::floor(ep_top_f_)) - 1);
             i <= std::min(count_ - 1, static_cast<int>(std::ceil(ep_top_f_)) + 7); ++i)
        {
            const float a = fade;
            const ::jf::Item &it = item(i);
            const Rect row{lx, list_top + (static_cast<float>(i) - ep_top_f_) * row_h + 4.0f, lw, row_h - 8.0f};
            list.rounded_rect(row, 24.0f, kWhite.with_alpha(0.05f * a));
            // the tick: green when watched, a ring when not, gold when it was left part-way
            const float tx = row.x + 44.0f, ty = row.cy();
            if (it.ready && it.watched)
                draw_check(list, tx, ty, 20.0f, a);
            else
            {
                list.circle(tx, ty, 20.0f, kWhite.with_alpha(0.22f * a));
                list.circle(tx, ty, 17.0f, Color{0.05f, 0.06f, 0.1f, 0.9f * a});
                if (it.ready && it.progress > 0.01f)
                    list.circle(tx, ty, 8.0f, kGold.with_alpha(a));
            }
            std::snprintf(text, sizeof(text), "%d", i + 1);
            ui::text(list, fonts.semibold, text, row.x + 88.0f, ty + 9.0f, 26, kWhite.with_alpha(0.5f * a));
            if (!it.ready)
            {
                list.rounded_rect(Rect{row.x + 150.0f, ty - 9.0f, 360.0f, 18.0f}, 9.0f, kWhite.with_alpha(0.08f * a));
                continue;
            }
            const std::string title(fonts.semibold.font->fit(it.title, 28, lw - 150.0f - (it.runtime[0] ? 150.0f : 40.0f)));
            ui::text(list, fonts.semibold, title.c_str(), row.x + 140.0f, ty + 10.0f, 28, kWhite.with_alpha(a));
            if (it.runtime[0])
                ui::text(list, fonts.regular, it.runtime, row.x + row.w - 28.0f, ty + 8.0f, 22, kWhite.with_alpha(0.6f * a),
                         gfx::Align::right);
            if (it.progress > 0.01f && !it.watched)
            {
                const Rect bar{row.x + 140.0f, row.y + row.h - 14.0f, row.w - 140.0f - 28.0f, 4.0f};
                list.rounded_rect(bar, 2.0f, kWhite.with_alpha(0.16f * a));
                list.rounded_rect({bar.x, bar.y, std::max(4.0f, bar.w * it.progress), 4.0f}, 2.0f, kGold.with_alpha(a));
            }
        }
        // the choosing bar, on top, gliding to the row that is chosen
        {
            const Rect bar{lx, list_top + (v - ep_top_f_) * row_h + 4.0f, lw, row_h - 8.0f};
            list.rounded_rect(bar, 24.0f, kWhite.with_alpha(0.13f * fade));
            list.bordered_rect(bar, 24.0f, kClear, 2.0f, kWhite.with_alpha(0.85f * fade * (1.0f - 0.85f * tabs)));
        }
        list.pop_clip();

        // the chosen episode
        const ::jf::Item &it = item(sel);
        const float px = 980.0f, pw = 844.0f;
        const float thumb_h = std::min(pw * (it.aspect > 0.2f ? it.aspect : 0.5625f), 440.0f);
        const float thumb_w = thumb_h / (it.aspect > 0.2f ? it.aspect : 0.5625f);
        const Rect thumb{px, 170.0f, std::min(thumb_w, pw), thumb_h};
        list.shadow({thumb.x, thumb.y + 14.0f, thumb.w, thumb.h}, 22.0f, 40.0f, kBlack.with_alpha(0.5f * fade));
        list.push_opacity(fade);
        list.image(it.cover, thumb, gfx::kFullUv, {1.0f, 1.0f, 1.0f, 1.0f}, 22.0f);
        list.bordered_rect(thumb, 22.0f, kClear, 1.5f, kWhite.with_alpha(0.2f));
        if (it.ready && it.watched)
            draw_check(list, thumb.x + 44.0f, thumb.y + 44.0f, 24.0f, 1.0f);
        if (it.ready && it.progress > 0.01f && !it.watched)
        {
            const Rect bar{thumb.x + 24.0f, thumb.y + thumb.h - 24.0f, thumb.w - 48.0f, 6.0f};
            list.rounded_rect(bar, 3.0f, kBlack.with_alpha(0.5f));
            list.rounded_rect({bar.x, bar.y, std::max(6.0f, bar.w * it.progress), 6.0f}, 3.0f, kGold);
        }
        float y = thumb.y + thumb.h + 62.0f;
        if (!it.ready)
        {
            list.rounded_rect(Rect{px, y - 30.0f, 420.0f, 30.0f}, 15.0f, kWhite.with_alpha(0.08f));
            list.pop_opacity();
            return;
        }
        ui::text(list, fonts.display, fonts.display.font->fit(it.title, 46, pw), px, y, 46, kWhite);
        y += 44.0f;
        std::string meta = "Episode " + std::to_string(sel + 1);
        if (it.runtime[0])
            meta += std::string("  \xC2\xB7  ") + it.runtime;
        if (it.year > 0)
            meta += "  \xC2\xB7  " + std::to_string(it.year);
        ui::text(list, fonts.regular, meta.c_str(), px, y, 24, kWhite.with_alpha(0.7f));
        // watched or not
        float sx = px + fonts.regular.measure(meta.c_str(), 24) + 28.0f;
        if (it.watched)
        {
            draw_check(list, sx + 14.0f, y - 8.0f, 14.0f, 1.0f);
            ui::text(list, fonts.semibold, "Watched", sx + 38.0f, y, 22, Color{0.5f, 0.9f, 0.62f, 1.0f});
        }
        else if (it.progress > 0.01f)
            ui::text(list, fonts.semibold, ("Left at " + clock_text(it.resumeSeconds)).c_str(), sx, y, 22, kGold);
        else
            ui::text(list, fonts.semibold, "Not watched", sx, y, 22, kWhite.with_alpha(0.5f));
        y += 46.0f;
        if (ep_wrap_gate_ != it.gateIndex)
        {
            ep_wrap_ = wrap_lines(fonts.regular, it.blurb, 26.0f, pw);
            ep_wrap_gate_ = it.gateIndex;
        }
        const int max_lines = static_cast<int>((990.0f - y) / 36.0f);
        for (int k = 0; k < static_cast<int>(ep_wrap_.size()) && k < max_lines; ++k)
        {
            std::string line = ep_wrap_[static_cast<std::size_t>(k)];
            if (k == max_lines - 1 && static_cast<int>(ep_wrap_.size()) > max_lines)
                line += " ...";
            ui::text(list, fonts.regular, line.c_str(), px, y + 36.0f * static_cast<float>(k), 26, kWhite.with_alpha(0.86f));
        }
        list.pop_opacity();
    }

    void draw_row(gfx::DrawList &list, float lift) const
    {
        // Far covers first, so nearer ones overlap them.
        struct Entry
        {
            int index;
            float away;
        };
        Entry order[2 * static_cast<int>(kReach) + 3];
        int entries = 0;
        const int first = std::max(0, static_cast<int>(std::floor(wheel_.value - kReach)));
        const int end = std::min(count_ - 1, static_cast<int>(std::ceil(wheel_.value + kReach)));
        for (int i = first; i <= end && entries < static_cast<int>(std::size(order)); ++i)
            order[entries++] = {i, std::fabs(static_cast<float>(i) - wheel_.value)};
        std::sort(order, order + entries,
                  [](const Entry &a, const Entry &b) { return a.away > b.away; });

        // The centre's light and ring wait, like the caption, for a slow wheel.
        const float focus = settled() * calm_.value * (1.0f - lift);
        for (int i = 0; i < entries; ++i)
        {
            const int index = order[i].index;
            if (index == opened_ && lift > 0.01f)
                continue; // drawn above the scrim
            draw_cover(list, index, pose(index), index == centre_ ? focus : 0.0f);
        }

        const Pose centre = pose(centre_);
        if (cue_flash_ > 0.01f && lift < 0.01f)
        {
            // The wheel has the focus back: a ring spreads from the cover and fades, and its light flares
            const float spread = 1.0f - cue_flash_;
            const Color accent = item(centre_).accent;
            list.glow(centre.rect.inset(10.0f), kRadius, 70.0f, accent.with_alpha(0.55f * cue_flash_));
            list.bordered_rect(centre.rect.inset(-7.0f - 34.0f * spread), kRadius + 7.0f + 34.0f * spread, kClear, 3.0f,
                               gfx::mix(accent, kWhite, 0.5f).with_alpha(0.85f * cue_flash_));
        }
        if (focus <= 0.01f)
            return;
        const float in = tween::stagger(age_, 0, 0.055f, 0.55f);
        // The focus ring: it belongs to the centre of the row, and appears on
        // whichever cover comes to rest there. The tabs take it while they have the focus.
        list.bordered_rect(centre.rect.inset(-7.0f), kRadius + 7.0f, kClear, 3.0f,
                           kWhite.with_alpha(0.95f * focus * in * (1.0f - 0.85f * tween::clamp01(tabs_amount_))));
    }

    // Five stars filled up to `rating`; the partial one is a clipped full star.
    static void draw_stars(gfx::DrawList &list, float x, float cy, float rating, float radius,
                           float pitch, float alpha)
    {
        for (int i = 0; i < 5; ++i)
        {
            const float cx = x + radius + static_cast<float>(i) * pitch;
            const float fill = tween::clamp01(rating - static_cast<float>(i));
            list.star(cx, cy, radius, kWhite.with_alpha(0.2f * alpha));
            if (fill >= 0.99f)
            {
                list.star(cx, cy, radius, kGold.with_alpha(alpha));
            }
            else if (fill > 0.01f)
            {
                list.push_clip({cx - radius, cy - radius, 2.0f * radius * fill, 2.0f * radius});
                list.star(cx, cy, radius, kGold.with_alpha(alpha));
                list.pop_clip();
            }
        }
    }

    // Title and facts of the centred cover. There is one caption, always for
    // the nearest cover; it is opaque only while the wheel rests, so passing
    // titles fade through instead of flickering.
    void draw_caption(gfx::DrawList &list, float lift) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float in = tween::stagger(age_, 5, 0.08f, 0.5f);
        const float shown = in * (1.0f - tween::clamp01(lift * 2.5f));
        // During a fast spin a counter takes the caption's place: digits in a
        // monospaced face can change every frame without flickering.
        const float blur = shown * (1.0f - calm_.value);
        if (blur > 0.01f)
        {
            char place[16];
            std::snprintf(place, sizeof(place), "%02d", centre_ + 1);
            ui::text(list, fonts.mono, place, kCentreX + dx_, kTitleY + dy_, 64, kWhite.with_alpha(0.5f * blur),
                     gfx::Align::center);
            std::snprintf(place, sizeof(place), "OF %d", count_);
            ui::text(list, fonts.semibold, place, kCentreX + dx_, kMetaY - 4.0f + dy_, 18,
                     kWhite.with_alpha(0.4f * blur), gfx::Align::center, 4.0f);
        }
        const float alpha = shown * settled() * calm_.value;
        if (alpha <= 0.01f)
            return;
        const ::jf::Item &it = item(centre_);
        if (!it.ready)
            return; // the server has not said what this one is yet
        // The caption belongs to its cover: it stays under it, so a step
        // carries the old title out one side as the new one arrives from the
        // other, and a row stretched past its end takes its caption along.
        const float x = reduced() ? kCentreX + dx_ : pose(centre_).rect.cx();
        const float rise = reduced() ? 0.0f : 14.0f * (1.0f - in);
        const float title_y = kTitleY + rise + dy_;
        const float meta_y = kMetaY + rise + dy_;
        char text[160];

        list.push_opacity(alpha);
        ui::text(list, fonts.display, fonts.display.font->fit(it.title, 64, 1200), x, title_y, 64, kWhite,
                 gfx::Align::center);

        // The kind and the length, then the rating when the server gave one, centred as one line.
        int used = std::snprintf(text, sizeof(text), "%s", it.genre);
        if (it.runtime[0] && used > 0 && used < static_cast<int>(sizeof(text)))
            used += std::snprintf(text + used, sizeof(text) - static_cast<std::size_t>(used),
                                  "  \xC2\xB7  %s", it.runtime);
        if (it.year > 0 && used > 0 && used < static_cast<int>(sizeof(text)))
            std::snprintf(text + used, sizeof(text) - static_cast<std::size_t>(used), "  \xC2\xB7  %d", it.year);
        const bool rated = it.rating > 0.0f;
        char score[8];
        std::snprintf(score, sizeof(score), "%.1f", static_cast<double>(it.rating));
        constexpr float kStar = 11.0f;
        constexpr float kStarPitch = 27.0f;
        const float facts = fonts.regular.measure(text, 26);
        const float stars = 4.0f * kStarPitch + 2.0f * kStar;
        const float total =
            rated ? facts + 36.0f + stars + 14.0f + fonts.semibold.measure(score, 26) : facts;
        float cursor = x - total * 0.5f;
        ui::text(list, fonts.regular, text, cursor, meta_y, 26, kWhite.with_alpha(0.74f));
        if (rated)
        {
            cursor += facts + 36.0f;
            draw_stars(list, cursor, meta_y - 9.0f, it.rating, kStar, kStarPitch, alpha);
            cursor += stars + 14.0f;
            ui::text(list, fonts.semibold, score, cursor, meta_y, 26, kWhite);
        }
        list.pop_opacity();
    }

    // The big letter that says where an L1 / R1 jump landed.
    void draw_letter(gfx::DrawList &list) const
    {
        if (letter_flash_ <= 0.01f)
            return;
        const ui::Fonts &fonts = context_.fonts;
        const char text[2] = {letter_shown_, 0};
        const float alpha = tween::clamp01(letter_flash_ * 1.6f);
        ui::text(list, fonts.display, text, kLetterX + dx_, kTitleY + dy_, kLetterSize, kWhite.with_alpha(0.9f * alpha));
    }

    // Where the wheel is within the collection. The thumb is drawn from the
    // same position as the covers, so it shows the coasting and the stretch.
    // A long library has the letters along the bar instead of one dot a title.
    void draw_scrub_bar(gfx::DrawList &list, float lift) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float in = tween::stagger(age_, 7, 0.08f, 0.5f);
        const float bar_y = kBarY + dy_;
        const float bx = kBarX + dx_;
        const float span = std::max(1.0f, last());
        list.push_opacity(in * (1.0f - tween::clamp01(lift * 2.5f)));
        list.rounded_rect({bx, bar_y - 2.0f, kBarW, 4.0f}, 2.0f, kWhite.with_alpha(0.14f));
        if (count_ <= kDotsMax)
        {
            for (int i = 0; i < count_; ++i)
            {
                const float x = bx + kBarW * static_cast<float>(i) / span;
                if (item(i).favorite)
                    list.circle(x, bar_y, 4.5f, kGold);
                else
                    list.circle(x, bar_y, 2.5f, kWhite.with_alpha(0.36f));
            }
        }
        else if (list_.lettersKnown)
        {
            const int here = bucket_of(centre_);
            for (int k = 0; k < 27; ++k)
            {
                const int start = list_.letters[k];
                const int next = k < 26 ? list_.letters[k + 1] : count_;
                if (start >= count_ || next <= start)
                    continue; // no titles under this letter
                const float x = bx + kBarW * static_cast<float>(start) / span;
                const bool now = k == here;
                list.circle(x, bar_y, now ? 4.0f : 2.5f, kWhite.with_alpha(now ? 0.95f : 0.36f));
                const char label[2] = {letter_char(k), 0};
                ui::text(list, fonts.semibold, label, x, bar_y + 34.0f, now ? 20 : 15,
                         kWhite.with_alpha(now ? 0.95f : 0.45f), gfx::Align::center);
            }
        }

        // Past an end the thumb is squeezed against it instead of leaving.
        const float over = beyond();
        const float squeeze = 1.0f - std::min(0.55f, std::fabs(over) * 1.5f);
        const float width = 44.0f * squeeze;
        const float at = bx + kBarW * tween::clamp01(wheel_.value / span);
        float left = at - width * 0.5f;
        if (over < 0.0f)
            left = at - 22.0f;
        else if (over > 0.0f)
            left = at + 22.0f - width;
        const Rect thumb{left, bar_y - 8.0f, width, 16.0f};
        const Color accent = palette_[4].value();
        list.glow(thumb, 8.0f, 16.0f, accent.with_alpha(0.45f));
        list.rounded_rect(thumb, 8.0f, gfx::mix(accent, kWhite, 0.35f));

        // The place in numbers; during a fast spin the large counter says it.
        char text[16];
        std::snprintf(text, sizeof(text), "%02d", centre_ + 1);
        ui::text(list, fonts.mono, text, bx - 30.0f, bar_y + 8.0f, 22,
                 kWhite.with_alpha(0.9f * calm_.value), gfx::Align::right);
        std::snprintf(text, sizeof(text), "%02d", count_);
        ui::text(list, fonts.mono, text, bx + kBarW + 30.0f, bar_y + 8.0f, 22,
                 kWhite.with_alpha(0.5f * calm_.value));
        list.pop_opacity();
    }

    // A rounded button of the film's page: a small caption over a value
    void draw_pill(gfx::DrawList &list, const Rect &r, bool focus, bool enabled, const char *caption,
                   const std::string &value, const Color &accent, float breath) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float a = enabled ? 1.0f : 0.5f;
        const float round = r.h * 0.5f;
        if (focus)
        {
            list.glow(r, round, 22.0f, accent.with_alpha((0.26f + 0.2f * breath) * a));
            list.rounded_rect(r, round, kWhite.with_alpha(a));
        }
        else
        {
            list.rounded_rect(r, round, kWhite.with_alpha(0.11f * a));
            list.bordered_rect(r, round, kClear, 1.5f, kWhite.with_alpha(0.16f * a));
        }
        const Color ink = focus ? kInk : kWhite;
        ui::text(list, fonts.semibold, caption, r.x + 26.0f, r.y + 24.0f, 13, ink.with_alpha((focus ? 0.55f : 0.5f) * a),
                 gfx::Align::left, 3.0f);
        const std::string fitted(fonts.semibold.font->fit(value.c_str(), 24, r.w - 52.0f));
        ui::text(list, fonts.semibold, fitted.c_str(), r.x + 26.0f, r.y + r.h - 14.0f, 24, ink.with_alpha(a));
    }

    // A small tag (4K, HDR ...); returns its width
    float draw_chip(gfx::DrawList &list, float x, float cy, const char *label, const Color &fill, const Color &ink) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float w = fonts.semibold.measure(label, 17) + 24.0f;
        list.rounded_rect({x, cy - 17.0f, w, 34.0f}, 9.0f, fill);
        ui::text(list, fonts.semibold, label, x + 12.0f, cy + 6.0f, 17, ink);
        return w;
    }

    static std::string clock_text(float seconds)
    {
        const int total = static_cast<int>(seconds + 0.5f);
        char buffer[24];
        if (total >= 3600)
            std::snprintf(buffer, sizeof(buffer), "%d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
        else
            std::snprintf(buffer, sizeof(buffer), "%d:%02d", total / 60, total % 60);
        return buffer;
    }

    // The QR code: a white card with the modules as merged runs
    void draw_qr(gfx::DrawList &list, const Rect &box, const std::string &url, float alpha) const
    {
        if (url.empty() || alpha <= 0.01f)
            return;
        if (qr_text_ != url)
        {
            qr_code_ = ::jfqr::encode(url);
            qr_text_ = url;
        }
        if (qr_code_.size <= 0)
            return;
        list.push_opacity(alpha);
        list.shadow({box.x, box.y + 8.0f, box.w, box.h}, 18.0f, 24.0f, kBlack.with_alpha(0.4f));
        list.rounded_rect(box, 18.0f, kWhite);
        const int n = qr_code_.size;
        const int px = std::max(1, static_cast<int>(std::floor(box.w / static_cast<float>(n + 6))));
        const float side = static_cast<float>(px * n);
        const float ox = box.x + (box.w - side) * 0.5f;
        const float oy = box.y + (box.h - side) * 0.5f;
        for (int row = 0; row < n; ++row)
        {
            int col = 0;
            while (col < n)
            {
                if (!qr_code_.dark(col, row))
                {
                    ++col;
                    continue;
                }
                int end = col;
                while (end < n && qr_code_.dark(end, row))
                    ++end;
                list.rounded_rect({ox + static_cast<float>(col * px), oy + static_cast<float>(row * px),
                                   static_cast<float>((end - col) * px), static_cast<float>(px)},
                                  0.0f, kBlack);
                col = end;
            }
        }
        list.pop_opacity();
    }

    // Bottom right of the wheel, clear of the row and the hints: scan it for the title's page
    void draw_corner_qr(gfx::DrawList &list, float lift) const
    {
        if (count_ <= 0 || stale() || lift > 0.02f)
            return;
        const ::jf::Item &it = item(current());
        if (it.qrUrl.empty())
            return;
        const float alpha = settled() * calm_.value * tween::stagger(age_, 8, 0.08f, 0.5f) * (1.0f - tabs_amount_);
        if (alpha <= 0.01f)
            return;
        const ui::Fonts &fonts = context_.fonts;
        const Rect box{1736.0f, 856.0f, 128.0f, 128.0f};
        ui::text(list, fonts.semibold, "SCAN", box.cx(), box.y - 12.0f, 13, kWhite.with_alpha(0.55f * alpha),
                 gfx::Align::center, 3.0f);
        draw_qr(list, box, it.qrUrl, alpha);
    }

    void draw_details(gfx::DrawList &list, std::uint32_t glass, float lift) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const ::jf::Item &it = item(opened_);
        const bool have = det_state_ == 1;
        char text[96];

        // The panel waits until the cover has moved out of its way, and on
        // the way back it is gone before the cover returns.
        const float appear_panel = tween::clamp01((lift - 0.45f) / 0.5f);
        Rect panel = kPanel;
        if (!reduced())
            panel.x += 80.0f * (1.0f - tween::cubic_out(appear_panel));
        list.push_opacity(appear_panel);
        list.shadow({panel.x, panel.y + 22, panel.w, panel.h}, kPanelRadius, 60,
                    kBlack.with_alpha(0.5f));
        // Frosted panel: the blurred screen, a tint in the cover's own dark
        // tone, then a hairline of light.
        list.glass(glass, panel, kPanelRadius, kWhite);
        const Color tint = gfx::mix(gfx::mix(it.dark, it.mid, 0.18f), kInk, 0.35f);
        list.rounded_rect(panel, kPanelRadius, tint.with_alpha(0.95f));
        list.bordered_rect(panel, kPanelRadius, kClear, 1.5f, kWhite.with_alpha(0.2f));

        const float x = panel.x + kPanelPad;
        const float width = panel.w - 2.0f * kPanelPad;
        const float breath = reduced() ? 0.5f : ui::breathe(clock_);
        ui::text(list, fonts.semibold, ui::upper(it.genre), x, panel.y + 62, 20, it.accent,
                 gfx::Align::left, 4.0f);

        // The favourite state, top right beside the title: Triangle toggles it
        {
            const bool starred = it.favorite;
            const float right = x + width;
            // the QR code: small, at the corner of the info box, for the title's page
            const float qr = 112.0f;
            draw_qr(list, {right - qr, panel.y + 32.0f, qr, qr}, it.qrUrl, 1.0f);
            // the star, filled when it is a favourite and an outline when it is not
            list.star(right - qr - 44.0f, panel.y + 90.0f, 17.0f + 8.0f * star_.value,
                      starred ? kGold : kWhite.with_alpha(0.7f), starred ? 0.0f : 2.5f);
        }
        ui::text(list, fonts.display, fonts.display.font->fit(it.title, 56, width - 230.0f), x - 2,
                 panel.y + 128, 56, kWhite);

        // Year and length, then what the file is: size of the picture, its range, its sound
        if (it.year > 0 && it.runtime[0])
            std::snprintf(text, sizeof(text), "%d  \xC2\xB7  %s", it.year, it.runtime);
        else if (it.year > 0)
            std::snprintf(text, sizeof(text), "%d", it.year);
        else
            std::snprintf(text, sizeof(text), "%s", it.runtime);
        float cx = x;
        const float meta_y = panel.y + 172;
        if (text[0])
        {
            ui::text(list, fonts.regular, text, x, meta_y, 24, kWhite.with_alpha(0.72f));
            cx += fonts.regular.measure(text, 24) + 22.0f;
        }
        if (have && det_.official[0])
            cx += draw_chip(list, cx, meta_y - 8.0f, det_.official, kWhite.with_alpha(0.16f), kWhite) + 10.0f;
        if (have && it.badges & ::jf::Item::kBadge4K)
            cx += draw_chip(list, cx, meta_y - 8.0f, "4K", kWhite.with_alpha(0.9f), kInk) + 10.0f;
        else if (have && it.badges & ::jf::Item::kBadgeHD)
            cx += draw_chip(list, cx, meta_y - 8.0f, "HD", kWhite.with_alpha(0.9f), kInk) + 10.0f;
        if (have && it.badges & ::jf::Item::kBadgeDV)
            cx += draw_chip(list, cx, meta_y - 8.0f, "DOLBY VISION", kDolby, kInk) + 10.0f;
        else if (have && it.badges & ::jf::Item::kBadgeHDR)
            cx += draw_chip(list, cx, meta_y - 8.0f, det_.range[0] ? upper_text(det_.range).c_str() : "HDR", kGold, kInk) + 10.0f;
        if (have && it.badges & ::jf::Item::kBadgeAtmos)
            cx += draw_chip(list, cx, meta_y - 8.0f, "ATMOS", kWhite.with_alpha(0.16f), kWhite) + 10.0f;

        // The buttons: resume, from start, audio, subtitles
        const float press = launch_.running ? tween::ping(launch_.progress()) : 0.0f;
        const bool second = resumable();
        const bool can = it.playable;
        std::string audio_value = "Loading \xE2\x80\xA6";
        std::string sub_value = "Loading \xE2\x80\xA6";
        if (!can)
        {
            audio_value = sub_value = "-";
        }
        else if (det_state_ == -1)
        {
            audio_value = sub_value = "Unavailable";
        }
        else if (have)
        {
            const JfGateTrack *a = find_track(effective_audio());
            audio_value = a ? track_name(*a) : (count_tracks(1) > 0 ? "Automatic" : "None");
            const JfGateTrack *s = find_track(effective_sub());
            sub_value = s ? track_name(*s) : (count_tracks(2) > 0 ? "Off" : "None");
        }
        std::string play_value = !can ? (it.kindRaw == "Series" ? "Seasons" : it.kindRaw == "Season" ? "Episodes" : "Open") : launch_.running ? "Starting \xE2\x80\xA6"
                                 : second ? "Resume " + clock_text(it.resumeSeconds) : std::string("Play");
        const float row_y = panel.y + 196;
        float widths[4] = {second ? 262.0f : 300.0f, second ? 206.0f : 0.0f, second ? 214.0f : 322.0f, second ? 214.0f : 322.0f};
        float bx = x;
        const float gap = second ? 14.0f : 14.0f;
        for (int id = 0; id < 4; ++id)
        {
            if (widths[id] <= 0.0f)
                continue;
            Rect r{bx, row_y, widths[id], 64.0f};
            bx += widths[id] + gap;
            if (id <= 1 && focus_ == id && !reduced())
                r = r.inset(-4.0f * press);
            const char *caption = id == 0 ? (!can ? "BROWSE" : second ? "CONTINUE" : "WATCH") : id == 1 ? "RESTART" : id == 2 ? "AUDIO" : "SUBTITLES";
            const std::string value = id == 0 ? play_value : id == 1 ? std::string("From start") : id == 2 ? audio_value : sub_value;
            const bool enabled = id == 0 ? true : can;
            draw_pill(list, r, focus_ == id, enabled, caption, value, it.accent, breath);
        }

        // The summary: it scrolls (right stick, up and down, L1 / R1), and carries the credits at its end
        const Rect view{x, panel.y + 288.0f, width, 236.0f};
        if (wrap_gate_ != it.gateIndex || read_have_ != have)
        {
            std::string full = it.blurb;
            if (have)
            {
                if (det_.tagline[0])
                    full = std::string(det_.tagline) + "\n\n" + full;
                if (det_.directors[0])
                    full += std::string("\n\nDirected by ") + det_.directors;
                if (det_.studio[0])
                    full += std::string("\n\nStudio: ") + det_.studio;
                if (det_.cast[0])
                    full += std::string("\n\nWith ") + det_.cast;
            }
            wrap_lines_ = wrap_lines(fonts.regular, full.c_str(), 25.0f, width - 30.0f);
            wrap_gate_ = it.gateIndex;
            read_have_ = have;
        }
        constexpr float kStep = 36.0f;
        const float content = static_cast<float>(wrap_lines_.size()) * kStep + 10.0f;
        summary_view_h_ = view.h;
        summary_max_ = std::max(0.0f, content - view.h);
        list.push_clip(view);
        for (std::size_t i = 0; i < wrap_lines_.size(); ++i)
        {
            const float y = view.y + 28.0f + kStep * static_cast<float>(i) - scroll_;
            if (y < view.y - 6.0f || y > view.y + view.h + kStep)
                continue;
            ui::text(list, fonts.regular, wrap_lines_[i].c_str(), view.x, y, 25, kWhite.with_alpha(0.88f));
        }
        list.pop_clip();
        if (summary_max_ > 1.0f)
        {
            const Rect track{view.x + view.w - 6.0f, view.y + 4.0f, 6.0f, view.h - 8.0f};
            list.rounded_rect(track, 3.0f, kWhite.with_alpha(0.12f));
            const float th = std::max(36.0f, track.h * view.h / content);
            const float ty = track.y + (track.h - th) * tween::clamp01(scroll_ / summary_max_);
            list.rounded_rect({track.x, ty, 6.0f, th}, 3.0f, kWhite.with_alpha(0.6f));
        }

        // Four facts, arriving one after the other once the panel is there.
        constexpr float kTileGap = 16.0f;
        const float tile_w = (width - 3.0f * kTileGap) / 4.0f;
        const bool show_size = have && it.playable && det_.file_size > 0;
        const char *labels[] = {"TYPE", show_size ? "FILE SIZE" : "LENGTH", "RATING", "WATCHED"};
        for (int i = 0; i < 4; ++i)
        {
            const float appear = tween::stagger(appear_panel, i, 0.1f, 0.6f);
            const Rect tile{x + static_cast<float>(i) * (tile_w + kTileGap),
                            panel.y + 548 + (reduced() ? 0.0f : 22.0f * (1.0f - appear)), tile_w, 96};
            list.push_opacity(appear);
            list.rounded_rect(tile, 22, kWhite.with_alpha(0.08f));
            ui::text(list, fonts.semibold, labels[i], tile.x + 22, tile.y + 32, 14,
                     kWhite.with_alpha(0.55f), gfx::Align::left, 3.0f);
            switch (i)
            {
            case 0:
                std::snprintf(text, sizeof(text), "%s", it.genre);
                break;
            case 1:
                if (show_size)
                {
                    const double bytes = static_cast<double>(det_.file_size);
                    if (bytes >= 1.0e9)
                        std::snprintf(text, sizeof(text), "%.1f GB", bytes / 1.0e9);
                    else
                        std::snprintf(text, sizeof(text), "%d MB", static_cast<int>(bytes / 1.0e6 + 0.5));
                }
                else
                    std::snprintf(text, sizeof(text), "%s", it.runtime[0] ? it.runtime : "-");
                break;
            case 2:
                if (it.rating > 0.0f)
                {
                    std::snprintf(text, sizeof(text), "%.1f", static_cast<double>(it.rating));
                    draw_stars(list, tile.x + 88, tile.y + 60, it.rating, 7.0f, 17.0f, appear);
                }
                else
                    std::snprintf(text, sizeof(text), "-");
                break;
            default:
            {
                if (it.progress > 0.0f)
                    std::snprintf(text, sizeof(text), "%d%%",
                                  static_cast<int>(it.progress * 100.0f + 0.5f));
                else
                    std::snprintf(text, sizeof(text), "New");
                const Rect bar{tile.x + 22, tile.y + 84, tile.w - 44, 4};
                list.rounded_rect(bar, 2, kWhite.with_alpha(0.16f));
                if (it.progress > 0.0f)
                    list.rounded_rect(
                        {bar.x, bar.y, std::max(4.0f, bar.w * it.progress * appear), 4}, 2,
                        it.accent);
                break;
            }
            }
            const std::string fitted(fonts.semibold.font->fit(text, 32, tile.w - 44.0f));
            ui::text(list, fonts.semibold, fitted.c_str(), tile.x + 22, tile.y + 70, 32, kWhite);
            list.pop_opacity();
        }

        // What critics and viewers think
        float rx = x;
        const float ry = panel.y + 684;
        if (have && det_.critic > 0.0f)
        {
            list.circle(rx + 14, ry, 12.0f, kTomato);
            std::snprintf(text, sizeof(text), "%d%%", static_cast<int>(det_.critic + 0.5f));
            ui::text(list, fonts.semibold, text, rx + 36, ry + 9, 26, kWhite);
            const float w = fonts.semibold.measure(text, 26);
            ui::text(list, fonts.regular, "Critics", rx + 36 + w + 10, ry + 8, 20, kWhite.with_alpha(0.55f));
            rx += 36 + w + 10 + fonts.regular.measure("Critics", 20) + 36;
        }
        if (have && det_.community > 0.0f)
        {
            list.star(rx + 14, ry, 13.0f, kGold);
            std::snprintf(text, sizeof(text), "%.1f", static_cast<double>(det_.community));
            ui::text(list, fonts.semibold, text, rx + 36, ry + 9, 26, kWhite);
            const float w = fonts.semibold.measure(text, 26);
            ui::text(list, fonts.regular, "Viewers", rx + 36 + w + 10, ry + 8, 20, kWhite.with_alpha(0.55f));
            rx += 36 + w + 10 + fonts.regular.measure("Viewers", 20) + 36;
        }
        if (have && det_.studio[0] && x + width - rx > 160.0f)
        {
            const std::string studio(fonts.regular.font->fit((std::string("Studio  ") + det_.studio).c_str(), 22, x + width - rx - 4.0f));
            ui::text(list, fonts.regular, studio.c_str(), rx, ry + 8, 22, kWhite.with_alpha(0.7f));
        }
        list.pop_opacity();

        if (modal_amt_ > 0.004f)
            draw_modal(list, panel, it);
    }

    // ---- the track pickers, over the film's panel ----
    void draw_modal(gfx::DrawList &list, const Rect &panel, const ::jf::Item &it) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float e = modal_amt_;
        Rect m{panel.x + 24.0f, panel.y + 24.0f, panel.w - 48.0f, panel.h - 48.0f};
        if (!reduced())
            m.y += 18.0f * (1.0f - tween::cubic_out(e));
        list.push_opacity(e);
        list.shadow({m.x, m.y + 14.0f, m.w, m.h}, 34.0f, 40.0f, kBlack.with_alpha(0.5f));
        list.rounded_rect(m, 34.0f, Color{0.035f, 0.04f, 0.075f, 0.97f});
        list.bordered_rect(m, 34.0f, kClear, 1.5f, kWhite.with_alpha(0.2f));
        const float x = m.x + 56.0f;
        const float width = m.w - 112.0f;
        const bool sound = modal_shown_ == Modal::audio;
        ui::text(list, fonts.display, sound ? "Audio" : "Subtitles", x, m.y + 82.0f, 44, kWhite);
        const int total = static_cast<int>(rows_.size());
        constexpr int kRows = 7;
        const int first = std::clamp(cursor_ - 3, 0, std::max(0, total - kRows));
        const int current = sound ? effective_audio() : effective_sub();
        for (int r = 0; r < kRows && first + r < total; ++r)
        {
            const PickRow &row = rows_[static_cast<std::size_t>(first + r)];
            const Rect box{x, m.y + 126.0f + 70.0f * static_cast<float>(r), width, 62.0f};
            const bool on = first + r == cursor_;
            const float a = row.usable ? 1.0f : 0.45f;
            if (on)
            {
                list.glow(box, 31.0f, 18.0f, it.accent.with_alpha(0.25f * a));
                list.rounded_rect(box, 31.0f, kWhite.with_alpha(a));
            }
            else
                list.rounded_rect(box, 31.0f, kWhite.with_alpha(0.07f));
            const Color ink = on ? kInk : kWhite;
            const std::string name(fonts.semibold.font->fit(row.name.c_str(), 25, width - 130.0f));
            ui::text(list, fonts.semibold, name.c_str(), box.x + 32.0f, box.y + 28.0f, 25, ink.with_alpha(a));
            const std::string note(fonts.regular.font->fit(row.note.c_str(), 17, width - 130.0f));
            ui::text(list, fonts.regular, note.c_str(), box.x + 32.0f, box.y + 51.0f, 17, ink.with_alpha(0.6f * a));
            if (row.stream == current)
            {
                list.circle(box.x + box.w - 40.0f, box.cy(), 11.0f, on ? it.accent : kWhite.with_alpha(0.85f));
                list.circle(box.x + box.w - 40.0f, box.cy(), 4.5f, kInk);
            }
        }
        list.pop_opacity();
    }

    // When the kit has no touch pad glyph, the search hint is drawn by hand at the left
    void draw_touch_fallback(gfx::DrawList &list, float alpha) const
    {
#if !JFW_TOUCH_GLYPH
        const ui::Fonts &fonts = context_.fonts;
        list.push_opacity(alpha);
        list.rounded_rect({96.0f, 1000.0f, 64.0f, 34.0f}, 10.0f, kWhite.with_alpha(0.85f));
        list.rounded_rect({108.0f, 1008.0f, 40.0f, 18.0f}, 4.0f, kInk.with_alpha(0.55f));
        ui::text(list, fonts.semibold, "Search", 176.0f, 1024.0f, 22, kWhite.with_alpha(0.9f));
        list.pop_opacity();
#else
        (void)list;
        (void)alpha;
#endif
    }

    void draw_hints(gfx::DrawList &list, float lift) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const ui::GlyphStyle style = ui::GlyphStyle::dark();
        if (open_)
        {
            list.push_opacity(tween::clamp01(lift * 2.0f));
            if (modal_ != Modal::none)
            {
                const ui::Hint hints[] = {{ui::Button::dpad, "Choose"}, {ui::Button::cross, "Select"},
                                          {ui::Button::circle, "Close"}};
                ui::draw_hints(list, fonts, style, hints, 3, 1824, true);
            }
            else
            {
                const ui::Hint hints[] = {{ui::Button::dpad, "Choose  /  Scroll"},
                                          {ui::Button::cross, focus_ <= 1 && item(opened_).playable ? "Play" : "Open"},
                                          {ui::Button::triangle, "Favorite"},
                                          {ui::Button::options, "Cast"},
                                          {ui::Button::circle, "Back"}};
                if (cast_available())
                    ui::draw_hints(list, fonts, style, hints, 5, 1824, true);
                else
                {
                    const ui::Hint plain[] = {hints[0], hints[1], hints[2], hints[4]};
                    ui::draw_hints(list, fonts, style, plain, 4, 1824, true);
                }
            }
            list.pop_opacity();
        }
        else if (tabs_focus_)
        {
            const ui::Hint hints[] = {{ui::Button::dpad, "Library"},
                                      {ui::Button::cross, list_.kind == 5 ? "Search" : "Back to the wheel"},
                                      {ui::Button::touchpad, "Search"},
            };
            list.push_opacity(tween::stagger(age_, 9, 0.08f, 0.5f) * (1.0f - lift));
            ui::draw_hints(list, fonts, style, hints, static_cast<int>(std::size(hints)), 1824, true);
            draw_touch_fallback(list, tween::stagger(age_, 9, 0.08f, 0.5f) * (1.0f - lift));
            list.pop_opacity();
        }
        else if (episode_mode())
        {
            const ui::Hint hints[] = {{ui::Button::dpad, "Choose"},
                                      {ui::Button::cross, item(current()).resumeSeconds > 1.0f ? "Resume" : "Play"},
                                      {ui::Button::options, "Details"},
                                      {ui::Button::triangle, "Favorite"},
                                      {ui::Button::circle, "Back"}};
            list.push_opacity(tween::stagger(age_, 9, 0.08f, 0.5f) * (1.0f - lift));
            ui::draw_hints(list, fonts, style, hints, static_cast<int>(std::size(hints)), 1824, true);
            list.pop_opacity();
        }
        else
        {
            const ui::Hint hints[] = {{ui::Button::left_stick, "Spin"},
                                      {ui::Button::dpad, "Step"},
                                      {ui::Button::l1, "Letter", ui::Button::r1},
                                      {ui::Button::l2, "Jump", ui::Button::r2},
                                      {ui::Button::cross, "Open"},
                                      {ui::Button::triangle, "Favorite"},
                                      {ui::Button::square, "A-Z"},
                                      {ui::Button::options, "Sort"},
                                      {ui::Button::touchpad, "Search"},
            };
            list.push_opacity(tween::stagger(age_, 9, 0.08f, 0.5f) * (1.0f - lift) * (overlay_ ? 0.0f : 1.0f));
            ui::draw_hints(list, fonts, style, hints, static_cast<int>(std::size(hints)), 1824, true);
            draw_touch_fallback(list, 1.0f);
            list.pop_opacity();
        }
    }

    app::Context &context_;
    ::jf::List &list_;
    int epoch_ = 0;
    int count_ = 0;
    float age_ = 0.0f;   // seconds since enter(): drives the entrance
    float clock_ = 0.0f; // free-running time for idle motion

    // Set by draw: how far down and how faded this wheel is drawn while it slides
    mutable float dy_ = 0.0f;
    mutable float dx_ = 0.0f; // sideways: the wheels slide left or right between categories
    mutable bool tabs_focus_ = false;
    mutable float tabs_amount_ = 0.0f;
    mutable bool overlay_ = false;

    // The hop when the wheel gets the focus back from the tabs
    float cue_t_ = -1.0f; // seconds since it started; -1 when it is not running
    float cue_y_ = 0.0f;
    float cue_flash_ = 0.0f;

    // The wheel. value is the position in covers (3.5 is half-way between the
    // fourth and fifth), velocity is covers per second. In Drive::snap it is
    // a spring toward target_; otherwise update() moves it directly.
    tween::Bounce wheel_;
    Drive drive_ = Drive::snap;
    int target_ = 0;
    float stick_ = 0.0f;       // the stick's deflection while scrubbing
    int centre_ = 0;           // the cover nearest the position, as last announced
    bool quiet_ = true;        // the current movement already had its sound
    bool against_end_ = false; // the wheel is past an end (refuse once)
    float tick_wait_ = 0.0f;   // seconds until the next tick may play
    float letter_flash_ = 0.0f; // 1 right after an L1 / R1 jump, then fades
    char letter_shown_ = '#';

    tween::Spring calm_; // 1 while the wheel is slow enough to read a caption
    ui::SpringColor palette_[5];
    ui::Pulse star_;

    // The film's page
    Modal modal_ = Modal::none;
    Modal modal_shown_ = Modal::none; // what the fading page was
    float modal_amt_ = 0.0f;
    int focus_ = 0; // 0 play, 1 from start, 2 audio, 3 subtitles, 4 summary, 5 QR
    JfGateDetails det_{};
    int det_state_ = 0; // 0 asked, 1 here, -1 the server did not answer
    float det_wait_ = 0.0f;
    float badge_wait_ = 0.0f;
    int pick_gate_ = -1; // the title the choices below belong to
    int pick_audio_ = -1;
    int pick_sub_ = kSubDefault;
    int cursor_ = 0;
    std::vector<PickRow> rows_;
    float scroll_ = 0.0f;
    float scroll_target_ = 0.0f;
    mutable float summary_view_h_ = 236.0f;
    mutable float summary_max_ = 0.0f;
    mutable std::vector<std::string> wrap_lines_;
    mutable std::vector<std::string> ep_wrap_; // the chosen episode's summary, wrapped
    mutable int ep_wrap_gate_ = -1;
    bool ep_placed_ = false; // the episode list has found where you were up to
    int ep_top_ = 0;         // the first of the seven rows on show
    mutable float ep_top_f_ = 0.0f; // ... as it scrolls
    float ep_wait_ = 0.0f;
    mutable int wrap_gate_ = -1;
    mutable int read_gate_ = -1;
    mutable bool read_have_ = false;
    mutable ::jfqr::Code qr_code_;
    mutable std::string qr_text_;

    bool open_ = false;
    int opened_ = 0;     // the cover the details belong to
    tween::Spring lift_; // 0 in the row, 1 lifted beside the panel
    tween::Timer launch_;
};

// ---- the pickers that open over the wheel: the search keyboard, the A-Z grid, the sort menu ----

// A panel that eases in and out: 0 gone, 1 fully up
struct Ease
{
    float value = 0.0f;
    void step(bool up, float dt, bool reduced)
    {
        const float target = up ? 1.0f : 0.0f;
        if (reduced)
            value = target;
        else
            value += (target - value) * (1.0f - std::exp(-16.0f * dt));
        if (std::fabs(target - value) < 0.002f)
            value = target;
    }
    bool visible() const
    {
        return value > 0.004f;
    }
};

// The panel's own look: dark and quiet, a little glow of the cover's colour above it
void draw_panel(gfx::DrawList &list, const Rect &panel, float radius)
{
    list.shadow({panel.x, panel.y + 18.0f, panel.w, panel.h}, radius, 54.0f, kBlack.with_alpha(0.55f));
    list.rounded_rect(panel, radius, Color{0.035f, 0.04f, 0.075f, 0.95f});
    list.bordered_rect(panel, radius, kClear, 1.5f, kWhite.with_alpha(0.2f));
}

// An on-screen keyboard of our own: letters, digits, space, delete and done.
class Keyboard
{
  public:
    enum class Kind
    {
        letter,
        space,
        erase,
        done
    };
    struct Key
    {
        const char *label;
        Kind kind;
        int row;
        int col;
        int span;
    };

    Keyboard()
    {
        static const char *const rows[3] = {"1234567890", "qwertyuiop", "asdfghjkl"};
        static const char *const labels[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "q", "w", "e", "r", "t", "y",
                                             "u", "i", "o", "p", "a", "s", "d", "f", "g", "h", "j", "k", "l", "z", "x", "c",
                                             "v", "b", "n", "m"};
        (void)rows;
        int at = 0;
        for (int row = 0; row < 4; ++row)
        {
            const int count = row == 0 ? 10 : row == 1 ? 10 : row == 2 ? 9 : 7;
            for (int col = 0; col < count; ++col)
                keys_[n_++] = {labels[at++], Kind::letter, row, col, 1};
        }
        keys_[n_++] = {"DEL", Kind::erase, 2, 9, 1};
        keys_[n_++] = {"SPACE", Kind::space, 3, 7, 2};
        keys_[n_++] = {"DONE", Kind::done, 3, 9, 1};
    }

    bool is_open() const
    {
        return open_;
    }

    const std::string &query() const
    {
        return query_;
    }

    void open(const std::string &query)
    {
        query_ = query;
        open_ = true;
        dirty_ = false;
        row_ = 1;
        col_ = 0;
        snap_ = true;
    }

    void close()
    {
        open_ = false;
    }

    // The text changed and has rested long enough to be searched for
    bool due() const
    {
        return dirty_ && wait_ <= 0.0f;
    }
    bool dirty() const
    {
        return dirty_;
    }
    void clean()
    {
        dirty_ = false;
    }

    void advance(float dt, bool reduced)
    {
        ease_.step(open_, dt, reduced);
        clock_ += dt;
        wait_ = std::max(0.0f, wait_ - dt);
    }

    // Returns true when the keyboard wants to close
    bool handle(const InputFrame &input, app::Feedback &feedback)
    {
        if (input.is_pressed(Action::back))
        {
            feedback.play(audio::Cue::back);
            return true;
        }
        if (input.is_pressed(Action::west))
            erase(feedback);
        if (input.is_pressed(Action::north))
            type(' ', feedback);
        if (input.nav != Direction::none)
            move(input.nav, feedback);
        if (input.is_pressed(Action::confirm))
        {
            const Key *key = key_at(row_, col_);
            if (key)
            {
                switch (key->kind)
                {
                case Kind::letter:
                    type(key->label[0], feedback);
                    break;
                case Kind::space:
                    type(' ', feedback);
                    break;
                case Kind::erase:
                    erase(feedback);
                    break;
                case Kind::done:
                    feedback.play(audio::Cue::open);
                    return true;
                }
            }
        }
        return false;
    }

    // `status` is the line at the right of the query: how many titles were found
    void draw(gfx::DrawList &list, const ui::Fonts &fonts, const char *status) const
    {
        if (!ease_.visible())
            return;
        const float e = tween::cubic_out(ease_.value);
        Rect panel{395.0f, 630.0f, 1130.0f, 430.0f};
        panel.y += (1.0f - e) * 460.0f;
        list.push_opacity(tween::clamp01(ease_.value * 1.6f));
        draw_panel(list, panel, 34.0f);

        // the query, with a caret
        const float qx = panel.x + 44.0f;
        const float qy = panel.y + 68.0f;
        if (query_.empty())
        {
            ui::text(list, fonts.semibold, "Type a title", qx, qy, 42, kWhite.with_alpha(0.3f));
        }
        else
        {
            const std::string shown = query_.size() > 30 ? query_.substr(query_.size() - 30) : query_;
            ui::text(list, fonts.semibold, shown.c_str(), qx, qy, 42, kWhite);
        }
        const std::string shown = query_.size() > 30 ? query_.substr(query_.size() - 30) : query_;
        const float caret_x = qx + (query_.empty() ? 0.0f : fonts.semibold.measure(shown.c_str(), 42)) + 4.0f;
        const float blink = 0.5f + 0.5f * std::sin(clock_ * 7.0f);
        list.rounded_rect({caret_x, qy - 36.0f, 3.0f, 44.0f}, 1.5f, kWhite.with_alpha(0.35f + 0.55f * blink));
        ui::text(list, fonts.regular, status, panel.x + panel.w - 44.0f, qy - 4.0f, 24, kWhite.with_alpha(0.62f),
                 gfx::Align::right);
        list.rounded_rect({panel.x + 40.0f, panel.y + 90.0f, panel.w - 80.0f, 1.5f}, 0.75f, kWhite.with_alpha(0.14f));

        // the keys, and the light that glides between them
        const float x0 = panel.x + 40.0f;
        const float y0 = panel.y + 108.0f;
        auto rect_of = [&](const Key &key) {
            return Rect{x0 + static_cast<float>(key.col) * (kKeyW + kKeyGap), y0 + static_cast<float>(key.row) * (kKeyH + kKeyGap),
                        kKeyW * static_cast<float>(key.span) + kKeyGap * static_cast<float>(key.span - 1), kKeyH};
        };
        const Key *here = key_at(row_, col_);
        if (here)
        {
            const Rect target = rect_of(*here);
            if (snap_ || !ease_.visible())
            {
                light_ = target;
                snap_ = false;
            }
            const float follow = 1.0f - std::exp(-24.0f * (1.0f / 60.0f));
            light_.x += (target.x - light_.x) * follow;
            light_.y += (target.y - light_.y) * follow;
            light_.w += (target.w - light_.w) * follow;
            light_.h += (target.h - light_.h) * follow;
        }
        for (int i = 0; i < n_; ++i)
        {
            const Key &key = keys_[i];
            const Rect r = rect_of(key);
            list.rounded_rect(r, 16.0f, kWhite.with_alpha(0.09f));
        }
        list.glow(light_, 16.0f, 22.0f, kWhite.with_alpha(0.28f));
        list.rounded_rect(light_, 16.0f, kWhite);
        for (int i = 0; i < n_; ++i)
        {
            const Key &key = keys_[i];
            const Rect r = rect_of(key);
            const float near = tween::clamp01(1.0f - (std::fabs(r.cx() - light_.cx()) / (kKeyW * 0.9f) +
                                                      std::fabs(r.cy() - light_.cy()) / (kKeyH * 0.9f)));
            const bool word = key.kind != Kind::letter;
            ui::text(list, fonts.semibold, key.label, r.cx(), r.cy() + (word ? 8.0f : 12.0f), word ? 20 : 32,
                     gfx::mix(kWhite.with_alpha(0.9f), kInk, near), gfx::Align::center, word ? 2.0f : 0.0f);
        }
        ui::text(list, fonts.regular, "Cross  type      Square  delete      Triangle  space      Circle  close", panel.cx(),
                 panel.y + panel.h - 24.0f, 20, kWhite.with_alpha(0.5f), gfx::Align::center);
        list.pop_opacity();
    }

  private:
    static constexpr float kKeyW = 96.0f;
    static constexpr float kKeyH = 62.0f;
    static constexpr float kKeyGap = 10.0f;

    const Key *key_at(int row, int col) const
    {
        for (int i = 0; i < n_; ++i)
        {
            if (keys_[i].row == row && col >= keys_[i].col && col < keys_[i].col + keys_[i].span)
                return &keys_[i];
        }
        return nullptr;
    }

    void move(Direction direction, app::Feedback &feedback)
    {
        int row = row_;
        int col = col_;
        const Key *now = key_at(row_, col_);
        const int left = now ? now->col : col_;
        const int right = now ? now->col + now->span - 1 : col_;
        switch (direction)
        {
        case Direction::left:
            col = left - 1;
            break;
        case Direction::right:
            col = right + 1;
            break;
        case Direction::up:
            row -= 1;
            break;
        case Direction::down:
            row += 1;
            break;
        default:
            return;
        }
        if (row < 0 || row > 3 || col < 0 || col > 9 || !key_at(row, col))
        {
            // the end of a row: a short row's nearest key
            if (row >= 0 && row <= 3 && direction != Direction::left && direction != Direction::right)
            {
                for (int c = std::min(col, 9); c >= 0; --c)
                {
                    if (key_at(row, c))
                    {
                        col = c;
                        break;
                    }
                }
            }
            else
                return;
        }
        if (!key_at(row, col))
            return;
        row_ = row;
        col_ = col;
        feedback.play(audio::Cue::tick, 1.0f, 0.0f, 0.8f);
    }

    void type(char ch, app::Feedback &feedback)
    {
        if (query_.size() >= 40 || (ch == ' ' && (query_.empty() || query_.back() == ' ')))
        {
            feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.5f);
            return;
        }
        query_ += ch;
        dirty_ = true;
        wait_ = 0.45f;
        feedback.play(audio::Cue::focus, 1.0f + 0.01f * static_cast<float>(query_.size() % 7), 0.0f, 0.8f);
    }

    void erase(app::Feedback &feedback)
    {
        if (query_.empty())
        {
            feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.5f);
            return;
        }
        query_.pop_back();
        dirty_ = true;
        wait_ = 0.45f;
        feedback.play(audio::Cue::back, 1.1f, 0.0f, 0.7f);
    }

    Key keys_[40];
    int n_ = 0;
    std::string query_;
    bool open_ = false;
    bool dirty_ = false;
    mutable bool snap_ = true;
    int row_ = 1;
    int col_ = 0;
    float wait_ = 0.0f;
    float clock_ = 0.0f;
    mutable Rect light_{0.0f, 0.0f, kKeyW, kKeyH};
    Ease ease_;
};

// 27 cells, # and A to Z: the ones with titles under them can be picked.
class LetterGrid
{
  public:
    bool is_open() const
    {
        return open_;
    }

    void open(const ::jf::List &list, int bucket)
    {
        open_ = true;
        count_ = static_cast<int>(list.size());
        for (int k = 0; k < 27; ++k)
        {
            const int start = list.letters[k];
            const int next = k < 26 ? list.letters[k + 1] : count_;
            first_[k] = start;
            number_[k] = (start < count_ && next > start) ? next - start : 0;
        }
        cursor_ = number_[bucket] > 0 ? bucket : first_available();
        snap_ = true;
    }

    void close()
    {
        open_ = false;
    }

    void advance(float dt, bool reduced)
    {
        ease_.step(open_, dt, reduced);
    }

    // Returns -1 to keep going, -2 to close, or the position to move the wheel to
    int handle(const InputFrame &input, app::Feedback &feedback)
    {
        if (input.is_pressed(Action::back) || input.is_pressed(Action::west))
        {
            feedback.play(audio::Cue::back);
            return -2;
        }
        if (input.nav != Direction::none)
        {
            int dx = 0, dy = 0;
            if (input.nav == Direction::left)
                dx = -1;
            else if (input.nav == Direction::right)
                dx = 1;
            else if (input.nav == Direction::up)
                dy = -1;
            else if (input.nav == Direction::down)
                dy = 1;
            int at = cursor_;
            for (;;)
            {
                const int col = at % kCols + dx;
                const int row = at / kCols + dy;
                if (col < 0 || col >= kCols || row < 0 || row >= kRows)
                {
                    at = -1;
                    break;
                }
                at = row * kCols + col;
                if (at >= 27)
                {
                    at = -1;
                    break;
                }
                if (number_[at] > 0)
                    break;
            }
            if (at >= 0 && at != cursor_)
            {
                cursor_ = at;
                feedback.play(audio::Cue::tick, 1.0f, 0.0f, 0.8f);
            }
        }
        if (input.is_pressed(Action::confirm))
        {
            if (number_[cursor_] > 0)
            {
                feedback.play(audio::Cue::tab, 1.0f, 0.0f);
                return first_[cursor_];
            }
            feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
        }
        return -1;
    }

    void draw(gfx::DrawList &list, const ui::Fonts &fonts) const
    {
        if (!ease_.visible())
            return;
        const float e = tween::cubic_out(ease_.value);
        Rect panel{(1920.0f - kPanelW) * 0.5f, 560.0f, kPanelW, 440.0f};
        panel.y += (1.0f - e) * 420.0f;
        list.push_opacity(tween::clamp01(ease_.value * 1.6f));
        draw_panel(list, panel, 34.0f);
        ui::text(list, fonts.semibold, "JUMP TO", panel.x + 48.0f, panel.y + 62.0f, 20, kWhite.with_alpha(0.6f),
                 gfx::Align::left, 4.0f);
        const float x0 = panel.x + 48.0f;
        const float y0 = panel.y + 92.0f;
        auto rect_of = [&](int k) {
            return Rect{x0 + static_cast<float>(k % kCols) * (kCellW + kCellGap),
                        y0 + static_cast<float>(k / kCols) * (kCellH + kCellGap), kCellW, kCellH};
        };
        const Rect target = rect_of(cursor_);
        if (snap_)
        {
            light_ = target;
            snap_ = false;
        }
        const float follow = 1.0f - std::exp(-24.0f * (1.0f / 60.0f));
        light_.x += (target.x - light_.x) * follow;
        light_.y += (target.y - light_.y) * follow;
        for (int k = 0; k < 27; ++k)
            list.rounded_rect(rect_of(k), 18.0f, kWhite.with_alpha(number_[k] > 0 ? 0.09f : 0.03f));
        list.glow(light_, 18.0f, 22.0f, kWhite.with_alpha(0.28f));
        list.rounded_rect(light_, 18.0f, kWhite);
        for (int k = 0; k < 27; ++k)
        {
            const Rect r = rect_of(k);
            const float near = tween::clamp01(1.0f - (std::fabs(r.cx() - light_.cx()) + std::fabs(r.cy() - light_.cy())) / (kCellW * 0.9f));
            const bool has = number_[k] > 0;
            const Color ink = gfx::mix(kWhite.with_alpha(has ? 0.92f : 0.22f), kInk, near);
            const char label[2] = {letter_char(k), 0};
            ui::text(list, fonts.display, label, r.cx(), r.cy() + 4.0f, 40, ink, gfx::Align::center);
            if (has)
            {
                char count[16];
                std::snprintf(count, sizeof(count), "%d", number_[k]);
                ui::text(list, fonts.regular, count, r.cx(), r.cy() + 30.0f, 16, gfx::mix(kWhite.with_alpha(0.5f), kInk.with_alpha(0.7f), near),
                         gfx::Align::center);
            }
        }
        ui::text(list, fonts.regular, "Cross  jump      Circle  close", panel.cx(), panel.y + panel.h - 26.0f, 20,
                 kWhite.with_alpha(0.5f), gfx::Align::center);
        list.pop_opacity();
    }

  private:
    static constexpr int kCols = 9;
    static constexpr int kRows = 3;
    static constexpr float kCellW = 104.0f;
    static constexpr float kCellH = 86.0f;
    static constexpr float kCellGap = 12.0f;
    static constexpr float kPanelW = 96.0f + 9.0f * kCellW + 8.0f * kCellGap;

    int first_available() const
    {
        for (int k = 0; k < 27; ++k)
        {
            if (number_[k] > 0)
                return k;
        }
        return 0;
    }

    bool open_ = false;
    int count_ = 0;
    int cursor_ = 0;
    int first_[27] = {};
    int number_[27] = {};
    mutable bool snap_ = true;
    mutable Rect light_{0.0f, 0.0f, kCellW, kCellH};
    Ease ease_;
};

// OPTIONS: how a library is sorted, and what it shows.
class SortMenu
{
  public:
    bool is_open() const
    {
        return open_;
    }

    void open(const ::jf::List &list)
    {
        open_ = true;
        category_ = list.category;
        favourites_ = list.kind == 4;
        sort_ = list.sort;
        filter_ = list.filter;
        row_ = 0;
        was_sort_ = list.sort;
        was_filter_ = list.filter;
    }

    void close()
    {
        open_ = false;
    }

    void advance(float dt, bool reduced)
    {
        ease_.step(open_, dt, reduced);
    }

    // Returns 0 to keep going, 1 to close, 2 to close and apply
    int handle(const InputFrame &input, app::Feedback &feedback)
    {
        if (input.is_pressed(Action::back) || input.is_pressed(Action::menu))
        {
            feedback.play(audio::Cue::back);
            return 1;
        }
        if (input.nav == Direction::up || input.nav == Direction::down)
        {
            const int rows = favourites_ ? 1 : 2;
            const int next = std::clamp(row_ + (input.nav == Direction::down ? 1 : -1), 0, rows - 1);
            if (next != row_)
            {
                row_ = next;
                feedback.play(audio::Cue::tick, 1.0f, 0.0f, 0.8f);
            }
        }
        if (input.nav == Direction::left || input.nav == Direction::right)
        {
            const int step = input.nav == Direction::right ? 1 : -1;
            int &value = row_ == 0 ? sort_ : filter_;
            const int top = row_ == 0 ? 4 : 2;
            const int next = std::clamp(value + step, 0, top);
            if (next != value)
            {
                value = next;
                feedback.play(audio::Cue::focus, 1.0f + 0.03f * static_cast<float>(step), 0.3f * static_cast<float>(step));
            }
        }
        if (input.is_pressed(Action::confirm))
        {
            feedback.play(audio::Cue::open);
            return 2;
        }
        return 0;
    }

    bool changed() const
    {
        return sort_ != was_sort_ || filter_ != was_filter_;
    }

    ::jf::ViewRequest request() const
    {
        return {category_, sort_, favourites_ ? 0 : filter_};
    }

    void draw(gfx::DrawList &list, const ui::Fonts &fonts) const
    {
        if (!ease_.visible())
            return;
        static const char *const sorts[] = {"Title A-Z", "Newest release", "Recently added", "Top rated", "Longest first"};
        static const char *const filters[] = {"Everything", "Unwatched", "Favourites"};
        const float e = tween::cubic_out(ease_.value);
        Rect panel{(1920.0f - 1360.0f) * 0.5f, 330.0f, 1360.0f, favourites_ ? 300.0f : 430.0f};
        panel.y += (1.0f - e) * 80.0f;
        list.push_opacity(tween::clamp01(ease_.value * 1.6f));
        draw_panel(list, panel, 34.0f);
        draw_row(list, fonts, panel, panel.y + 70.0f, "SORT BY", sorts, 5, sort_, row_ == 0);
        if (!favourites_)
            draw_row(list, fonts, panel, panel.y + 220.0f, "SHOW", filters, 3, filter_, row_ == 1);
        ui::text(list, fonts.regular, "Left Right  change      Up Down  next row      Cross  apply      Circle  cancel", panel.cx(),
                 panel.y + panel.h - 26.0f, 20, kWhite.with_alpha(0.5f), gfx::Align::center);
        list.pop_opacity();
    }

  private:
    static void draw_row(gfx::DrawList &list, const ui::Fonts &fonts, const Rect &panel, float y, const char *title,
                         const char *const *names, int count, int chosen, bool focus)
    {
        ui::text(list, fonts.semibold, title, panel.x + 56.0f, y, 20, kWhite.with_alpha(focus ? 0.95f : 0.45f), gfx::Align::left, 4.0f);
        float widths[5];
        float total = 18.0f * static_cast<float>(count - 1);
        for (int i = 0; i < count; ++i)
        {
            widths[i] = fonts.semibold.measure(names[i], 26) + 44.0f;
            total += widths[i];
        }
        float x = panel.x + 56.0f;
        for (int i = 0; i < count; ++i)
        {
            const Rect chip{x, y + 22.0f, widths[i], 66.0f};
            const bool on = i == chosen;
            if (on && focus)
                list.glow(chip, 33.0f, 22.0f, kWhite.with_alpha(0.3f));
            list.rounded_rect(chip, 33.0f, on ? kWhite.with_alpha(focus ? 1.0f : 0.78f) : kWhite.with_alpha(0.09f));
            ui::text(list, fonts.semibold, names[i], chip.cx(), chip.cy() + 9.0f, 26, on ? kInk : kWhite.with_alpha(focus ? 0.85f : 0.5f),
                     gfx::Align::center);
            x += widths[i] + 18.0f;
        }
        (void)total;
    }

    bool open_ = false;
    bool favourites_ = false;
    int category_ = 0;
    int sort_ = 0;
    int filter_ = 0;
    int was_sort_ = 0;
    int was_filter_ = 0;
    int row_ = 0;
    Ease ease_;
};

// The design: the tabs, and the wheel of the library they name.
class CoverFlow final : public app::Concept
{
  public:
    explicit CoverFlow(app::Context &context) : context_(context)
    {
        sync();
        const int n = static_cast<int>(shelves_.size());
        active_ = n > 0 ? std::clamp(::jf::startCategory(), 0, n - 1) : 0;
        tab_pos_.snap(static_cast<float>(active_));
        tabs_amount_.snap(0.0f);
    }

    const app::ConceptInfo &info() const override
    {
        static const app::ConceptInfo kInfo{
            "carousel",
            "Cover Wheel",
            "A carousel with weight: scrub it, let it coast, open a cover",
            "examples/jellyfinwheel/kit/screen.cpp",
            audio::SoundSet::glass,
            Color::rgb(0x38e8ff),
            kTechniques,
        };
        return kInfo;
    }

    void enter() override
    {
        age_ = 0.0f;
        if (!shelves_.empty())
            shelves_[static_cast<std::size_t>(active_)]->enter();
    }

    void update(const InputFrame &input, float dt, app::Feedback &feedback) override
    {
        age_ += dt;
        clock_ += dt;
        sync();
        if (shelves_.empty())
            return;
        if (disc_ && (!::jf::discoverEnabled() || ::jf::discoverLeave()))
            set_disc(false);
        ::jf::discoverLeave() = false;
        if (::jf::discoverGo() == 1 || ::jf::discoverGo() == 2)
        {
            // Settings > Seerr > Address / Account: the Discover tab opens, and asks
            if (::jf::discoverEnabled())
            {
                if (disc_)
                    set_disc(false);
                set_disc(true);
                tabs_focus_ = false;
                profile_focus_ = false;
            }
            else
                ::jf::discoverGo() = 0;
        }

        if (::jf::castRequest().pending && !disc_)
        {
            // a film page asked for its cast: the Discover tab opens on it (Back returns here, the page is still open)
            if (::jf::discoverEnabled())
            {
                set_disc(true);
                tabs_focus_ = false;
                profile_focus_ = false;
            }
            else
                ::jf::castRequest().pending = false;
        }
        if (::jf::browseRequest() >= 0)
        {
            // a series, season or collection was opened: its children are on the browse wheel
            const int index = ::jf::browseRequest();
            ::jf::browseRequest() = -1;
            const int category = ::jf_gate_browse_open(index);
            if (category >= 0 && category < static_cast<int>(shelves_.size()))
            {
                if (!is_browse())
                    before_browse_ = active_;
                select(category, feedback);
                tabs_focus_ = false;
            }
        }

        InputFrame neutral{};
        neutral.connected = true;
        Shelf &shelf = *shelves_[static_cast<std::size_t>(active_)];

        keyboard_.advance(dt, reduced());
        letters_.advance(dt, reduced());
        menu_.advance(dt, reduced());
        send_search(false);

        // While one wheel slides out and the other in, presses wait
        const bool busy = slide_ < 0.5f;
        const bool had_tabs = tabs_focus_;
        bool wheel_has_input = false;
        if (overlay_ != Overlay::none)
        {
            run_overlay(input, shelf, feedback);
        }
        else if (!shelf.is_open() && !busy)
        {
            if (is_browse() && input.is_pressed(Action::back))
            {
                // up one level (seasons from episodes), or back to the wheel it was opened from
                feedback.play(audio::Cue::back);
                if (!::jf_gate_browse_back())
                    select(before_browse_, feedback);
                tabs_focus_ = false;
            }
            else if (has_search() && active_ == search_index() && input.is_pressed(Action::back))
            {
                // one press, whether the keyboard was hidden or the tabs had the focus
                select(std::min(before_search_, tab_count() - 1), feedback);
                tabs_focus_ = false;
                return_from_search_ = true;
            }
            route_tabs(input, feedback);
            if (!tabs_focus_ && !had_tabs && overlay_ == Overlay::none && !disc_)
                open_from_wheel(input, shelf, feedback);
            // the press that brought the focus back, or opened a picker, is not also the wheel's
            wheel_has_input = !tabs_focus_ && !had_tabs && overlay_ == Overlay::none && !disc_;
        }
        else
        {
            wheel_has_input = !tabs_focus_ && !busy && !disc_;
        }
        if (disc_)
        {
            // The Discover page has the pad whenever the tabs do not (not on the frame the tabs handed it over)
            const ::jf::DiscoverHooks &h = ::jf::discoverHooks();
            const bool pad = !tabs_focus_ && !had_tabs && overlay_ == Overlay::none && !busy;
            if (h.update && h.update(h.ctx, pad ? static_cast<const void *>(&input) : static_cast<const void *>(&neutral), dt, &feedback, pad))
            {
                tabs_focus_ = true; // the page wants the tabs back
                feedback.play(audio::Cue::focus, 0.9f, 0.0f, 0.8f);
            }
        }
        shelf.update(wheel_has_input ? input : neutral, dt, feedback);
        if (leaving_ >= 0)
        {
            app::Feedback silent;
            shelves_[static_cast<std::size_t>(leaving_)]->update(neutral, dt, silent);
        }
        // The hop waits for a wheel that has its titles
        if (nudge_pending_ && shelf.ready())
        {
            nudge_pending_ = false;
            shelf.nudge();
        }

        if (slide_ < 1.0f)
        {
            slide_ = std::min(1.0f, slide_ + dt / kSlideTime);
            if (slide_ >= 1.0f)
                leaving_ = -1;
        }
        tab_pos_.target = static_cast<float>(disc_ ? tab_count() : active_);
        tab_pos_.update(dt, 14.0f);
        tabs_amount_.target = tabs_focus_ ? 1.0f : 0.0f;
        tabs_amount_.update(dt, 16.0f);

        {
            const Color glow = shelf.light_colour();
            ::jf::LightColour &lc = ::jf::lightColour();
            lc.r = glow.r;
            lc.g = glow.g;
            lc.b = glow.b;
        }

        // Tell the worker thread where we are looking
        ::jf::view().category = shelf.category();
        ::jf::view().centre = shelf.centre();
    }

    void draw(app::Frame &frame) const override
    {
        if (shelves_.empty())
            return;
        const Shelf &active = *shelves_[static_cast<std::size_t>(active_)];
        const bool sliding = leaving_ >= 0 && slide_ < 0.999f;
        const float e = tween::cubic_in_out(tween::clamp01(slide_));

        active.set_chrome(tabs_focus_, tabs_amount_.value, overlay_ != Overlay::none);
        if (!disc_)
            active.draw_base(frame); // the Discover page sets its own backdrop
        if (sliding && !disc_)
            shelves_[static_cast<std::size_t>(leaving_)]->set_chrome(tabs_focus_, tabs_amount_.value, overlay_ != Overlay::none);
        if (sliding && !disc_)
            shelves_[static_cast<std::size_t>(leaving_)]->draw_front(frame, false, 0.0f, 1.0f - e, -dir_ * kSlide * e);
        if (!disc_)
            active.draw_front(frame, true, 0.0f, sliding ? e : 1.0f, sliding ? dir_ * kSlide * (1.0f - e) : 0.0f);
        if (disc_)
        {
            const ::jf::DiscoverHooks &h = ::jf::discoverHooks();
            if (h.draw)
                h.draw(h.ctx, &frame, &context_.fonts);
            const ui::GlyphStyle style = ui::GlyphStyle::dark();
            // the Discover page draws its own legend when it has the pad
            if (tabs_focus_)
            {
                const ui::Hint hints[] = {{ui::Button::dpad, "Library"}, {ui::Button::cross, "Open Discover"}};
                ui::draw_hints(frame.scene, context_.fonts, style, hints, 2, 1824, true);
            }
        }
        draw_tabs(frame.scene);
        draw_profile(frame.scene);

        const ui::Fonts &fonts = context_.fonts;
        if (overlay_ != Overlay::none || keyboard_.is_open())
        {
            const ::jf::List &found = *::jf::lists()[static_cast<std::size_t>(search_index())];
            char status[64];
            if (keyboard_.dirty() || (!found.known && !keyboard_.query().empty()))
                std::snprintf(status, sizeof(status), "Searching ...");
            else if (keyboard_.query().empty())
                status[0] = 0;
            else
                std::snprintf(status, sizeof(status), "%d found", static_cast<int>(found.size()));
            keyboard_.draw(frame.overlay, fonts, status);
        }
        else
        {
            keyboard_.draw(frame.overlay, fonts, "");
        }
        letters_.draw(frame.overlay, fonts);
        menu_.draw(frame.overlay, fonts);
    }

    std::span<const app::TourStep> tour() const override
    {
        return kTour;
    }

  private:
    enum class Overlay
    {
        none,
        keyboard,
        letters,
        menu
    };

    bool reduced() const
    {
        return context_.settings.reduced_motion;
    }

    int search_index() const
    {
        return static_cast<int>(shelves_.size()) - 1;
    }

    // The search wheel is the last list; it has no tab, the touch pad opens it
    bool has_search() const
    {
        return shelves_.size() >= 2 && ::jf::lists().back()->kind == 5;
    }

    // The lists that are browsed into (a series' seasons, the next episode) have no tab
    int hidden_lists() const
    {
        int n = 0;
        for (const auto &list : ::jf::lists())
            n += (list->kind == 6 || list->kind == 7) ? 1 : 0;
        return n;
    }

    int browse_index() const
    {
        for (std::size_t i = 0; i < ::jf::lists().size(); ++i)
            if (::jf::lists()[i]->kind == 6)
                return static_cast<int>(i);
        return -1;
    }

    bool is_browse() const
    {
        return browse_index() >= 0 && active_ == browse_index();
    }

    // How many tabs are shown
    int tab_count() const
    {
        return static_cast<int>(shelves_.size()) - (has_search() ? 1 : 0) - hidden_lists();
    }

    // A wheel for every category the program has made a list for.
    void sync()
    {
        while (shelves_.size() < ::jf::lists().size())
            shelves_.push_back(std::make_unique<Shelf>(context_, *::jf::lists()[shelves_.size()]));
    }

    // Changes library: the wheel that was on screen slides up and away, the new one comes up from below
    // (or the other way round when the new tab is to the left).
    bool select(int index, app::Feedback &feedback)
    {
        const int n = static_cast<int>(shelves_.size());
        if (index < 0 || index >= n || index == active_)
            return false;
        set_disc(false);
        const int from = active_;
        leaving_ = active_;
        dir_ = index > active_ ? 1.0f : -1.0f;
        active_ = index;
        slide_ = reduced() ? 1.0f : 0.0f;
        if (reduced())
            leaving_ = -1;
        shelves_[static_cast<std::size_t>(active_)]->enter();
        shelves_[static_cast<std::size_t>(active_)]->take_palette_from(*shelves_[static_cast<std::size_t>(from)]);
        feedback.play(audio::Cue::tab, 1.0f + 0.05f * dir_, 0.4f * dir_);
        return true;
    }

    // Enters or leaves the Discover tab (the program's Seerr page starts or stops with it)
    void set_disc(bool on)
    {
        if (disc_ == on)
            return;
        disc_ = on;
        const ::jf::DiscoverHooks &h = ::jf::discoverHooks();
        if (h.activate)
            h.activate(h.ctx, on);
    }

    // The wheel has the focus back: a cue to hear, and a hop and a ring to see
    void give_focus_to_wheel(app::Feedback &feedback)
    {
        tabs_focus_ = false;
        feedback.play(audio::Cue::focus, 1.14f, 0.0f, 0.9f);
        feedback.rumble(0.18f, 0.04f);
        nudge_pending_ = !disc_;
    }

    void route_tabs(const InputFrame &input, app::Feedback &feedback)
    {
        const int n = static_cast<int>(shelves_.size());
        if (input.is_pressed(Action::touch))
        {
            open_keyboard(feedback);
            return;
        }
        if (!tabs_focus_)
        {
            profile_focus_ = false;
            if (disc_)
                return; // the Discover page has the pad (see update)
            if (input.is_pressed(Action::up) && n > 1 && !is_browse())
            {
                tabs_focus_ = true;
                nudge_pending_ = false;
                feedback.play(audio::Cue::focus, 0.9f, 0.0f, 0.8f);
            }
            return;
        }
        if (profile_focus_)
        {
            // the profile picture at the end of the tabs
            if (input.nav == Direction::left)
            {
                profile_focus_ = false;
                feedback.play(audio::Cue::focus, 1.0f, -0.3f, 0.7f);
            }
            else if (input.is_pressed(Action::confirm))
            {
                ::jf::profileRequest() = true;
                feedback.play(audio::Cue::open);
            }
            else if (input.is_pressed(Action::down) || input.is_pressed(Action::back))
            {
                profile_focus_ = false;
                give_focus_to_wheel(feedback);
            }
            return;
        }
        const bool go_down = input.is_pressed(Action::down);
        const bool press = input.is_pressed(Action::confirm);
        if (go_down || press || input.is_pressed(Action::back))
        {
            const bool on_search = active_ == search_index();
            if (on_search && (press || (go_down && shelves_[static_cast<std::size_t>(active_)]->size() == 0)))
            {
                open_keyboard(feedback);
                return;
            }
            give_focus_to_wheel(feedback);
            return;
        }
        if (input.nav == Direction::left || input.nav == Direction::right)
        {
            const int direction = input.nav == Direction::right ? 1 : -1;
            bool moved = false;
            if (disc_ && direction < 0)
            {
                // Discover sits after the last category: Left goes on to the last one, so the round keeps going
                const int last_tab = tab_count() - 1;
                set_disc(false);
                if (last_tab >= 0 && active_ != last_tab && !is_browse())
                {
                    select(last_tab, feedback);
                    leaving_ = -1; // the wheel we leave was not on show (Discover was): nothing slides out
                }
                else
                    feedback.play(audio::Cue::tab, 0.95f, -0.4f);
                return;
            }
            if (direction < 0 && !disc_ && active_ == 0 && !is_browse() && ::jf::discoverEnabled())
            {
                set_disc(true); // left of the first category wraps round to Discover
                feedback.play(audio::Cue::tab, 0.95f, -0.4f);
                return;
            }
            if (direction > 0 && !disc_ && !(has_search() && active_ == search_index()) && active_ + 1 >= tab_count() && ::jf::discoverEnabled())
            {
                set_disc(true);
                feedback.play(audio::Cue::tab, 1.05f, 0.4f);
                return;
            }
            if (direction > 0 && !(has_search() && active_ == search_index()) && active_ + 1 >= tab_count())
            {
                profile_focus_ = true; // past the last tab: the profile picture
                feedback.play(audio::Cue::focus, 1.05f, 0.3f, 0.7f);
                return;
            }
            if (has_search() && active_ == search_index())
                moved = direction < 0 && select(std::min(before_search_, tab_count() - 1), feedback);
            else if (active_ + direction < tab_count())
                moved = select(active_ + direction, feedback);
            if (!moved && !input.nav_repeat)
            {
                feedback.play(audio::Cue::error, 1.0f, 0.4f * static_cast<float>(direction), 0.6f);
                feedback.rumble(0.25f, 0.05f);
            }
        }
    }

    // SQUARE: the A-Z grid (or the keyboard on the search wheel); OPTIONS: sort and filter; the touch pad: search
    void open_from_wheel(const InputFrame &input, Shelf &shelf, app::Feedback &feedback)
    {
        if (input.is_pressed(Action::touch))
        {
            open_keyboard(feedback);
            return;
        }
        if (input.is_pressed(Action::west))
        {
            if (active_ == search_index())
                open_keyboard(feedback);
            else if (shelf.lettered())
            {
                letters_.open(shelf.list(), shelf.letter_bucket());
                overlay_ = Overlay::letters;
                feedback.play(audio::Cue::open);
            }
            else
                feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
            return;
        }
        if ((input.is_pressed(Action::menu) || input.is_pressed(Action::r3)) && shelf.list().sortable)
        {
            menu_.open(shelf.list());
            overlay_ = Overlay::menu;
            feedback.play(audio::Cue::open);
        }
    }

    void open_keyboard(app::Feedback &feedback)
    {
        if (active_ != search_index())
        {
            before_search_ = active_;
            select(search_index(), feedback);
        }
        tabs_focus_ = false;
        nudge_pending_ = false;
        keyboard_.open(::jf::searchText());
        overlay_ = Overlay::keyboard;
        feedback.play(audio::Cue::open);
    }

    // Hands the typed text to the program, once it has rested (or at once with `force`)
    void send_search(bool force)
    {
        if (!keyboard_.dirty() || !(force || keyboard_.due()))
            return;
        ::jf::searchText() = keyboard_.query();
        ++::jf::searchSerial();
        keyboard_.clean();
    }

    void run_overlay(const InputFrame &input, Shelf &shelf, app::Feedback &feedback)
    {
        switch (overlay_)
        {
        case Overlay::keyboard:
            if (keyboard_.handle(input, feedback))
            {
                send_search(true);
                keyboard_.close();
                overlay_ = Overlay::none;
                if (keyboard_.query().empty())
                    tabs_focus_ = true; // nothing to look at: stay on the tabs
                else
                    give_focus_to_wheel(feedback);
            }
            break;
        case Overlay::letters:
        {
            const int result = letters_.handle(input, feedback);
            if (result >= 0)
                shelf.jump_to(result, feedback);
            if (result != -1)
            {
                letters_.close();
                overlay_ = Overlay::none;
            }
            break;
        }
        case Overlay::menu:
        {
            const int result = menu_.handle(input, feedback);
            if (result != 0)
            {
                if (result == 2 && menu_.changed())
                    ::jf::viewRequests().push_back(menu_.request());
                menu_.close();
                overlay_ = Overlay::none;
            }
            break;
        }
        default:
            break;
        }
    }

    // The categories, in a row; one underline glides between them.
    void draw_tabs(gfx::DrawList &list) const
    {
        const int real = tab_count();
        const int n = real + (::jf::discoverEnabled() ? 1 : 0);
        if (n < 1 || shelves_.size() < 2)
            return;
        const ui::Fonts &fonts = context_.fonts;
        auto tab_name = [real](int i) -> const char * {
            return i < real ? ::jf::lists()[static_cast<std::size_t>(i)]->name.c_str() : "Discover";
        };
        const float in = tween::stagger(age_, 0, 0.05f, 0.5f);
        const float underline = tween::clamp01(1.0f - std::max(0.0f, tab_pos_.value - static_cast<float>(n - 1)));
        const float focus = tween::clamp01(tabs_amount_.value);

        float size = kTabSize;
        float gap = kTabGap;
        std::vector<float> width(static_cast<std::size_t>(n));
        float total = gap * static_cast<float>(n - 1);
        for (int i = 0; i < n; ++i)
        {
            width[static_cast<std::size_t>(i)] = fonts.semibold.measure(tab_name(i), size);
            total += width[static_cast<std::size_t>(i)];
        }
        if (total > kTabsMax)
        {
            const float shrink = kTabsMax / total;
            size *= shrink;
            gap *= shrink;
            for (float &w : width)
                w *= shrink;
            total = kTabsMax;
        }
        std::vector<float> left(static_cast<std::size_t>(n));
        float x = kCentreX - total * 0.5f;
        for (int i = 0; i < n; ++i)
        {
            left[static_cast<std::size_t>(i)] = x;
            x += width[static_cast<std::size_t>(i)] + gap;
        }

        list.push_opacity(in);
        for (int i = 0; i < n; ++i)
        {
            const float amount = tween::clamp01(1.0f - std::fabs(tab_pos_.value - static_cast<float>(i)));
            const float rest = 0.40f + 0.22f * focus;
            const float alpha = rest + (1.0f - rest) * amount;
            ui::text(list, fonts.semibold, tab_name(i),
                     left[static_cast<std::size_t>(i)], kTabY, static_cast<int>(size),
                     kWhite.with_alpha(alpha));
        }
        // The underline glides: between two tabs it is between their places
        const float at = std::clamp(tab_pos_.value, 0.0f, static_cast<float>(n - 1));
        const int from = std::min(static_cast<int>(std::floor(at)), n - 1);
        const int to = std::min(from + 1, n - 1);
        const float t = at - static_cast<float>(from);
        const float ux = tween::lerp(left[static_cast<std::size_t>(from)], left[static_cast<std::size_t>(to)], t);
        const float uw = tween::lerp(width[static_cast<std::size_t>(from)], width[static_cast<std::size_t>(to)], t);
        const Rect bar{ux, kTabY + 14.0f, uw, 4.0f};
        if (focus > 0.01f)
            list.glow(bar, 2.0f, 18.0f, kWhite.with_alpha(0.4f * focus * underline));
        list.rounded_rect(bar, 2.0f, kWhite.with_alpha((0.55f + 0.45f * focus) * underline));
        list.pop_opacity();
    }

    // The profile picture, at the right end of the tabs: the server's picture, or a coloured disc with the initial
    void draw_profile(gfx::DrawList &list) const
    {
        if (shelves_.empty())
            return;
        const float in = tween::stagger(age_, 1, 0.05f, 0.5f);
        const float focus = profile_focus_ ? tween::clamp01(tabs_amount_.value) : 0.0f;
        const float r = 24.0f;
        const Rect box{1840.0f - r, kTabY - 11.0f - r, 2.0f * r, 2.0f * r};
        list.push_opacity(in);
        if (focus > 0.01f)
            list.glow(box, r, 18.0f, kWhite.with_alpha(0.45f * focus));
        if (::jf::profileTexture())
        {
            // a square from the middle of the picture (a portrait keeps its top, where the face is)
            const float aspect = std::max(0.1f, ::jf::profileAspect());
            Rect uv{0.0f, 0.0f, 1.0f, 1.0f};
            if (aspect > 1.0f)
            {
                uv.h = 1.0f / aspect;
                uv.y = (1.0f - uv.h) * 0.25f;
            }
            else if (aspect < 1.0f)
            {
                uv.w = aspect;
                uv.x = (1.0f - uv.w) * 0.5f;
            }
            list.image(::jf::profileTexture(), box, uv, kWhite, r);
        }
        else
        {
            list.rounded_rect(box, r, Color{0.67f, 0.36f, 0.76f, 1.0f});
            char initial[2] = {'?', 0};
            if (!::jf::profileName().empty())
                initial[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(::jf::profileName()[0])));
            ui::text(list, context_.fonts.semibold, initial, box.cx(), box.y + 33.0f, 26, kWhite, gfx::Align::center);
        }
        list.bordered_rect(box, r, kClear, 2.0f, kWhite.with_alpha(0.3f + 0.65f * focus));
        list.pop_opacity();
    }

    app::Context &context_;
    std::vector<std::unique_ptr<Shelf>> shelves_;
    int active_ = 0;
    int leaving_ = -1;    // the wheel on its way out, or -1
    float dir_ = 1.0f;    // 1: the old wheel goes left and the new one comes from the right (a later category)
    float slide_ = 1.0f;  // 0 when the slide starts, 1 when it is over
    bool tabs_focus_ = false;
    bool profile_focus_ = false; // the profile picture at the end of the tabs has the focus
    bool disc_ = false;          // the Discover tab (Seerr, drawn by the program) is the current tab
    bool nudge_pending_ = false; // the wheel got the focus back: hop once it has titles
    int before_search_ = 0;      // the library the search was opened from
    int before_browse_ = 0;      // the wheel a series, season or collection was opened from
    bool return_from_search_ = false;
    Overlay overlay_ = Overlay::none;
    Keyboard keyboard_;
    LetterGrid letters_;
    SortMenu menu_;
    float age_ = 0.0f;
    float clock_ = 0.0f;
    tween::Spring tab_pos_;     // the active tab as a number: 1.4 is between tabs 1 and 2
    tween::Spring tabs_amount_; // 1 while the tabs have the focus
};

} // namespace

std::unique_ptr<app::Concept> make_carousel(app::Context &context)
{
    return std::make_unique<CoverFlow>(context);
}

} // namespace hui::screen

// The title's screen, for its program (ps5/ui/kit.hpp declares it)
namespace ps5ui
{
std::unique_ptr<hui::app::Concept> make_screen(hui::app::Context &context)
{
    return hui::screen::make_carousel(context);
}
} // namespace ps5ui
