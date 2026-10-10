#!/usr/bin/env bash
# A tester release in one go:
#
#   ps5/tools/release.sh
#
# 1. the version (the file VERSION) is written into param.json; the tree must then be committed
# 2. a release build (no server, login or device id of yours in it)
# 3. the security check of the source, git history and the built title
# 4. the zip (dist/release-<version>/Mortalfin-<version>.zip) and the source of every part
# 5. the security check of the zip itself, and the Homebrew Store record for it
#
# Nothing is published by this script: it only prepares the files. See docs/RELEASING.md for GitHub.
set -euo pipefail
cd "$(dirname "$0")/../.."

python3 ps5/tools/version.py --sync
version=$(python3 ps5/tools/version.py)

if [[ -n $(git status --porcelain) ]]; then
    echo "release.sh: commit your changes first (the release records the exact source it was built from):" >&2
    git status --short >&2
    exit 1
fi

ps5/tools/build-release.sh
python3 ps5/tools/security-check.py --build
python3 ps5/tools/package-release.py "$version"
zip="dist/release-$version/Mortalfin-$version.zip"
python3 ps5/tools/security-check.py --zip "$zip"
python3 ps5/tools/catalog-record.py "$version" "$@"
echo
echo "Ready: $zip"
echo "Next: docs/RELEASING.md (tag $version, GitHub release, then the Homebrew Store pull request)"
