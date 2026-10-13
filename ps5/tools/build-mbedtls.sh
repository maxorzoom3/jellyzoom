#!/usr/bin/env bash
# Builds mbedTLS (TLS client for https; Apache-2.0 OR GPL-2.0-or-later, used here under Apache-2.0) for the PS5 as static
# libraries, into their own folder. It does not touch FFmpeg, dav1d or the title build.
#
#   ps5/tools/build-mbedtls.sh          (needs make, python3, curl, tar, bzip2 and the PS5 payload SDK)
#
# Results: <project>/vendor/mbedtls-ps5/{include,lib}, and ~/mbedtls-imports.txt (every symbol the libraries expect the
# console's libc to provide).
#
# The configuration is the library's default with what a TLS 1.2 client does not need taken out: no sockets or file
# access of its own (the tunnel in jf_tls.cpp has both), no TLS 1.3, no server, no DTLS. Random bytes and the calendar
# date come from jf_tls.cpp (mbedtls_hardware_poll and mbedtls_platform_gmtime_r).
set -eu
proj=$(cd "$(dirname "$0")/../.." && pwd)
PATH=/mnt/data/ps5stack/venv/bin:$PATH
export PATH
SDK=
for c in "${PS5_PAYLOAD_SDK:-}" /opt/ps5-payload-sdk /mnt/data/ps5stack/PS5_Vulkan/.deps/native/ps5-payload-sdk; do
    if [ -n "$c" ] && [ -f "$c/toolchain/prospero.sh" ]; then SDK=$c; break; fi
done
VER=${MBEDTLS_VERSION:-3.6.7}
# the release tarball's hash: the download is refused if it differs
SHA256=${MBEDTLS_SHA256:-a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6}
prefix=${MBEDTLS_PREFIX:-$proj/vendor/mbedtls-ps5}
work=${MBEDTLS_WORK:-$proj/.build-mbedtls}

for t in make python3 curl tar bzip2 sha256sum; do
    command -v "$t" >/dev/null 2>&1 || { echo "missing tool: $t (install it, then run this again)"; exit 1; }
done
[ -n "$SDK" ] || { echo "no payload SDK with toolchain/prospero.sh found: set PS5_PAYLOAD_SDK to it, then run this again"; exit 1; }
echo "payload SDK: $SDK"

# the SDK's own toolchain settings (the ports use the same file)
. "$SDK/toolchain/prospero.sh"
CC_BIN=${CC:-$SDK/bin/prospero-clang}
AR_BIN=${AR:-$SDK/bin/prospero-ar}
NM_BIN=${NM:-$SDK/bin/prospero-nm}
echo "compiler: $CC_BIN"

mkdir -p "$work" "$prefix"
cd "$work"
tarball="mbedtls-$VER.tar.bz2"
if [ ! -f "$tarball" ]; then
    curl -fL "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$VER/$tarball" -o "$tarball.part"
    mv "$tarball.part" "$tarball"
fi
echo "$SHA256  $tarball" | sha256sum -c - || { echo "$tarball does not match the expected hash: removed, run this again"; rm -f "$tarball"; exit 1; }

# a fresh copy every time, so the configuration below always starts from the default
rm -rf "src"
mkdir src
tar xjf "$tarball" -C src --strip-components=1
cd src
cfg=include/mbedtls/mbedtls_config.h
unset_opt() { python3 scripts/config.py -f "$cfg" unset "$1" >/dev/null; }
set_opt() { python3 scripts/config.py -f "$cfg" set "$1" >/dev/null; }
for o in MBEDTLS_NET_C MBEDTLS_FS_IO MBEDTLS_TIMING_C MBEDTLS_PSA_ITS_FILE_C MBEDTLS_PSA_CRYPTO_STORAGE_C \
         MBEDTLS_SSL_PROTO_TLS1_3 MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE \
         MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_ENABLED MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED \
         MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL_ENABLED \
         MBEDTLS_SSL_SRV_C MBEDTLS_SSL_PROTO_DTLS MBEDTLS_SSL_DTLS_HELLO_VERIFY MBEDTLS_SSL_DTLS_ANTI_REPLAY \
         MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE MBEDTLS_SSL_DTLS_BADMAC_LIMIT MBEDTLS_SSL_DTLS_CONNECTION_ID \
         MBEDTLS_SSL_DTLS_CONNECTION_ID_COMPAT MBEDTLS_SSL_DTLS_SRTP MBEDTLS_SSL_COOKIE_C MBEDTLS_SSL_CACHE_C \
         MBEDTLS_SSL_TICKET_C MBEDTLS_LMS_C MBEDTLS_LMS_PRIVATE MBEDTLS_SELF_TEST MBEDTLS_DEBUG_C; do
    unset_opt "$o"
done
set_opt MBEDTLS_NO_PLATFORM_ENTROPY      # the random bytes come from jf_tls.cpp ...
set_opt MBEDTLS_ENTROPY_HARDWARE_ALT     # ... as mbedtls_hardware_poll
set_opt MBEDTLS_PLATFORM_GMTIME_R_ALT    # the calendar date comes from jf_tls.cpp as mbedtls_platform_gmtime_r
set_opt MBEDTLS_NO_UDBL_DIVISION           # no 128-bit division (__udivti3): a client only verifies, it never needs it fast

make -C library -j"$(nproc 2>/dev/null || echo 4)" static CC="$CC_BIN" AR="$AR_BIN" \
    CFLAGS="-O2 -fPIC -pthread -ffunction-sections -fdata-sections -ffile-prefix-map=$work=. -ffile-prefix-map=$proj=."

for l in libmbedtls.a libmbedx509.a libmbedcrypto.a; do
    [ -f "library/$l" ] || { echo "no $l was built"; exit 1; }
done
rm -rf "$prefix/include" "$prefix/lib"
mkdir -p "$prefix/include" "$prefix/lib"
cp -r include/mbedtls include/psa "$prefix/include/"
cp library/libmbedtls.a library/libmbedx509.a library/libmbedcrypto.a "$prefix/lib/"
printf 'mbedTLS %s (Apache-2.0 OR GPL-2.0-or-later), release tarball sha256 %s\n' "$VER" "$SHA256" > "$prefix/PROVENANCE.txt"
cp LICENSE "$prefix/LICENSE" 2>/dev/null || true

for l in libmbedtls.a libmbedx509.a libmbedcrypto.a; do
    "$NM_BIN" -u "$prefix/lib/$l" 2>/dev/null
done | awk 'NF==2 && $1=="U" {print $2}' | sort -u > "$HOME/mbedtls-imports.txt" || true
echo "built mbedTLS $VER into $prefix/lib ($(cat "$prefix"/lib/*.a | wc -c) bytes)"
echo "names it expects from libc or the platform: $(wc -l < "$HOME/mbedtls-imports.txt"), listed in ~/mbedtls-imports.txt"
