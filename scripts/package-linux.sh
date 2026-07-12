#!/usr/bin/env bash
# Assemble a Linux x86_64 distribution in dist-linux64/ (and dist-linux64.zip).
# Run after building:
#   cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j
#
# Unlike the Windows package, this one links distro libraries at runtime —
# see README.md for the packages required on the target machine.

set -euo pipefail
cd "$(dirname "$0")/.."

BUILD=build
DIST=dist-linux64

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
echo "== stripping"
before=$(stat -c%s "$DIST/deartt")
strip --strip-unneeded "$DIST/deartt"
after=$(stat -c%s "$DIST/deartt")
printf '  deartt  %8.1f MB -> %6.1f MB\n' \
       "$(echo "$before/1048576" | bc -l)" "$(echo "$after/1048576" | bc -l)"

# Build marker (see package-win64.sh).
{
  echo "built:    $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "host:     $(uname -srm)"
  echo "exe:      sha256 $(sha256sum "$DIST/deartt" | cut -d' ' -f1)"
} > "$DIST/build-info.txt"

rm -f "$DIST.zip"
zip -qr "$DIST.zip" "$DIST"
echo "== packaged: $DIST/ and $DIST.zip"
du -sh "$DIST" "$DIST.zip"
sha256sum "$DIST.zip"
