# Changelog

## Unreleased

## 1.0.0-beta.5 (2026-10-16)

### TV shows and collections
- The show's page has a season drop-down (with each season's episode count and what is left to watch); the page's poster follows the highlighted season.
- Cross on Episodes opens that season's episode list directly: no seasons wheel. Back returns to the show's page.
- Episode page: the show's picture behind it, the list and the summary on their own panels, the show's logo above the list, the season's own poster, and each episode's rating.
- Collections open straight to their films.
- Opening a show, season or collection keeps the tab you came from lit (it used to jump to Discover).

### Discover (Seerr)
- Circle on a title you requested cancels the request (asked twice, to be sure). Administrators may cancel any request; a refusal says why.
- Cast and film credits from Seerr, and a clearer button legend.

### Look and feel
- The Mortalfin logo: full screen at start, then it moves to the top-left corner.
- Quick Connect sign-in shows a QR code that opens the server's Quick Connect page on your phone.
- Changing category no longer flashes: the backdrop and floor of the wheel you left stay until the new wheel's colours are ready, then glide across.
- The start screen no longer shows the loading text.

## 1.0.0-beta.4 (2026-10-14)

- Discover: Options on a movie or series shows its cast; pick an actor for their films and series (with Request) and what
  they directed. Back steps up a page. Titles already in your library get a green tick.
- Discover: the button legend follows the cursor (Options for cast, Select on an actor); the sign-in keyboard shows
  button pictures; larger role text on cast pages.
- Film page: file size, studio, and an Options button for the cast (needs Seerr); Back returns to the film.
- Seasons open to an episode list with watched ticks and summaries; Cross plays from where you left off.
- Left from the first category goes to Discover, and Left from Discover carries on to the last category.
- Fixed: a brightness flash when changing category; the app could crash when Seerr did not answer.
- Pictures load faster over https: a new connection reuses the earlier TLS session, and small writes go out at once.

## 1.0.0-beta.3 (2026-10-13)

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
