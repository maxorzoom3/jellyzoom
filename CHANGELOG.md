# Changelog

## 1.0.0-beta.3 (not released yet)

- Added: https and domain names. Enter `https://your.domain` on the setup screen to connect through a reverse proxy
  (Traefik, Caddy, nginx) from outside your network; plain IP addresses and `http://` still work.
- The app does its own DNS lookups for names (`/data/jf-dns`, `/data/jf-hosts`), so a DNS server that blocks Sony is respected.
- Certificates are checked against Mozilla's root list and `/data/jf-ca.pem`; `/data/jf-tls-insecure` turns the check off.
- Seerr works with an https address too.
- Credits: Mbed TLS (Apache-2.0) and Mozilla's root certificates (MPL-2.0).
- Known: pictures load more slowly over the internet than on a LAN (one secure connection each); a server that only offers
  TLS 1.3 is not supported yet.

## 1.0.0-beta.2
- Fixed: accented letters (Czech, Polish, Hungarian, Turkish and more) and Cyrillic now show in the UI and in subtitles
- Changed: the app now sits under Media on the home screen

## 1.0.0-beta.1

The first test build. Mortalfin is a native Jellyfin client for the PS5, drawn with Vulkan.

- Sign in to your Jellyfin server from the console (address, then username and password); switch server or sign out from the menu.
- Browse libraries on the cover wheel: films, shows, seasons, episodes, continue watching, next up, search.
- Film and episode playback with a glass control bar, the title's logo, chapters and scrubbing on L2/R2.
- HDR10 pictures are passed through; Dolby Vision plays as its HDR10 layer; other HDR is tone-mapped; AV1 plays (dav1d).
- Sound: Dolby Digital, Dolby Digital Plus (including Atmos) and DTS can be sent to the soundbar as they are; everything else is decoded to PCM up to 7.1.
- Subtitles: text subtitles from the server, picture subtitles (PGS, DVD) drawn by the app, in the black bar of a wide film when they fit.
- Settings: sound output, soundbar delay, audio and subtitle language, subtitle size, next episode automatically, HDR brightness, optional Seerr (discover and request), About.
- Plain http on your own network only (no https yet).
