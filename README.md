<p align="center">
  <img src="docs/media/icon.png" alt="Mortalfin" width="180"><br>
  <b>A native Jellyfin client for jailbroken PS5 consoles.</b><br>
  Its own GPU-drawn interface, HDR playback, surround-sound passthrough, and no browser in between.
</p>

<p align="center">
  <a href="../../releases/latest"><img alt="Release" src="https://img.shields.io/github/v/release/mortaljinx/mortalfin?include_prereleases&label=release&color=f08a24"></a>
  <img alt="Platform: PS5" src="https://img.shields.io/badge/platform-PS5%20(jailbroken)-4b5bdc">
  <img alt="Tested firmware: 4.03" src="https://img.shields.io/badge/tested%20on-firmware%204.03-2a7ae2">
  <img alt="Jellyfin" src="https://img.shields.io/badge/Jellyfin-client-7b5cd6">
  <img alt="Seerr" src="https://img.shields.io/badge/Seerr-requests-3aa6b9">
  <img alt="Vulkan" src="https://img.shields.io/badge/graphics-Vulkan%20(RADV)-ac162c">
  <img alt="License: GPL-3.0" src="https://img.shields.io/badge/license-GPL--3.0-555">
  <img alt="Status: beta" src="https://img.shields.io/badge/status-beta-c06">
  <a href="https://discord.gg/PmSvYChzd"><img alt="Discord" src="https://img.shields.io/badge/chat-Discord-5865F2?logo=discord&logoColor=white"></a>
</p>

<p align="center">
  <a href="../../releases"><img alt="Downloads" src="https://img.shields.io/github/downloads/mortaljinx/mortalfin/total?color=44aa66"></a>
  <a href="../../issues"><img alt="Open issues" src="https://img.shields.io/github/issues/mortaljinx/mortalfin?color=cc8844"></a>
  <a href="../../stargazers"><img alt="Stars" src="https://img.shields.io/github/stars/mortaljinx/mortalfin?style=flat&color=ddaa22"></a>
  <img alt="Last commit" src="https://img.shields.io/github/last-commit/mortaljinx/mortalfin?color=888">
</p>

<!--
Screenshots: add them under docs/media/ and un-comment.

<p align="center">
  <img src="docs/media/screenshot-wheel.webp" alt="The cover wheel" width="100%">
</p>
-->

Mortalfin brings your Jellyfin library to the PS5 as a real app: a cover-wheel interface drawn on the GPU with
Vulkan and made for the TV, and video played from your own server with HDR and your surround sound intact. With
[Seerr](https://github.com/seerr-team/seerr) you can also request new films and series without leaving the couch.

> **Beta.** Mortalfin is in testing. It works, it has rough edges, and your bug reports are the point of this
> release. See [docs/TESTING.md](docs/TESTING.md).

**Contents:** [Features](#features) · [Installation](#installation) · [Controls](#controls) · [Seerr](#seerr-optional) ·
[Formats and limits](#formats-and-limits) · [Troubleshooting](#troubleshooting) · [Privacy](#privacy) ·
[Building](#building) · [Credits](#credits) · [Licence](#licence) · [Disclaimer](#disclaimer)

## Features

**Browsing**
- Sign-in by server list or typed address, then by user and password, or **Quick Connect** (scan the code, tap *Authorize*)
- Libraries as a cover wheel you turn with the stick or the D-pad: films, shows, seasons and episodes
- Continue watching, next up, favourites (△ on a title) and search (✕ on the search button) across your libraries;
  episodes in Continue Watching and Next Up use their season posters when available
- A glass look throughout, with sound effects and music from the UI kit

**Playback**
- Direct play from your server: H.264, HEVC and AV1 (dav1d), with HDR10 sent to the display as HDR10
- Dolby Vision plays as its HDR10 layer; other HDR is tone-mapped, and an in-player brightness control for HDR (L1/R1)
- Audio and subtitle tracks, switched while the film plays
- Text subtitles from the server and **picture subtitles (PGS, DVD)** drawn by the app, placed in the black bar of a
  widescreen film when they fit
- Chapters, scrubbing on L2/R2, resume where you left off, and the next episode automatically if you want it
- A playback-info panel (Options) with codec, range, peak brightness and what the sound output is doing

**Sound**
- **Bitstream to your soundbar or receiver (optional):** Dolby Digital, Dolby Digital Plus (so Atmos in
  Dolby Digital Plus) and DTS core go out untouched
- Everything else is decoded to PCM up to 7.1, including Dolby TrueHD and DTS-HD Master Audio (lossless)
- A soundbar delay to line the picture up with the sound
- Three output modes: *Auto*, *Bitstream* and *PCM*

**Settings**
- Sound output and delay, audio and subtitle language, subtitle size, next episode automatically, HDR brightness,
  Seerr, and an About page with the version

**Requests with Seerr** (optional)
- Discover and request films and series from the same app

## Installation

### What you need

- A **jailbroken PS5** that runs homebrew apps. Mortalfin was developed and tested on **firmware 4.03**; other firmware
  with the same tools may work. If yours does (or doesn't), please [say so in an issue](../../issues).
- A way to put a folder on the console: **[ps5upload](https://github.com/phantomptr/ps5upload)** (recommended), or an
  FTP client such as FileZilla talking to the console's FTP server.
- A **Jellyfin** server on your network that the PS5 can reach.

### Install from the Homebrew Store

Once Mortalfin's entry is in the [PS5 homebrew catalog](https://github.com/blackbearreloaded/ps5-homebrew-catalog),
install it from the Prospero Homebrew Store like any other title. No files to copy. *(Pending: the listing is being
submitted. Until it appears, use the manual install below.)*

### Install by hand

1. Download `Mortalfin-<version>.zip` from the [latest release](../../releases/latest) and unzip it. Inside is one
   folder called `PPSA99141`.
2. Start your jailbreak and its homebrew loader (and your FTP server, if you use FTP).
3. Upload the `PPSA99141` folder to `/data/homebrew/` on the console, so that
   `/data/homebrew/PPSA99141/eboot.bin` exists. Create `/data/homebrew` first if it isn't there.
4. After a moment, a **Mortalfin** tile appears on the home screen. If it doesn't, run your payloads again, or reboot
   and jailbreak again.
5. Open Mortalfin, pick your server from the list or type its address (for example `192.168.1.10:8096`), and sign in.
   **Quick Connect** is quickest: scan the QR code with your phone and tap *Authorize*.

Mortalfin remembers your accounts and settings.

### Update

1. **Close Mortalfin completely first** (PS button, then close it). Replacing files under a running app can crash the
   console.
2. Upload the new folder's files **over** the old ones in `/data/homebrew/PPSA99141/`. Don't delete the old folder and
   copy in a new one, and never keep a second folder with the same ID under `/data/homebrew`.

Via the Homebrew Store, update from the store.

### Uninstall

Close Mortalfin and delete `/data/homebrew/PPSA99141` (or remove it in the store). Its settings are small files in the
console's `/data` folder whose names start with `jf-`; delete them to reset Mortalfin, or leave them for a reinstall.
Nothing else on the console is touched.

## Controls

| Button | In the menus | In the player |
| --- | --- | --- |
| ✕ | Select | Select, play / pause |
| ○ | Back | Hide the controls |
| D-pad / left stick | Move | Move on the control bar |
| △ | Favourite the highlighted title | Audio and subtitles |
| L2 / R2 | | Scrub back / forward |
| L1 / R1 | Switch main sections | HDR brightness |
| Options | | Playback info |
| Hold Options (1 s) | Leave to the menu | Leave to the menu |

On a movie or episode's details page, use **Mark as** to mark it watched or unwatched.
From a season's episode list, press Options to open that episode's details. Changing the watch status clears its
resume position.

When an intro skip prompt appears at the lower right, press Triangle or TV-remote OK to skip. The prompt fills and
hides after eight seconds of playback; bring up the playback controls to show it again while the intro is still playing.
Skipping does not wake hidden playback controls. Outro prompts use the same position and controls.

## Seerr (optional)

With [Seerr](https://github.com/seerr-team/seerr) (the successor of Overseerr and Jellyseerr), Mortalfin can show
what your library is missing and let you request it. Seerr needs Jellyfin as its media server, your user imported, and
permission to request.

Turn it on under *Settings → Seerr*. Use Seerr's address **on your own network** (for example
`http://192.168.1.10:5055`); a public name that only works from outside your home won't work from the PS5. *Test
connection* checks the address and your sign-in. Mortalfin keeps only Seerr's session, never a password.

## Formats and limits

| | Plays | Notes |
| --- | --- | --- |
| Video | H.264, HEVC (Main, Main 10), AV1 | AV1 is decoded in software (dav1d) |
| HDR | HDR10 as HDR10; Dolby Vision as its HDR10 layer | Other HDR is tone-mapped. Dolby Vision profile 5 and FEL (profile 7) still need more testing |
| Audio | AAC, AC-3, E-AC-3, TrueHD, DTS (including DTS-HD MA), FLAC, Opus, MP3 and more | PCM up to 7.1. With *Bitstream* on, Dolby Digital, Dolby Digital Plus and DTS go to the soundbar as they are |
| Subtitles | SRT, ASS/SSA, WebVTT; PGS and DVD pictures | Embedded or from the server |

Limits of the platform, and of this beta:

- **Dolby TrueHD and DTS-HD MA are decoded to PCM, never sent as they are.** The PS5's audio output can't carry them
  at full speed (I tried): you get the lossless sound, without the soundbar's Atmos label on a TrueHD film.
  Atmos in Dolby Digital Plus does pass through.
- **Plain `http` only**, on your own network, for now. Use an address like `192.168.1.10:8096`; a public name behind a
  reverse proxy won't connect.
- No true 24p output: the display runs at the console's own refresh rate.
- No downloads or offline mode, no live TV, no music library, no background music.
- The app can't quit itself: close it with the PS button.

## Connecting with a domain name (https)

The setup screen takes an IP address, a name, or an `https://` address. If your server sits behind a reverse proxy
(Traefik, Caddy, nginx), enter the address you use in a browser, for example `https://jellyfin.example.com`; with no port
it uses 443. This lets people who are not on your network connect.

- The app opens a small tunnel inside the console: it looks the name up with its own DNS client (so a DNS server that
  blocks Sony still blocks Sony), does the TLS, and hands the rest of the app plain traffic on the console itself.
- TLS 1.2, with the certificate and its name checked against Mozilla's root list. Let's Encrypt works. A server that only
  offers TLS 1.3 does not connect yet.
- The console's clock has to be right, or every certificate looks expired or not yet valid.
- Pictures load a little slower over the internet than on your LAN, because each one is a separate secure connection.

Files you can put on the console (in `/data`; they are read when the app starts, so restart it after a change):

| File | What it does |
| --- | --- |
| `/data/jf-dns` | The DNS server to ask, as an address on one line, when the console's own does not answer |
| `/data/jf-hosts` | Fixed names, one per line: `192.168.1.10 jellyfin.example.com` |
| `/data/jf-ca.pem` | Extra trusted roots, for a server whose certificate comes from your own authority |
| `/data/jf-tls-insecure` | Any content: turns certificate checking off. For testing only; the app logs a warning |

Nothing of yours is in the download: the address, user name and keys you enter are stored on your own console.

## Troubleshooting

| Problem | What to do |
| --- | --- |
| No Mortalfin tile | Check the path is exactly `/data/homebrew/PPSA99141/eboot.bin`, then rerun your payloads, or reboot and jailbreak again. |
| The upload fails ("Text file busy") | Mortalfin is still running: close it first. |
| Your server isn't listed | Type its address. Discovery needs UDP port 7359; in Docker, publish `7359/udp`, and in Jellyfin turn on *Enable auto discovery*. |
| It can't connect | The PS5 can't resolve names: use the server's IP address and port (for example `192.168.1.10:8096`), over `http`. |
| A film has no sound or crackles | Try another *Sound output*: Auto, Bitstream or PCM. Include the film's audio codec in a report. |
| The receiver shows PCM, not Dolby | Turn on *Bitstream*. Only Dolby Digital, Dolby Digital Plus and DTS core can be sent as they are. |
| Subtitles don't show | Switch them on with △ in the player; picture subtitles (PGS) appear once chosen. |
| A title stutters | Over Wi-Fi, a high-bitrate 4K file may be too much; try a wired connection. Press Options for the playback info and include it in a report. |
| Seerr isn't answering | Use Seerr's local address (port 5055 by default) and press *Test connection*. |

**Reporting a problem:** open an [issue](../../issues/new?template=bug_report.md) with your firmware, your server and
its version, what you did and what happened. For sound or picture problems, include the file's codecs and your TV or
soundbar. [docs/TESTING.md](docs/TESTING.md) says what to try and how to capture the log.

## Privacy

Mortalfin talks to your Jellyfin server and, if you turn it on, your Seerr server, and nothing else. It has no
analytics, no accounts of its own and no ads. The release build contains no server address, user name or password:
you type them in, and they are kept on your console only. Each console makes its own random device ID the first time it
runs (`/data/jf-device-id`). The debug log is sent only to a computer that you connect to the console's debug port.

## Community and testing

Join the [Mortalfin Discord](https://discord.gg/PmSvYChzd) to try the beta, ask for help, send bug reports with your klog, and follow new releases.

## Building

See [docs/BUILDING.md](docs/BUILDING.md). Releasing and the security check: [docs/RELEASING.md](docs/RELEASING.md).

## Credits

- Mbed TLS (Apache-2.0) and Mozilla's root certificate list (MPL-2.0) carry the https connection; their licences are in
  `ps5/licenses/` and in the `licenses/` folder beside `eboot.bin`.
Mortalfin stands on the work of others in the PS5 scene and beyond. Thank you to:

- **[Mihawk](https://github.com/mihawk-99)**, for the [PS5 Vulkan Template](https://github.com/mihawk-99/PS5_VulkanTemplate)
  this title is built on, and the PS5 Vulkan stack under it: the RADV link recipe, the C runtime start-up and the AGC
  import stubs, the platform layer, and the Mesa port
- **[BlackBearReloaded](https://github.com/blackbearreloaded)**, for
  [ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui), the UI kit behind the glass interface, its
  sounds and music, and for the [PS5 homebrew catalog](https://github.com/blackbearreloaded/ps5-homebrew-catalog)
- **[John Törnblom](https://github.com/ps5-payload-dev) and the ps5-payload-dev contributors**, for the
  [PS5 payload SDK](https://github.com/ps5-payload-dev/sdk) that makes any of this possible
- **[Sascha Willems](https://github.com/SaschaWillems/Vulkan)**, for the Vulkan examples and base class the title grew
  from (MIT)
- **[Mesa](https://www.mesa3d.org) and RADV**, the Vulkan driver, with ACO and NIR
- **[FFmpeg](https://ffmpeg.org)**, for demuxing and decoding, and **[dav1d](https://code.videolan.org/videolan/dav1d)**
  (VideoLAN), for AV1
- **[Dear ImGui](https://github.com/ocornut/imgui)**, **[tinygltf](https://github.com/syoyo/tinygltf)**,
  **[volk](https://github.com/zeux/volk)**, **[KTX-Software](https://github.com/KhronosGroup/KTX-Software)** (Khronos),
  **[stb](https://github.com/nothings/stb)** by Sean Barrett, and **LLVM**'s libc++, libc++abi and compiler-rt
- The typefaces **Inter**, **Montserrat**, **DejaVu Sans Mono**, **Press Start 2P**, **Patrick Hand** and **Roboto**
- The **[Jellyfin](https://jellyfin.org)** project and its API, and the **[Seerr](https://github.com/seerr-team/seerr)**
  team (Seerr, Jellyseerr and Overseerr before it)

Every component, its licence, its copyright and where its source is are listed in the `licenses/` folder inside the app
(`LEGAL.txt` and `licenses/README.txt`) and in [`ps5/ASSETS.md`](ps5/ASSETS.md).

## Licence

Mortalfin as distributed is under the **GNU General Public License v3.0** ([`LICENSE.md`](LICENSE.md)), because it links
GPL-3.0 parts (the UI kit and the PS5 platform layer). Parts under other licences keep them: Sascha Willems' Vulkan
examples (MIT, [`ps5/licenses/MIT-VulkanExamples.md`](ps5/licenses/MIT-VulkanExamples.md)), Dear ImGui, tinygltf,
volk, KTX, dav1d (BSD-2-Clause), Mesa (MIT), the fonts (OFL-1.1, Apache-2.0, Bitstream Vera) and others. Their licence
texts ship inside the app, and source for the GPL parts is offered with each release (`SOURCES.txt`).

## Disclaimer

Mortalfin is an unofficial homebrew project, provided **as is, without any warranty** of any kind, express or implied,
as set out in sections 15 and 16 of the GPL. **You use it entirely at your own risk.** The author and contributors are
not responsible or liable for any damage, data loss, malfunction, account or online-service consequences, or any other
harm, to your console, your devices, your data or anything else, arising from installing, using or being unable to use it.

Running homebrew requires a jailbroken console, which may break Sony's terms of service and void your warranty. Whether
you do so is your choice and your responsibility.

Mortalfin contains no media, no games, no firmware and no keys, and gives access to no content of its own: it plays what
is on your own Jellyfin server. Use it only with media you have the right to watch.

Mortalfin is not affiliated with or endorsed by Sony Interactive Entertainment or the Jellyfin project. *PlayStation*,
*PS5* and *DualSense* are trademarks of Sony Interactive Entertainment Inc. *Jellyfin* is a trademark of its project.
