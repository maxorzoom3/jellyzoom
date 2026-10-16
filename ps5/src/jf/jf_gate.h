#pragma once
/* The Jellyfin client inside the title, on a thread of its own, reporting through klog (JFGATE lines).
 *
 * It signs in, lists the server's libraries, and then works on whatever the screen is looking at:
 * the page of titles around the wheel, the posters near the wheel, and the A-Z offsets.
 * A film is named by its gate index: category * JF_GATE_STRIDE + position in that category.
 *
 * A category is a library ("Movies", "Shows"...), or one of the lists the server keeps for the user
 * (Continue Watching, Next Up, Recently Added, Favourites), or the one search list, which is always
 * the last category. Libraries and Favourites can be sorted and filtered; every change of that kind
 * starts the category over and raises its generation, so the program knows to rebuild its list. */
#ifdef __cplusplus
extern "C" {
#endif

#define JF_GATE_STRIDE 1000000
#define JF_GATE_PAGE 100 /* titles the server is asked for at a time */

/* What a category is */
#define JF_GATE_LIBRARY 0
#define JF_GATE_HOME 1   /* Continue Watching, Next Up, Recently Added, Favourites */
#define JF_GATE_SEARCH 2 /* the last category: results of the search term */

/* The sorts and filters a library offers */
#define JF_GATE_SORTS 5
#define JF_GATE_FILTERS 3

void jf_gate_start(void);
void jf_gate_stop(void);

/* The categories: 0 until the worker has signed in and listed them. */
int jf_gate_category_count(void);
const char *jf_gate_category_name(int category);
int jf_gate_category_role(int category); /* JF_GATE_LIBRARY / HOME / SEARCH */
/* 0 a library, 1 Continue Watching, 2 Next Up, 3 Recently Added, 4 Favourites, 5 the search */
int jf_gate_category_kind(int category);
/* The category the wheel should open on: Continue Watching when there is something to continue,
 * otherwise the first library. Valid once the category count is. */
int jf_gate_start_category(void);
/* How many titles a category has: -1 until the worker has asked. */
int jf_gate_category_total(int category);
/* Raised every time a category starts over (sorted, filtered, searched, refreshed). */
int jf_gate_generation(int category);
/* 1 when the category is in name order, so the A-Z offsets mean something */
int jf_gate_category_lettered(int category);
/* 1 when the category can be sorted and filtered (libraries and Favourites) */
int jf_gate_category_sortable(int category);
int jf_gate_sort(int category);   /* 0 name, 1 newest release, 2 recently added, 3 top rated, 4 shuffle */
int jf_gate_filter(int category); /* 0 all, 1 unwatched, 2 favourites */
const char *jf_gate_sort_name(int sort);
const char *jf_gate_filter_name(int filter);

/* Sorts and filters a library: it starts over. */
void jf_gate_set_view(int category, int sort, int filter);
/* Asks the server again (after a film was watched, or a favourite changed). */
void jf_gate_refresh(int category);
/* The search list shows what the server finds for this text; "" empties it. */
void jf_gate_search(const char *term);
/* Makes a title a favourite, or not, on the server too. */
void jf_gate_set_favorite(int gate_index, int on);

/* Where the screen is looking: the worker fetches around it first. */
void jf_gate_want(int category, int centre);
/* 1 while a film plays: the worker waits, so the stream has the network. */
void jf_gate_pause(int paused);

/* 1 once the page of JF_GATE_PAGE titles starting at page * JF_GATE_PAGE is in. */
int jf_gate_page_loaded(int category, int page);

/* What the server said about one title. The strings stay valid until its category starts over. */
typedef struct {
	const char *name;
	const char *kind; /* Movie, Series, BoxSet, Episode... */
	const char *overview;
	long long runtime_ticks;  /* 10,000,000 a second; 0 if unknown */
	long long position_ticks; /* where it was left; 0 if not started */
	const char *genre;        /* the first genre, or a line about the episode, or "" */
	const char *sort_name;
	int year;
	float rating; /* the community rating out of 10; 0 if none */
	int favorite;
	int has_poster;
	int watched; /* the server's played flag */
	int added_days; /* days since it was added to the server; -1 if unknown */
	int child_count; /* a series: its seasons; a season: its episodes; a collection: its titles; else 0 */
} JfGateMeta;
int jf_gate_item_meta(int gate_index, JfGateMeta *out); /* 1 once that page is in */


/* ---- everything the server knows about one title: its tracks, its picture, its ids ----
 * Asked for when a title is opened (or rests in the middle of the wheel), answered by the worker. */
#define JF_GATE_TRACKS 40

typedef struct {
	int index;       /* the server's stream number: also the container's, for what is inside the file */
	int kind;        /* 1 sound, 2 subtitle */
	char title[96];  /* what the server shows for it: "English - Dolby TrueHD Atmos - 7.1" */
	char lang[12];
	char codec[20];
	int channels;
	int is_default;
	int forced;
	int external;    /* a file beside the film */
	int text;        /* a subtitle that can be shown: text, or pictures (PGS, DVD, DVB) inside the file */
	int atmos;
} JfGateTrack;

typedef struct {
	char kind[16]; /* Movie, Series, BoxSet, Episode... */ char imdb[24], tmdb[16], tvdb[16];
	char official[16];  /* PG-13, 15, TV-MA... */
	float critic;       /* Rotten Tomatoes percent when the server has it, else 0 */
	float community;    /* out of 10, else 0 */
	char tagline[200];
	char directors[96];
	char studio[96];    /* the first two studios, comma separated */
	char cast[280];
	int width, height;
	char video_codec[16];
	char range[24];      /* "SDR", "HDR10", "HDR10+", "HLG", "Dolby Vision" */
	char range_type[32]; /* the server's own word: DOVIWithHDR10, DOVIWithEL... */
	int dv_profile, dv_level;
	int dv_el;           /* a Dolby Vision enhancement layer is present */
	int bit_depth;
	float fps;
	int video_bitrate;
	char container[24];
	long long file_size;
	int total_bitrate;
	char media_source[48];
	int default_audio;   /* stream index, or -1 */
	int default_subtitle;
	int track_count;
	JfGateTrack tracks[JF_GATE_TRACKS];
} JfGateDetails;

/* Asks for a title's details (once: asking again is free). Returns 1 when they are here, 0 while they
 * are on their way, -1 when the server would not say. */
int jf_gate_details_request(int gate_index);
/* Copies them out: 1 when here, 0 while loading, -1 when failed. */
int jf_gate_details_get(int gate_index, JfGateDetails *out);

/* ---- inside a title: a series' seasons, a season's episodes, a collection's titles ----
 * The children are listed by one more category (kind 6) that has no tab. Opening a title there starts it over. */
/* Lists the children of the title at this gate index; returns the category that holds them, or -1. */
int jf_gate_browse_open(int gate_index);
/* A series' seasons, for the season choice on its page. Asking again is free. */
#define JF_GATE_SEASONS 40
typedef struct {
	char name[64];
	int number;   /* 0 for Specials */
	int episodes;
	int unplayed;
} JfGateSeason;
/* 1 when they are here, 0 while on their way, -1 when the server would not say */
int jf_gate_seasons_request(int gate_index);
/* Copies up to max out; returns how many (0 while loading) */
int jf_gate_seasons_get(int gate_index, JfGateSeason *out, int max);
/* Opens one season's episodes straight away (the series' other level is skipped); returns the category or -1 */
int jf_gate_season_open(int gate_index, int season);

/* One level back: 1 when the list now shows the parent's children again, 0 when there is nothing to go back to. */
int jf_gate_browse_back(void);

/* ---- the episode after the one that played ---- */
void jf_gate_next_request(int gate_index);
int jf_gate_next_state(void); /* 0 idle, 1 asking, 2 ready, -1 there is none */
int jf_gate_next_index(void); /* its gate index (a one-title category, kind 7), when ready */
void jf_gate_next_cancel(void);

/* The position of the first title of '#', 'A'..'Z' in a library: 27 ints. Returns 0 until known,
 * 1 for the server's answer, 2 once every title is in and the offsets were counted locally. */
int jf_gate_letters(int category, int *starts);

/* One decoded poster. rgba is width*height*4 bytes from malloc; whoever takes it frees it with free(). */
typedef struct {
	int category;
	int position;
	int generation; /* of the category when the poster was asked for */
	int width;
	int height;
	unsigned char *rgba;
} JfGatePoster;
/* 1, and *out filled, when a decoded poster is waiting; 0 otherwise. */
int jf_gate_take_poster(JfGatePoster *out);
/* The program dropped this poster's texture: the worker may fetch it again later. */
void jf_gate_poster_released(int category, int position);

/* The poster of one season of a series (for the series' page while a season is being chosen). Asking for another replaces
 * the one not yet fetched. A season with no picture is simply never delivered.
 * In the poster that comes back, category is the series' gate index and position the season's place in its list. */
void jf_gate_season_poster_want(int gate_index, int season);
int jf_gate_season_poster_take(JfGatePoster *out);

/* 1 once the worker has stopped, 2 if it could not sign in or list the libraries. */
int jf_gate_finished(void);

/* ---- signing in ----
 * Until the worker has a session the screen shows the sign-in pages. The worker talks to the server;
 * the screen only asks and reads back. */
/* 0 working, 1 needs a server, 2 needs a sign-in (server known), 3 signed in */
int jf_gate_auth_state(void);
int jf_gate_auth_busy(void); /* 1 while the worker is on a request the screen made */
/* 0 the last problem ("" if none), 1 the server's name, 2 the signed-in profile, 3 the Quick Connect code, 4 the server's Quick Connect page (for a QR code) */
void jf_gate_auth_text(int which, char *out, int size);
/* The server's public profiles: returns how many; copies profile 'index' when it is in range. */
int jf_gate_auth_users(int index, char *out, int size);
/* Servers found on the network: returns how many; copies server 'index' when in range. */
int jf_gate_auth_found(int index, char *name, int name_size, char *host, int host_size, int *port);
void jf_gate_auth_discover(void);
int jf_gate_auth_discover_state(void); /* 0 idle, 1 searching, 2 done */
void jf_gate_auth_server(const char *host, int port);
void jf_gate_auth_password(const char *user, const char *pass);
void jf_gate_auth_quick_start(void);
void jf_gate_auth_quick_cancel(void);
int jf_gate_auth_quick_state(void);     /* 0 idle, 1 waiting for approval, 2 approved, -1 failed */
int jf_gate_auth_quick_available(void); /* -1 unknown, 0 off on this server, 1 on */
/* Sign out (the server stays) or, with forget_server, switch server: everything the worker built is emptied and it
 * waits for a sign-in again. 1 when done, 0 if the worker was too busy to stop. */
int jf_gate_signout(int forget_server);
/* The profile picture, once: 1 and *out filled (free rgba), else 0. None if the user has no picture. */
int jf_gate_take_avatar(JfGatePoster *out);
/* Decodes a JPEG or PNG held in memory to RGBA: 1 and *w, *h, *rgba filled (free it), else 0. */
int jf_gate_decode_image(const unsigned char *data, int size, int *w, int *h, unsigned char **rgba);
/* Names and https: a local tunnel (jf_tls.cpp). 0 and the local port, or -1; the last problem in words. */
int jf_tls_open(const char *name, int port, int tls, int *local_port);
void jf_tls_error(char *buf, int size);

#ifdef __cplusplus
}
#endif
