#!/usr/bin/env bash
# Records what the build downloads or clones, so the next build can prove it got the same thing:
#   dav1d-<version>   the sha256 of the source tarball
#   PS5_Vulkan, PS5_Mesa, PS5_PayloadSDK   the commit each sibling checkout is at
# Run it when you have checked those are what you meant to build against, and commit ps5/tools/pins.txt.
#   ps5/tools/pin-deps.sh
set -euo pipefail
ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
root=$(dirname "$ps5")
parent=$(dirname "$root")
pins=$ps5/tools/pins.txt
ver=${DAV1D_VERSION:-1.5.1}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

curl -fL "https://code.videolan.org/videolan/dav1d/-/archive/$ver/dav1d-$ver.tar.gz" -o "$tmp/d.tar.gz"
{
    echo "# name value: dav1d tarball sha256, and the commit of each sibling checkout. Written by ps5/tools/pin-deps.sh"
    echo "dav1d-$ver $(sha256sum "$tmp/d.tar.gz" | cut -d' ' -f1)"
    for pair in "PS5_Vulkan ${PS5_VULKAN_DIR:-$parent/PS5_Vulkan}" "PS5_Mesa ${PS5_MESA_FORK:-$parent/PS5_Mesa}" \
                "PS5_PayloadSDK ${PS5_PAYLOAD_SDK_FORK:-$parent/PS5_PayloadSDK}"; do
        read -r name path <<< "$pair"
        if [[ -d $path/.git ]]; then
            echo "$name $(git -C "$path" rev-parse HEAD)"
        else
            echo "# $name: no checkout at $path, not pinned" >&2
        fi
    done
} > "$pins.new"
mv "$pins.new" "$pins"
cat "$pins"
