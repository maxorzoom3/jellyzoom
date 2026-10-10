# Building Mortalfin

Mortalfin builds against a PS5 stack checked out beside it (all under one folder):

- `PS5_Vulkan`: the RADV release archive, the link recipe, the native tool and `libc.prx`
- the payload SDK fork: `ps5/tools/setup-sdk.sh` installs it at the pinned revision
- `PS5_VKHomebrewUI`: the UI kit; `ps5/ui/setup-kit.sh` exports it at the pinned revision (from GitHub when it is not beside it)

## A development build

```bash
cp ps5/src/jf/config.h.example ps5/src/jf/config.h   # your own server and login; git ignores this file
ps5/tools/build.sh        # dist/PPSA99141/: eboot.bin, sce_sys, assets
ps5/tools/deploy.sh       # upload what changed to the console's FTP server
ps5/tools/run.sh          # a test run
```

A development build starts signed in to the server in `config.h`. That is for you: **never share that build**. A
release build does not read `config.h` at all (see below).

## A release build

```bash
ps5/tools/release.sh
```

That writes the version into `param.json`, makes a release build (`JF_RELEASE`: no server, no login, no device id of yours;
the device id is made on each console), runs `ps5/tools/security-check.py` on the source, the git history, the built
title and the zip, packages `dist/release-<version>/Mortalfin-<version>.zip`, and writes the Homebrew Store record.
It prepares files only; [RELEASING.md](RELEASING.md) says how to publish them.

## The version

One file, `VERSION` (`1.0.0-beta.1`, `1.0.0-rc.1`, `1.0.0`). `ps5/tools/version.py --sync` writes the console's
`contentVersion` from it (`01.000.001` for 1.0.0-beta.1); the About page and the name Mortalfin reports to Jellyfin use it too.

## Layout

- `examples/jellyfinwheel/`: the program and its screens (`kit/screen.cpp` is the wheel)
- `ps5/src/jf/`: the Jellyfin client: sign-in, library, player (FFmpeg), sound, subtitles
- `ps5/tools/`: build, deploy, packaging, `security-check.py`
- `ps5/sce_sys/`: the title's identity and icon
