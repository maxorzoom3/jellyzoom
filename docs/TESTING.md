# Testing Mortalfin

Thank you for testing. This is a beta: the most useful thing you can do is use it the way you normally watch, and tell me
what goes wrong.

## What to try

- Sign in, browse, switch to another server and back, sign out and in again.
- Play a film, an episode, something 4K HDR, something in AV1 if you have it.
- Pause, scrub with L2/R2, switch the audio track and the subtitles while a film plays.
- Subtitles on and off: text ones and picture ones (PGS from a Blu-ray rip). On a widescreen film, do they sit in the black bar?
- The Settings page, each row.
- If you have a soundbar or receiver: Dolby Digital, Dolby Digital Plus and DTS films, with Sound output on Auto and on Bitstream.
- Leave a film running for an hour, or play several in a row: does it stay smooth?

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

Start that **before** you open Mortalfin, reproduce the problem, then stop it with Ctrl+C and attach `klog.txt`.
**Look through the file first**: it can contain your server's address and the names of your films. Remove what you do not want to share.

## Sound output on Auto, Bitstream and PCM

Auto sends a sound to the soundbar as it is when your TV or soundbar says it can decode it, and decodes the rest.
Bitstream sends whatever it can. PCM decodes everything in the console. If a film has no sound or crackles, try the other two.
