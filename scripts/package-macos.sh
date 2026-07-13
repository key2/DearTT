#!/usr/bin/env bash
# Assemble a macOS distribution in dist-macos/ (and dist-macos.zip).
# Run after building:
#   cmake -B build-macos -S . -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-macos -j
#
# Like the Linux package (and unlike the self-contained Windows one), this
# links Homebrew/system libraries at runtime: the target machine needs the
# same Homebrew formulae installed (ffmpeg, protobuf, freetype, openssl@3,
# abseil, brotli, zstd, libidn2 — pulled in transitively by `brew install
# ffmpeg protobuf freetype`). The arch (arm64 vs x86_64) follows the build.

set -euo pipefail
cd "$(dirname "$0")/.."

BUILD=build-macos
DIST=dist-macos

[ -f "$BUILD/deartt" ] || { echo "error: $BUILD/deartt not built"; exit 1; }

rm -rf "$DIST"
mkdir -p "$DIST"

cp "$BUILD/deartt" "$DIST/"

# TikTok SDK JS, loaded at runtime by the QuickJS signer (resolved relative
# to the binary by LiveSession::findJsDir).
mkdir -p "$DIST/js"
cp ttlive-cpp/js/*.js "$DIST/js/"

# Event-viewer website, served by the embedded web server on :8080.
mkdir -p "$DIST/web"
cp web/* "$DIST/web/"

# Bundled fonts (resolved via findResource):
#   GoNotoKurrent (SIL OFL)  — pan-Unicode base font
#   Twemoji (CC-BY 4.0)      — color emoji (COLR)
mkdir -p "$DIST/fonts"
cp fonts/GoNotoKurrent-Regular.ttf fonts/Twemoji.Mozilla.ttf "$DIST/fonts/"

# Strip the dist copy (the build tree keeps its symbols for debugging).
# -x removes local symbols only; keeps the dynamic symbols the dylibs need.
echo "== stripping"
before=$(stat -f%z "$DIST/deartt")
strip -x "$DIST/deartt"
after=$(stat -f%z "$DIST/deartt")
printf '  deartt  %8.1f MB -> %6.1f MB\n' \
       "$(echo "$before/1048576" | bc -l)" "$(echo "$after/1048576" | bc -l)"

# Build marker (see package-win64.sh).
{
  echo "built:    $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "host:     $(uname -srm)"
  echo "arch:     $(lipo -archs "$DIST/deartt" 2>/dev/null || uname -m)"
  echo "exe:      sha256 $(shasum -a 256 "$DIST/deartt" | cut -d' ' -f1)"
} > "$DIST/build-info.txt"

rm -f "$DIST.zip"
# -y: store symlinks as-is rather than following them.
zip -qry "$DIST.zip" "$DIST"
echo "== packaged: $DIST/ and $DIST.zip"
du -sh "$DIST" "$DIST.zip"
shasum -a 256 "$DIST.zip"
