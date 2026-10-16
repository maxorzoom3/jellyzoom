/* What the Cover Wheel shows: one List per Jellyfin library ("4K", "Movies", "Shows"...), each with
 * one Item per title the server has. An item exists as soon as the list's size is known, so the wheel
 * has its real length at once; its details and its poster arrive later (ready, cover). The fields have
 * the names the kit's designs read from their own demo catalog (title, genre, blurb, rating...).
 *
 * Everything here is built and changed on the render thread only. */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <map>
#include <vector>

#include "gfx/draw_list.hpp"

namespace jf
{

// A film's place in the gate is category * kStride + position in that category
constexpr int kStride = 1000000;

struct Item
{
    // What the kit's designs read
    const char *title = "";
    const char *studio = "";
    const char *genre = "";
    const char *blurb = "";
    int year = 0;
    float rating = 0.0f; // 0 = not known: the screen leaves the stars out
    float progress = 0.0f;
    int hours = 0;
    int players = 0;
    hui::gfx::Color dark{0.02f, 0.02f, 0.03f, 1.0f};
    hui::gfx::Color mid{0.18f, 0.2f, 0.28f, 1.0f};
    hui::gfx::Color accent{0.6f, 0.7f, 1.0f, 1.0f};
    std::uint32_t cover = 0; // a renderer texture, top row first: the poster, or the shared placeholder

    // Ours
    const char *runtime = "";
    float aspect = 1.5f; // the poster's height over its width
    int gateIndex = -1;  // category * kStride + position
    float resumeSeconds = 0.0f;
    float durationSeconds = 0.0f;
    bool ready = false;     // the server's details have arrived
    bool playable = false;  // a film or an episode, as opposed to a series or a collection
    bool hasPoster = false; // cover is this title's own picture
    bool favorite = false;  // a favourite of the user's, on the server too
    bool watched = false;   // the server's played flag
    bool fresh = false;     // added to the server within the last two weeks
    char letter = '#';
    // Picture and sound flags, filled in once the server's full details are in (see JfGateDetails)
    enum Badge { kBadge4K = 1, kBadgeHDR = 2, kBadgeDV = 4, kBadgeAtmos = 8, kBadgeHD = 16 };
    int badges = 0;
    std::string qrUrl;      // the one page to scan: IMDb for a film, TheTVDB for a show
    bool detailed = false;
    std::string titleText, kindText, overviewText, runtimeText;
    std::string kindRaw;   // the server's type: Movie, Series, Season, Episode, BoxSet ...
    std::string countText; // "3 seasons", "10 episodes", "4 titles": what a series, season or collection holds

    // Points the strings above at the std::strings below; call after filling them in
    void bind()
    {
        title = titleText.c_str();
        genre = kindText.c_str();
        blurb = overviewText.c_str();
        runtime = runtimeText.c_str();
    }
};

struct List
{
    std::string name;
    int category = 0;
    bool known = false; // the server has said how many titles there are
    std::vector<std::unique_ptr<Item>> items;
    // The position of the first title of '#', 'A'...'Z' (27 values); empty letters repeat the next one
    int letters[27] = {};
    bool lettersKnown = false;

    // Set by the program from what the gate says about the category
    int kind = 0;           // 0 a library, 1 Continue Watching, 2 Next Up, 3 Recently Added, 4 Favourites, 5 search
    bool lettered = false;  // in name order: the A-Z offsets mean something
    bool sortable = false;  // the OPTIONS menu can sort and filter it
    int sort = 0;           // 0 title, 1 newest release, 2 recently added, 3 top rated, 4 longest
    int filter = 0;         // 0 everything, 1 unwatched, 2 favourites
    int epoch = 0;          // raised each time the items are thrown away and made again
    std::string message;    // what the wheel says while it has no titles ("" for the default)

    std::size_t size() const
    {
        return items.size();
    }
    const Item &operator[](std::size_t index) const
    {
        return *items[index];
    }
};

inline std::vector<std::unique_ptr<List>> &lists()
{
    static std::vector<std::unique_ptr<List>> instance;
    return instance;
}

// The right stick's vertical deflection, -1 (up) .. 1 (down): the program sets it each frame, the film page scrolls with it
inline float &rightStickY()
{
    static float instance = 0.0f;
    return instance;
}

// The colour of the wheel's spotlight, 0..1: the screen sets it, the program gives it to the controller's light bar
struct LightColour
{
    float r{ 0.2f }, g{ 0.3f }, b{ 0.5f };
};
inline LightColour &lightColour()
{
    static LightColour instance;
    return instance;
}

// What the screen is looking at: the program tells the worker thread so it fetches those posters first
struct View
{
    int category = 0;
    int centre = 3;
};
inline View &view()
{
    static View instance;
    return instance;
}

// Set by the design, taken by the program each frame
struct FavouriteRequest
{
    int gateIndex;
    bool on;
};
inline std::vector<FavouriteRequest> &favouriteRequests()
{
    static std::vector<FavouriteRequest> instance;
    return instance;
}
struct ViewRequest
{
    int category;
    int sort;
    int filter;
};
inline std::vector<ViewRequest> &viewRequests()
{
    static std::vector<ViewRequest> instance;
    return instance;
}
// The search box: the text, and a number that goes up each time it changes
inline std::string &searchText()
{
    static std::string instance;
    return instance;
}
inline int &searchSerial()
{
    static int instance = 0;
    return instance;
}
// The Discover tab (Seerr): switched on in Settings; the tab is the current one; the page (not the tabs) has the pad;
// the page wants the pad given back to the tabs; the program asks the design to leave the tab (Seerr off, signed out)
inline bool &discoverEnabled()
{
    static bool value = false;
    return value;
}
// The program gives the design these to run Seerr's page inside the design: it is drawn with the tabs and gets the pad
// from the design's own input handling. Plain function pointers, because this file knows none of the kit's types.
struct DiscoverHooks
{
    void *ctx = nullptr;
    void (*activate)(void *ctx, bool on) = nullptr;
    // input (const InputFrame *), feedback, pad: true when the page has the pad. Returns true when it wants the tabs back
    bool (*update)(void *ctx, const void *input, float dt, void *feedback, bool pad) = nullptr;
    // frame (app::Frame *: the page sets the backdrop and draws into its scene), fonts (const ui::Fonts *)
    void (*draw)(void *ctx, void *list, const void *fonts) = nullptr;
};
inline DiscoverHooks &discoverHooks()
{
    static DiscoverHooks value;
    return value;
}
// Settings > Seerr: 1 the Discover tab opens at the address, 2 at the password, 3 forget the Seerr sign-in (the page stays)
inline int &discoverGo()
{
    static int value = 0;
    return value;
}
// A film page asks for the cast of its title: the design opens the Discover tab on that title's cast page, and Back returns
struct CastRequest
{
    bool pending = false;
    long long tmdb = 0;
    char type[8] = {};   // "movie" or "tv"
    char title[160] = {};
};
inline CastRequest &castRequest()
{
    static CastRequest value;
    return value;
}
inline bool &discoverLeave()
{
    static bool value = false;
    return value;
}
// The design sets this when the profile picture is pressed; the program opens the profile menu and clears it
inline bool &profileRequest()
{
    static bool value = false;
    return value;
}
// The profile picture's renderer texture (0: none, the design draws the initial), and the profile's name
inline std::uint32_t &profileTexture()
{
    static std::uint32_t value = 0;
    return value;
}
// The picture's height over its width
inline float &profileAspect()
{
    static float value = 1.0f;
    return value;
}
inline std::string &profileName()
{
    static std::string value;
    return value;
}
// The category the wheel opens on (set by the program before the design is made)
inline int &startCategory()
{
    static int instance = 0;
    return instance;
}

// The design sets this to the gate index of the title whose page is open, or -1: the program shows its backdrop
inline int &openedTitle()
{
    static int index = -1;
    return index;
}

// The design asks to look inside a series, season or collection (a gate index); the design itself takes it again at once
inline int &browseRequest()
{
    static int index = -1;
    return index;
}
// With browseRequest: open this season (a position in the series' season list) instead of the series' own list; -1 for none
inline int &browseSeason()
{
    static int season = -1;
    return season;
}
// Stage 60: the season poster shown on a series' page (the design says which one it wants, the program fetches and keeps them)
struct SeasonWant
{
    int gate = -1;
    int season = -1;
};
inline SeasonWant &seasonWant()
{
    static SeasonWant want;
    return want;
}
struct SeasonCover
{
    std::uint32_t tex = 0;
    int w = 0, h = 0;
};
inline long long seasonKey(int gate, int season) { return static_cast<long long>(gate) * 64 + season; }
inline std::map<long long, SeasonCover> &seasonCovers()
{
    static std::map<long long, SeasonCover> covers;
    return covers;
}
// The logo of the series whose episodes are listed: the design says whose (a gate index), the program fetches it
inline int &logoWant()
{
    static int gate = -1;
    return gate;
}
struct LogoInfo
{
    std::uint32_t tex = 0;
    int w = 0, h = 0;
    int gate = -1;  // the title the picture is for
    int state = 0;  // 0 nothing asked, 1 loading, 2 here, -1 the title has no logo
};
inline LogoInfo &logoInfo()
{
    static LogoInfo info;
    return info;
}
// 0..1: how much of a title's backdrop picture the program is showing behind its page (the floor lets it through)
inline float &backdropShown()
{
    static float value = 0.0f;
    return value;
}

// The design sets this when Play is pressed on a film (its gateIndex); the program takes it each frame.
inline int &playRequest()
{
    static int index = -1;
    return index;
}
// What the person chose on the film's page: from the start or from the resume point, and the tracks.
// -1 means "the server's default". The program reads this when it takes the play request.
struct PlayOptions
{
    bool fromStart = false;
    int audio = -1;
    int subtitle = -1;
};
inline PlayOptions &playOptions()
{
    static PlayOptions instance;
    return instance;
}
inline void requestPlay(int index)
{
    playRequest() = index;
}
inline void requestPlay(int index, const PlayOptions &options)
{
    playOptions() = options;
    playRequest() = index;
}

} // namespace jf
