#!/usr/bin/env bash
# Assemble a self-contained Windows x86_64 distribution in dist-win64/
# (and dist-win64.zip). Run after building:
#   cmake -B build-win64 -S . -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake ...
#   cmake --build build-win64 -j

set -euo pipefail
cd "$(dirname "$0")/.."

BUILD=build-win64
DEPS=win64-deps
DIST=dist-win64
MINGW_LIB=/usr/x86_64-w64-mingw32/lib

[ -f "$BUILD/deartt.exe" ] || { echo "error: $BUILD/deartt.exe not built"; exit 1; }

rm -rf "$DIST"
mkdir -p "$DIST"

cp "$BUILD/deartt.exe" "$DIST/"

# FFmpeg (only the libraries we link; their deps stay within this set).
for d in avformat avcodec avutil swresample swscale; do
  cp "$DEPS"/ffmpeg/bin/${d}-*.dll "$DIST/"
done

# TikTok transport (Chrome-fingerprint libcurl).
cp "$DEPS/curl-impersonate/lib/libcurl-impersonate.dll" "$DIST/"

# MinGW runtime (libgcc/libstdc++ are linked statically; see the toolchain).
cp "$MINGW_LIB/libwinpthread-1.dll" "$DIST/"
cp "$MINGW_LIB/zlib1.dll" "$DIST/"

# TikTok SDK JS, loaded at runtime by the QuickJS signer (resolved relative
# to the exe by LiveSession::findJsDir).
mkdir -p "$DIST/js"
cp ttlive-cpp/js/*.js "$DIST/js/"

# Event-viewer website, served by the embedded web server on :8080
# (resolved relative to the exe by findWebDir).
mkdir -p "$DIST/web"
cp web/* "$DIST/web/"

# Bundled color-emoji fallback (Twemoji COLR, CC-BY 4.0) — used when Segoe
# UI Emoji is unavailable (e.g. Wine); resolved via findResource.
mkdir -p "$DIST/fonts"
cp fonts/Twemoji.Mozilla.ttf "$DIST/fonts/"

# ONNX Runtime + face recognition models (if the cross-build included them).
if [ -f "$DEPS/onnxruntime/lib/onnxruntime.dll" ]; then
  cp "$DEPS/onnxruntime/lib/onnxruntime.dll" "$DIST/"
  # onnxruntime.dll is MSVC-built; ship the VC++ runtime it depends on.
  VCRT="$DEPS/dl/vcrt"
  for dll in vcruntime140.dll vcruntime140_1.dll msvcp140.dll msvcp140_1.dll; do
    [ -f "$VCRT/$dll" ] && cp "$VCRT/$dll" "$DIST/"
  done
  mkdir -p "$DIST/models"
  for m in models/*.onnx; do
    [ -f "$m" ] && cp "$m" "$DIST/models/"
  done
  # Profiles are created by the user at runtime (each pairs a stream with the
  # people expected on it + their reference photos); ship an empty folder.
  mkdir -p "$DIST/profiles"
fi

# Bundled pan-Unicode base font (GoNotoKurrent, SIL OFL 1.1;
# github.com/satbyy/go-noto-universal): 80+ merged Noto scripts so chat in
# Arabic/Cyrillic/Thai/Indic/CJK/... renders identically on every machine.
cp fonts/GoNotoKurrent-Regular.ttf "$DIST/fonts/"

# CA bundle for TLS verification: the curl-impersonate DLL (BoringSSL) has no
# OS trust-store integration on Windows; ttlive picks this file up from the
# exe directory (web_defaults::ca_bundle_path).
if [ ! -f "$DEPS/dl/cacert.pem" ]; then
  curl -fL https://curl.se/ca/cacert.pem -o "$DEPS/dl/cacert.pem"
fi
cp "$DEPS/dl/cacert.pem" "$DIST/curl-ca-bundle.crt"

# Strip the exe and every shipped DLL: MinGW/FFmpeg binaries carry large
# symbol/debug sections that are useless at runtime (avcodec alone shrinks
# ~4x). Stripped copies live only in the dist; the originals keep their
# symbols for debugging.
echo "== stripping"
for f in "$DIST"/*.exe "$DIST"/*.dll; do
  before=$(stat -c%s "$f")
  x86_64-w64-mingw32-strip --strip-unneeded "$f" || true
  after=$(stat -c%s "$f")
  printf '  %-28s %8.1f MB -> %6.1f MB\n' "$(basename "$f")" \
         "$(echo "$before/1048576" | bc -l)" "$(echo "$after/1048576" | bc -l)"
done

# Sanity: every DLL referenced by the exe is shipped or a Windows system lib.
echo "== import check"
missing=0
for dll in $(x86_64-w64-mingw32-objdump -p "$DIST/deartt.exe" |
             awk '/DLL Name/{print $3}'); do
  case "$dll" in
    KERNEL32.dll|USER32.dll|GDI32.dll|SHELL32.dll|OPENGL32.dll|msvcrt.dll|\
    ADVAPI32.dll|WS2_32.dll|ole32.dll|IMM32.dll|SETUPAPI.dll|WINMM.dll|\
    api-ms-*) continue ;;
  esac
  if [ ! -f "$DIST/$dll" ]; then
    echo "  MISSING: $dll"
    missing=1
  fi
done
[ "$missing" = 0 ] && echo "  ok — all non-system imports shipped"

# Build marker: lets anyone verify which build an extracted folder came from
# (stale copies of older zips are a recurring source of "DLL not found").
{
  echo "built:    $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "host:     $(uname -srm)"
  echo "exe:      sha256 $(sha256sum "$DIST/deartt.exe" | cut -d' ' -f1)"
  echo "contents:"
  (cd "$DIST" && sha256sum ./*.exe ./*.dll | sed 's/^/  /')
} > "$DIST/build-info.txt"

rm -f "$DIST.zip"
zip -qr "$DIST.zip" "$DIST"
echo "== packaged: $DIST/ and $DIST.zip"
du -sh "$DIST" "$DIST.zip"
sha256sum "$DIST.zip"
