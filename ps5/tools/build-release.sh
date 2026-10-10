#!/bin/sh
# A release build: the title carries no server address and no login of its own (people sign in on the console).
# Builds with the marker file .release present, then removes it, so the next ordinary build brings the config.h login back.
set -e
cd "$(dirname "$0")/../.."
touch .release
trap 'rm -f .release' EXIT
python3 ps5/tools/version.py --sync
ps5/tools/build.sh "$@"
echo "==> release build done (no built-in server or login). Package it: ps5/tools/package-release.py TAG"
