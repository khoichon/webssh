#!/usr/bin/env bash
# Cross-compiles mbedTLS + libssh2 with Emscripten, then builds ssh_shim.c
# against them, producing web/ssh.js + web/ssh.wasm.
#
# Requires: Emscripten SDK active in this shell (emcc, emcmake, emmake on PATH).

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
THIRD_PARTY="$ROOT/third_party"
BUILD="$ROOT/build"

mkdir -p "$THIRD_PARTY" "$BUILD"

command -v emcc >/dev/null || {
  echo "emcc not found. Activate the Emscripten SDK first (source ./emsdk_env.sh)." >&2
  exit 1
}

# ---- mbedTLS ---------------------------------------------------------------
if [ ! -d "$THIRD_PARTY/mbedtls" ]; then
  git clone --depth 1 --branch v3.6.0 https://github.com/Mbed-TLS/mbedtls "$THIRD_PARTY/mbedtls"
  (cd "$THIRD_PARTY/mbedtls" && git submodule update --init --depth 1)
fi

MBEDTLS_BUILD="$BUILD/mbedtls"
mkdir -p "$MBEDTLS_BUILD"
(
  cd "$MBEDTLS_BUILD"
  emcmake cmake "$THIRD_PARTY/mbedtls" \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_TESTING=OFF \
    -DENABLE_PROGRAMS=OFF
  emmake make -j"$(nproc)" mbedtls mbedx509 mbedcrypto
)

# ---- libssh2 ----------------------------------------------------------------
if [ ! -d "$THIRD_PARTY/libssh2" ]; then
  git clone --depth 1 --branch libssh2-1.11.1 https://github.com/libssh2/libssh2 "$THIRD_PARTY/libssh2"
fi

LIBSSH2_BUILD="$BUILD/libssh2"
mkdir -p "$LIBSSH2_BUILD"
(
  cd "$LIBSSH2_BUILD"
  emcmake cmake "$THIRD_PARTY/libssh2" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCRYPTO_BACKEND=mbedTLS \
    -DMBEDTLS_INCLUDE_DIR="$THIRD_PARTY/mbedtls/include" \
    -DMBEDTLS_LIBRARY="$MBEDTLS_BUILD/library/libmbedtls.a" \
    -DMBEDX509_LIBRARY="$MBEDTLS_BUILD/library/libmbedx509.a" \
    -DMBEDCRYPTO_LIBRARY="$MBEDTLS_BUILD/library/libmbedcrypto.a" \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_TESTING=OFF
  emmake make -j"$(nproc)" libssh2_static
)

# ---- shim ---------------------------------------------------------------
emcc "$ROOT/src/ssh_shim.c" \
  -I"$THIRD_PARTY/libssh2/include" \
  -I"$THIRD_PARTY/mbedtls/include" \
  "$LIBSSH2_BUILD/src/libssh2.a" \
  "$MBEDTLS_BUILD/library/libmbedtls.a" \
  "$MBEDTLS_BUILD/library/libmbedx509.a" \
  "$MBEDTLS_BUILD/library/libmbedcrypto.a" \
  -O2 \
  -s WASM=1 \
  -s MODULARIZE=1 \
  -s EXPORT_NAME=createSshModule \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap"]' \
  -s ENVIRONMENT=web \
  -o "$ROOT/web/ssh.js"

echo "Built web/ssh.js + web/ssh.wasm"
