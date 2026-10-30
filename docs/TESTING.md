# Testing Jellyzoom

Thank you for testing. This is a beta: the most useful thing you can do is use it the way you normally watch, and tell me
what goes wrong.

## What to try

- Sign in, browse, switch to another server and back, sign out and in again.
- In Continue Watching and Next Up, check that episodes from different seasons show the matching season poster while
  retaining the series name and episode details. If a season has no poster, its previous series (or episode) artwork
  should be shown instead. Movies and posters in other sections should remain unchanged.
- On the main wheel, use L1/R1 to move between the top sections with or without the tabs focused; check the first and
  last sections and the optional Discover tab. Continue right to the profile icon and open Settings; move back left
  from the profile. Repeat with the tabs focused and with Discover enabled and disabled, including entering Discover
  from the first section. The press must not also move a cover. Verify letter selection with Square
  and that L1/R1 keep their existing behavior in item details, episode lists, overlays and playback.
- Play a film, an episode, something 4K HDR, something in AV1 if you have it.
- On an episode with Intro Skipper metadata, check that Skip Intro appears only during the intro and that Triangle skips only when pressed.
  Rewind into the intro after skipping or playing past it; also rewind before it and let playback reach it again. The prompt should reappear.
- On an episode with outro metadata, check that Skip Outro appears only during the outro. Triangle skips to the end of the segment;
  when it ends within one second of the file's end, Jellyfin marks it watched and the next episode starts.
  With no next episode, or with `/data/jellyzoom-no-autoplay`, it returns to the library; movies never start a next episode.
  If marking fails, it returns to the library and shows an error. Check a failed or timed-out next-episode lookup and cancel it with Back.
  Also try an outro followed by more content, replay an outro after rewinding, and check an episode without an outro.
- Scrub with L2/R2 and confirm a trickplay preview appears when the server has generated sheets; also try without sheets.
  Seek with left/right while the scrubber is focused and confirm the preview remains briefly after either kind of seek.
  If previews are missing, the log reports whether trickplay metadata is available and any sheet request or decoding errors.
- Pause, switch the audio track and the subtitles while a film plays.
- With HDMI-CEC enabled, use the TV remote to navigate the library and playback controls, confirm and go back.
  Try play, pause, rewind and fast-forward if the remote provides them, then verify the DualSense still works,
  including while the TV remote is connected. Check that inputs are not delivered while a system overlay is open.
  During an intro or outro prompt, remote OK skips the segment; controller Triangle still skips and Cross retains its
  usual controls behavior. Outside the prompt, remote OK keeps its normal behavior; an open panel takes priority.
- Subtitles on and off: text ones and picture ones (PGS from a Blu-ray rip). On a widescreen film, do they sit in the black bar?
- The Settings page, each row.
- If you have a soundbar or receiver: Dolby Digital, Dolby Digital Plus and DTS films, with Sound output on Auto and on Bitstream.
- Leave a film running for an hour, or play several in a row: does it stay smooth?
- Start and resume a 4K HDR episode, including with Dolby Digital Plus bitstream audio, and check that sound and video
  begin together without a burst of late drops. Repeat with SDR, HDR disabled, subtitles, pause and seeking.
- On a 120 Hz HDR-capable display, HDR playback first tries to keep the 120 Hz output mode. Check the TV's signal
  information and compare picture colour and motion with the 59.94 Hz path, selected by creating `/data/jellyzoom-hdr-60`.
  Buffer acceptance alone does not prove that the TV received HDR at 120 Hz. If the HDR buffer is refused, the normal
  59.94 Hz path is tried instead. Also test a display that starts at 60 Hz and playback with HDR disabled.

## What to tell me

Use the [bug report form](../../issues/new?template=bug_report.md). The useful parts are:

- the version (Settings > About) and your console's firmware
- what you did, in order, and what you expected
- the file: container, video and sound codecs, subtitles (Jellyfin's "Media info" shows them)
- your TV and sound setup, if it is a picture or sound problem

## The log

The app writes a log to the console's debug port. To capture it, on a computer on your network:

```bash
nc <console address> 3232 | tee klog.txt
```

Start that **before** you open Jellyzoom, reproduce the problem, then stop it with Ctrl+C and attach `klog.txt`.
**Look through the file first**: it can contain your server's address and the names of your films. Remove what you do not want to share.
For playback stutter, the `PLAYER PERF` lines summarize the decoder, conversion times, late drops and video queues.
The dropped-pictures counter counts decoded pictures more than 300 ms behind the audio clock; it does not count every
picture skipped during presentation.
`PLAYER PRESENT` reports pictures selected and skipped by the render loop, plus gaps between render calls and between
picture selections. These are application timings, not measurements of actual display flips. Seek, pause and buffering
reset the measurement window. At 59.94 Hz, approximately 24 fps video normally alternates between two and three refreshes.

## Playback metadata checks

On a Linux host with a C++17 compiler and the json-c development package installed, run:

```bash
python3 ps5/tests/branding.py
c++ -std=c++17 ps5/tests/playback_features.cpp ps5/src/jf/jf_playback_features.cpp $(pkg-config --cflags --libs json-c) -o /tmp/mortalfin-playback-tests
/tmp/mortalfin-playback-tests
c++ -std=c++17 ps5/tests/subtitle_text.cpp -o /tmp/mortalfin-subtitle-tests
/tmp/mortalfin-subtitle-tests
c++ -std=c++17 -DJF_RELEASE -ffunction-sections -fdata-sections -I ps5/src -I external/tinygltf \
  ps5/tests/season_posters.cpp $(pkg-config --cflags --libs json-c) -pthread -Wl,--gc-sections -o /tmp/mortalfin-season-poster-tests
/tmp/mortalfin-season-poster-tests
cc -std=c11 -DJF_RELEASE -ffunction-sections -fdata-sections -I ps5/src -I ps5/src/jf \
  ps5/tests/mark_played.c ps5/src/jf/jellyfin.c $(pkg-config --cflags --libs json-c) \
  -Wl,--gc-sections -o /tmp/mortalfin-mark-played-tests
/tmp/mortalfin-mark-played-tests
cc -std=c11 ps5/tests/http_methods.c ps5/src/jf/http_client.c -o /tmp/mortalfin-http-tests
/tmp/mortalfin-http-tests
c++ -std=c++17 -ffunction-sections -fdata-sections -I ps5/src \
  ps5/tests/hdr_refresh.cpp ps5/src/jf/jf_hdrlab.cpp \
  -Wl,--gc-sections -Wl,--wrap=access -Wl,--wrap=usleep -o /tmp/mortalfin-hdr-refresh-tests
/tmp/mortalfin-hdr-refresh-tests
cc -std=c11 -D_POSIX_C_SOURCE=200809L -ffunction-sections -fdata-sections \
  -idirafter .deps/native/ps5-payload-sdk/target/include ps5/tests/cec_remote.c \
  -Wl,--gc-sections -o /tmp/mortalfin-cec-tests
/tmp/mortalfin-cec-tests
```

On the console, check text subtitles in SDR and HDR against the same track on another player: invisible direction
marks should not appear as question marks, and the outline should remain legible on light and dark scenes. A real
question mark and other visible punctuation must remain visible.

For an intro longer than eight seconds, check that the skip prompt fills over eight seconds of playback and disappears
when the controls are hidden. Bring the controls back and skip using Triangle or remote OK; the first OK when controls
are hidden should only wake them. Check pause/buffering, seeking into and out of the intro, rewinding after a skip, short
intros, and that the outro prompt and next-episode behavior are unchanged. Both prompts should sit at the lower right,
clear of the scrubber when the controls are visible. Skipping with Triangle or remote OK must not wake controls that
were hidden automatically or with Circle; if they were already visible, leave them visible. Check an in-progress scrub
does not override a skip. Long text and picture subtitles should remain legible and not overlap the prompt with the
controls both hidden and visible.

On a movie details page and an episode's own page (Options on the episode list), switch the watch status in both
directions. Check the server and other clients, Continue Watching, Next Up and the unwatched filter. With the server
unavailable, the status must remain unchanged and an error must appear. Marking watched or unwatched clears the
item's resume position.

## Sound output on Auto, Bitstream and PCM

Auto sends a sound to the soundbar as it is when your TV or soundbar says it can decode it, and decodes the rest.
Bitstream sends whatever it can. PCM decodes everything in the console. If a film has no sound or crackles, try the other two.
