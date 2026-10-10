# Mortalfin

A native Jellyfin client for the PS5, drawn with Vulkan. Browse your library on a cover wheel, play films
and shows with HDR and surround sound, all from the couch, with no browser and no web view.

> **Beta.** Mortalfin is in testing. It works, it has rough edges, and your bug reports are the point of
> this release. See [docs/TESTING.md](docs/TESTING.md).

Title ID `PPSA99141` · version in [`VERSION`](VERSION) · licence GPL-3.0 · built on the
[PS5 Vulkan Template](https://github.com/mihawk-99/PS5_VulkanTemplate) and
[BlackBearReloaded's UI kit](https://github.com/blackbearreloaded).

## What it does

- **Your Jellyfin server, on the console.** Type the server's address, sign in, and your libraries appear:
  films, shows, seasons and episodes, continue watching, next up, search.
- **Cover-wheel interface.** Posters and backdrops on a wheel you turn with the stick or the d-pad.
- **Playback.** Glass control bar with the title's logo, chapters, and scrubbing on L2/R2. Resumes where you left off,
  plays the next episode if you ask it to.
- **Picture.** HDR10 goes out as HDR10. Dolby Vision plays as its HDR10 layer. Other HDR is tone-mapped.
  AV1 plays too (dav1d).
- **Sound.** Dolby Digital, Dolby Digital Plus (so Atmos in Dolby Digital Plus) and DTS can be sent to your
  soundbar untouched. Everything else is decoded to PCM, up to 7.1.
- **Subtitles.** Text subtitles from the server, picture subtitles (PGS, DVD) drawn by the app, and on a widescreen film
  they sit in the black bar when they fit.
- **Settings.** Sound output, soundbar delay, audio and subtitle language, subtitle size, next episode automatically,
  HDR brightness, optional [Seerr](https://github.com/seerr-team/seerr) (discover and request), and an About page.

## What it does not do (yet)

- **Plain `http` only**, on your own network. There is no `https` support in this build, so use an address like
  `192.168.1.10:8096`, not a public name behind a reverse proxy.
- **Dolby TrueHD and DTS-HD Master Audio are decoded to PCM**, not sent as they are: the PS5's audio output does not
  carry them at full speed. You get the lossless sound, without the soundbar's Atmos label for TrueHD films.
- No download or offline mode, no live TV, no music library, no background music.

## Installing

You need a PS5 that runs homebrew apps. Mortalfin was developed on firmware 4.03.

1. Get `Mortalfin-<version>.zip` from the [Releases](../../releases) page.
2. Unzip it. You get one folder named `PPSA99141`.
3. Install that folder with the homebrew app installer you normally use. Once Mortalfin is in the
   Homebrew Store catalog, you can install it from there.
4. Start Mortalfin from the home screen, enter your server's address, and sign in.

Mortalfin keeps its settings in the console's `/data` folder (files starting `jf-`). Deleting them resets Mortalfin.

## Reporting a bug

Please [open an issue](../../issues/new?template=bug_report.md). The template asks for what you need to say;
[docs/TESTING.md](docs/TESTING.md) says what to try and how to capture the log.

## Building it yourself

See [docs/BUILDING.md](docs/BUILDING.md). Releasing: [docs/RELEASING.md](docs/RELEASING.md).

## Licence

Mortalfin as distributed is under the **GPL-3.0** ([LICENSE.md](LICENSE.md)), because it links GPL-3.0 parts
(the UI kit, the PS5 platform layer). Parts under other licences keep them: Sascha Willems' Vulkan examples (MIT,
[ps5/licenses/MIT-VulkanExamples.md](ps5/licenses/MIT-VulkanExamples.md)), Dear ImGui, libktx, tinygltf, volk,
dav1d (BSD-2-Clause), the kit's fonts (OFL-1.1, Bitstream Vera) and others. Every part, its licence and its
source are listed in the `licenses/` folder inside the title and in `ps5/ASSETS.md`.

Mortalfin is not affiliated with or endorsed by Jellyfin or Sony Interactive Entertainment.
"PlayStation" and "PS5" are trademarks of Sony Interactive Entertainment Inc. Jellyfin is a trademark of its project.
It contains no games, no firmware and no keys, and plays only what is on your own server.
