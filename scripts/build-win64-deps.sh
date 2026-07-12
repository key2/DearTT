#!/usr/bin/env bash
# Fetch / cross-build the Windows x86_64 dependencies of DearTT into
# win64-deps/. Requires: mingw-w64 (posix variants), make, cmake, curl.
#
#   win64-deps/ffmpeg/            prebuilt shared FFmpeg (BtbN, gpl)
#   win64-deps/curl-impersonate/  prebuilt DLL + import lib (lexiforest)
#   win64-deps/openssl/           static OpenSSL (cross-built)
#   win64-deps/protobuf/          static libprotobuf (cross-built; version
#                                 matches the host protoc)
#
# Downloads are cached in win64-deps/dl/.

set -euo pipefail
cd "$(dirname "$0")/.."

DEPS="$PWD/win64-deps"
DL="$DEPS/dl"
JOBS="$(nproc)"
HOST=x86_64-w64-mingw32
CC_POSIX="$HOST-gcc-posix"; command -v "$CC_POSIX" >/dev/null || CC_POSIX="$HOST-gcc"

FFMPEG_URL="https://ffmpeg.org/releases/ffmpeg-7.1.tar.xz"
CI_URL="https://github.com/lexiforest/curl-impersonate/releases/download/v2.0.0a5/libcurl-impersonate-v2.0.0a5.x86_64-win32.tar.gz"
OPENSSL_URL="https://github.com/openssl/openssl/releases/download/openssl-3.3.2/openssl-3.3.2.tar.gz"
# Protobuf must match the host protoc's version (protoc --version).
PROTOBUF_URL="https://github.com/protocolbuffers/protobuf/releases/download/v21.12/protobuf-cpp-3.21.12.tar.gz"

mkdir -p "$DL"
fetch() { [ -f "$DL/$2" ] || curl -fL "$1" -o "$DL/$2"; }

# ---------------------------------------------------------------------------
# FFmpeg (minimal cross-build). The prebuilt BtbN "gpl-shared" bundle ships
# every codec/filter FFmpeg has (~140 MB of DLLs); DearTT only plays TikTok
# LIVE streams. This LGPL build enables exactly what video_player.cpp needs
# — FLV/HLS/MPEG-TS/fMP4 demux, H.264/HEVC + AAC/MP3 decode, http(s) via
# Windows schannel (no OpenSSL) — and lands around a tenth of the size.
# ---------------------------------------------------------------------------
if [ ! -d "$DEPS/ffmpeg" ]; then
  echo "== FFmpeg (minimal cross-build)"
  fetch "$FFMPEG_URL" ffmpeg-src.tar.xz
  rm -rf "$DEPS/ffmpeg-src"
  mkdir -p "$DEPS/ffmpeg-src"
  tar xJf "$DL/ffmpeg-src.tar.xz" -C "$DEPS/ffmpeg-src" --strip-components=1
  (
    cd "$DEPS/ffmpeg-src"
    ./configure \
      --prefix="$DEPS/ffmpeg" \
      --arch=x86_64 --target-os=mingw32 \
      --cross-prefix="$HOST-" --enable-cross-compile \
      --cc="$CC_POSIX" \
      --enable-shared --disable-static \
      --disable-programs --disable-doc --disable-debug \
      --disable-avdevice --disable-avfilter --disable-postproc \
      --disable-everything \
      --enable-network --enable-schannel --enable-zlib \
      --enable-protocol=file,http,https,tcp,tls,crypto \
      --enable-demuxer=flv,live_flv,hls,mpegts,mov,aac,mp3,webp_pipe,jpeg_pipe,png_pipe,gif \
      --enable-decoder=h264,hevc,aac,mp3,mp3float,webp,mjpeg,png,gif \
      --enable-parser=h264,hevc,aac,mpegaudio,mjpeg,png,webp \
      --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc,extract_extradata
    make -j"$JOBS"
    make install
  )
  rm -rf "$DEPS/ffmpeg-src"
fi

# ---------------------------------------------------------------------------
# curl-impersonate (prebuilt DLL). We link the DLL through a MinGW-style
# import lib: the bundled static .lib is MSVC-built and not safe to link
# with MinGW, while the DLL's C ABI is.
# ---------------------------------------------------------------------------
if [ ! -d "$DEPS/curl-impersonate" ]; then
  echo "== curl-impersonate (prebuilt)"
  fetch "$CI_URL" curl-impersonate-win64.tar.gz
  rm -rf "$DEPS/curl-impersonate.tmp"
  mkdir -p "$DEPS/curl-impersonate.tmp"
  tar xzf "$DL/curl-impersonate-win64.tar.gz" -C "$DEPS/curl-impersonate.tmp"
  mkdir -p "$DEPS/curl-impersonate/include" "$DEPS/curl-impersonate/lib"
  cp -r "$DEPS/curl-impersonate.tmp/include/curl" "$DEPS/curl-impersonate/include/"
  # Import lib under a name find_library(curl-impersonate) resolves for MinGW.
  cp "$DEPS/curl-impersonate.tmp/lib/libcurl-impersonate_imp.lib" \
     "$DEPS/curl-impersonate/lib/libcurl-impersonate.dll.a"
  cp "$DEPS/curl-impersonate.tmp/lib/libcurl-impersonate.dll" \
     "$DEPS/curl-impersonate/lib/"
  rm -rf "$DEPS/curl-impersonate.tmp"
fi

# ---------------------------------------------------------------------------
# OpenSSL (static, cross-built)
# ---------------------------------------------------------------------------
if [ ! -d "$DEPS/openssl" ]; then
  echo "== OpenSSL (cross-build)"
  fetch "$OPENSSL_URL" openssl.tar.gz
  rm -rf "$DEPS/src-openssl"
  mkdir -p "$DEPS/src-openssl"
  tar xzf "$DL/openssl.tar.gz" -C "$DEPS/src-openssl" --strip-components=1
  (
    cd "$DEPS/src-openssl"
    ./Configure mingw64 --cross-compile-prefix="$HOST-" \
        --prefix="$DEPS/openssl" --libdir=lib \
        no-shared no-tests no-docs no-apps >/dev/null
    make -j"$JOBS" build_libs >/dev/null
    make install_dev >/dev/null
  )
  rm -rf "$DEPS/src-openssl"
fi

# ---------------------------------------------------------------------------
# protobuf runtime (static, cross-built; protoc itself stays the host's)
# ---------------------------------------------------------------------------
if [ ! -d "$DEPS/protobuf" ]; then
  echo "== protobuf (cross-build)"
  fetch "$PROTOBUF_URL" protobuf.tar.gz
  rm -rf "$DEPS/src-protobuf"
  mkdir -p "$DEPS/src-protobuf"
  tar xzf "$DL/protobuf.tar.gz" -C "$DEPS/src-protobuf" --strip-components=1
  cmake -B "$DEPS/src-protobuf/build" -S "$DEPS/src-protobuf" \
      -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchain-mingw64.cmake" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$DEPS/protobuf" \
      -Dprotobuf_BUILD_TESTS=OFF \
      -Dprotobuf_BUILD_PROTOC_BINARIES=OFF \
      -Dprotobuf_BUILD_SHARED_LIBS=OFF \
      -Dprotobuf_WITH_ZLIB=OFF >/dev/null
  cmake --build "$DEPS/src-protobuf/build" -j"$JOBS" >/dev/null
  cmake --install "$DEPS/src-protobuf/build" >/dev/null
  rm -rf "$DEPS/src-protobuf"
fi

echo "== done: $DEPS"
