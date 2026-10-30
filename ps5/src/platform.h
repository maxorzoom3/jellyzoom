/*
 * PS5 Vulkan Template - the console: klog, the splash, the pad, sound, time, the exit.
 *
 * Copyright (C) 2026 Mihawk
 *
 * This code is licensed under the MIT license (MIT) (http://opensource.org/licenses/MIT)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standard error into klog (each line prefixed with the title's name), and
 * the shell's splash dismissed. Call first. */
void platform_init(const char *title_name);

/* One line to klog, flushed. */
void say(const char *format, ...) __attribute__((format(printf, 1, 2)));

double now_seconds(void);

/* The pad, in the console's button numbering. */
enum {
   PAD_L3 = 0x000002,
   PAD_R3 = 0x000004,
   PAD_OPTIONS = 0x000008,
   PAD_UP = 0x000010,
   PAD_RIGHT = 0x000020,
   PAD_DOWN = 0x000040,
   PAD_LEFT = 0x000080,
   PAD_L1 = 0x000400,
   PAD_R1 = 0x000800,
   PAD_TRIANGLE = 0x001000,
   PAD_CIRCLE = 0x002000,
   PAD_CROSS = 0x004000,
   PAD_SQUARE = 0x008000,
   PAD_TOUCH_PAD = 0x100000,
};

struct pad {
   uint32_t held;    /* buttons down now */
   uint32_t pressed; /* buttons that went down since the last poll */
   float left_x, left_y, right_x, right_y; /* -1..1, a dead zone removed; y is down-positive */
   float l2, r2;                           /* 0..1 */
};

/* Up to four players, one per signed-in user: the console pairs each controller
 * with a user. Player 0 is the user who started the title and stays theirs;
 * pad_poll reads the signed-in users again about once a second, a new user's
 * controller takes the first free player, and a player is let go when their user
 * signs out. pad_poll, pad_readings, pad_vibrate and pad_light_bar are player
 * 0's; the pad_player calls reach the others. */
#define PAD_PLAYERS 4

/* Open player 0's pad (the first user's), and every other signed-in user's;
 * false (and no input for player 0) when there is none. */
bool pad_open(void);
/* Read every player's controller; *pad is player 0's. Call once a frame. */
void pad_poll(struct pad *pad);
/* The players whose controller reported itself connected, a bit each (bit 0 is
 * player 0), as of the last pad_poll. */
uint32_t pad_players(void);
/* A player's pad as the last pad_poll read it, `pressed` against the held
 * buttons *pad had; false, and a pad at rest, when that player has none. */
bool pad_player(int player, struct pad *pad);

/* Set in a reading's buttons while the shell has the pad (the home screen, a
 * system dialog): that reading is not input for the title. */
#define PAD_INTERCEPTED 0x80000000u

/* One reading as the console took it, in its numbering. */
struct pad_reading {
   uint32_t buttons;
   uint8_t left_x, left_y, right_x, right_y; /* 0..255, 128 at rest; y is down-positive */
   uint8_t l2, r2;                           /* 0..255 */
   bool connected;
   uint64_t timestamp_us;
};

/* Every reading the last pad_poll took, oldest first (the console keeps up to
 * 64 between polls), for input models that must see a tap shorter than a
 * frame. Returns their number; *readings stays valid until the next poll. */
int pad_readings(const struct pad_reading **readings);
int pad_player_readings(int player, const struct pad_reading **readings);

enum {
   REMOTE_PLAY = 1u << 0,
   REMOTE_PAUSE = 1u << 1,
   REMOTE_PLAY_PAUSE = 1u << 2,
};
/* HDMI-CEC remote readings and media-key presses from the last pad_poll. */
int remote_readings(const struct pad_reading **readings);
uint32_t remote_media_pressed(void);

/* Rumble: the large and the small motor, 0..1 each; 0, 0 stops it. */
void pad_vibrate(float large, float small);
void pad_player_vibrate(int player, float large, float small);
/* The light bar's colour. */
void pad_light_bar(uint8_t r, uint8_t g, uint8_t b);
void pad_player_light_bar(int player, uint8_t r, uint8_t g, uint8_t b);

/* Sound: one 48 kHz output of interleaved stereo 16-bit samples. A thread of
 * its own calls fill for 256 frames at a time and blocks while the console
 * plays them, so fill paces itself and must never wait on the render loop.
 * false when there is no output (the host reference build has none). */
typedef void (*audio_fill_fn)(int16_t *frames, int count, void *user);
bool audio_start(audio_fill_fn fill, void *user);
/* Stops the thread and closes the output; call it before what fill uses goes. */
void audio_stop(void);

/* Surround: while a film's sound has more than two channels the output is opened with eight (7.1 PCM: FL FR FC LFE BL BR SL SR)
 * instead of two, and closed back to stereo afterwards. fill8 writes 256 frames of eight interleaved 16-bit channels (the
 * same contract as audio_fill_fn, `count` frames); want(user) says, before each grain, how many channels the output should
 * have now (2 or 8). The console may refuse eight channels: the output then stays stereo. */
typedef int (*audio_want_fn)(void *user);
void audio_set_surround(audio_fill_fn fill8, audio_want_fn want, void *user);

#ifdef __cplusplus
}
#endif
