#!/bin/sh
# Builds dav1d (AV1 decoder, BSD-2-Clause) for the PS5 as a static library, into its own folder. It does not touch
# FFmpeg or the project build; it only answers "does dav1d cross-build with this SDK, and what does it import?".
#
#   ps5/tools/build-dav1d.sh            (needs meson, ninja, nasm, curl, tar and the PS5 payload SDK)
#
# Results: <project>/vendor/dav1d-ps5/{include,lib}, and ~/dav1d-imports.txt (every symbol the library expects the
# console's libc to provide). Send me that file and the last lines of the output.
set -eu
proj=$(cd "$(dirname "$0")/../.." && pwd)
# meson lives in the project's venv (the same one the title build uses)
PATH=/mnt/data/ps5stack/venv/bin:$PATH
export PATH
# the SDK with the compiler: the one in the environment, the homebrew one, or the copy the Vulkan build keeps
SDK=
for c in "${PS5_PAYLOAD_SDK:-}" /opt/ps5-payload-sdk /mnt/data/ps5stack/PS5_Vulkan/.deps/native/ps5-payload-sdk; do
    if [ -n "$c" ] && [ -f "$c/toolchain/prospero.sh" ]; then SDK=$c; break; fi
done
VER=${DAV1D_VERSION:-1.5.1}
prefix=${DAV1D_PREFIX:-$proj/vendor/dav1d-ps5}
work=${DAV1D_WORK:-$proj/.build-dav1d}

for t in meson ninja nasm curl tar python3; do
    command -v "$t" >/dev/null 2>&1 || { echo "missing tool: $t (install it, then run this again)"; exit 1; }
done
[ -n "$SDK" ] || { echo "no payload SDK with toolchain/prospero.sh found: set PS5_PAYLOAD_SDK to it, then run this again"; exit 1; }
echo "payload SDK: $SDK"

# the SDK's own toolchain settings (the ports use the same file)
. "$SDK/toolchain/prospero.sh"
CC_BIN=${CC:-$SDK/bin/prospero-clang}
AR_BIN=${AR:-$SDK/bin/prospero-ar}
STRIP_BIN=${STRIP:-$SDK/bin/prospero-strip}
NM_BIN=${NM:-$SDK/bin/prospero-nm}
echo "compiler: $CC_BIN"

mkdir -p "$work" "$prefix"
cd "$work"
if [ ! -d "dav1d-$VER" ]; then
    curl -fL "https://code.videolan.org/videolan/dav1d/-/archive/$VER/dav1d-$VER.tar.gz" -o "dav1d-$VER.tar.gz"
    tar xf "dav1d-$VER.tar.gz"
fi

python3 - "$CC_BIN" "$AR_BIN" "$STRIP_BIN" > cross.txt <<'PY'
import shlex, sys
cc, ar, strip = (shlex.split(a) for a in sys.argv[1:4])
print("[binaries]")
print("c = %r" % cc)
print("ar = %r" % ar)
print("strip = %r" % strip)
print("[host_machine]")
print("system = 'freebsd'\ncpu_family = 'x86_64'\ncpu = 'znver2'\nendian = 'little'")
print("[built-in options]")
print("c_args = ['-O2', '-fPIC', '-pthread']")
print("c_link_args = ['-pthread']")
PY

rm -rf build
meson setup build "dav1d-$VER" --cross-file cross.txt --prefix "$prefix" --libdir lib \
    -Ddefault_library=static -Denable_tools=false -Denable_tests=false -Denable_examples=false -Dbitdepths=8,16
ninja -C build
ninja -C build install

lib="$prefix/lib/libdav1d.a"
[ -f "$lib" ] || { echo "no libdav1d.a was built"; exit 1; }
"$NM_BIN" -u "$lib" 2>/dev/null | awk 'NF==2 && $1=="U" {print $2}' | sort -u > "$HOME/dav1d-imports.txt" || true
echo "built $lib ($(wc -c < "$lib") bytes)"
echo "imports it expects from libc: $(wc -l < "$HOME/dav1d-imports.txt") symbols, listed in ~/dav1d-imports.txt"
