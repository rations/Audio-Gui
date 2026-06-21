#!/usr/bin/env bash
#
# Build Audio-Gui and package a release tarball.
#
# Produces audio-gui-<version>.tar.gz that unpacks to a single Audio-Gui/
# directory containing the prebuilt binaries plus install.sh / uninstall.sh.
#
# Usage: ./release-tarball.sh [version]
#   With no argument the version is read from the VERSION file (bump that when
#   you cut a release). An explicit argument overrides it for one-off builds.
#   Falls back to `git describe`/today's date if VERSION is absent.

set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd -- "$ROOT"

if [ "$#" -ge 1 ] && [ -n "$1" ]; then
  VERSION="$1"
elif [ -f "$ROOT/VERSION" ]; then
  VERSION="$(tr -d '[:space:]' <"$ROOT/VERSION")"
else
  VERSION="$(git describe --tags --always --dirty 2>/dev/null || date +%Y%m%d)"
fi
[ -n "$VERSION" ] || { echo "ERROR: could not determine a version (empty VERSION file?)" >&2; exit 1; }
BUILD_DIR="$ROOT/build-release"
OUT="$ROOT/audio-gui-$VERSION.tar.gz"

info() { printf '  %s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; }
die()
{
  printf 'ERROR: %s\n' "$*" >&2
  exit 1
}

# ---- build ------------------------------------------------------------------

step "Building Audio-Gui ($VERSION)"
cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || echo 2)"

# ---- stage ------------------------------------------------------------------

STAGE="$(mktemp -d)"
trap 'rm -rf -- "$STAGE"' EXIT
PKG="$STAGE/Audio-Gui"
mkdir -p -- "$PKG/bin"

step "Staging release tree"

# Required binaries.
for b in audio-gui pa-alsa-bridge; do
  [ -f "$BUILD_DIR/$b" ] || die "missing build output: $b (did the build succeed?)"
  install -m 0755 -- "$BUILD_DIR/$b" "$PKG/bin/$b"
  info "staged bin/$b"
done

# Optional JACK bridge (only built when libjack was present).
if [ -f "$BUILD_DIR/pulse-jack-bridge" ]; then
  install -m 0755 -- "$BUILD_DIR/pulse-jack-bridge" "$PKG/bin/pulse-jack-bridge"
  info "staged bin/pulse-jack-bridge"
else
  info "pulse-jack-bridge not built — JACK routing will not be packaged"
fi

# Strip release binaries to shrink the tarball (best-effort).
if command -v strip >/dev/null 2>&1; then
  strip --strip-unneeded "$PKG"/bin/* 2>/dev/null || true
fi

# Scripts, desktop template, and docs.
install -m 0755 -- "$ROOT/packaging/install.sh" "$PKG/install.sh"
install -m 0755 -- "$ROOT/packaging/uninstall.sh" "$PKG/uninstall.sh"
install -m 0644 -- "$ROOT/packaging/audio-gui.desktop.in" "$PKG/audio-gui.desktop.in"
install -m 0644 -- "$ROOT/README.md" "$PKG/README.md"
install -m 0644 -- "$ROOT/LICENSE.txt" "$PKG/LICENSE.txt"
info "staged install.sh, uninstall.sh, desktop template, README, LICENSE"

# ---- archive ----------------------------------------------------------------

step "Creating tarball"
# Clean, reproducible ownership; unpacks to Audio-Gui/.
tar -czf "$OUT" \
  --owner=0 --group=0 --numeric-owner \
  -C "$STAGE" Audio-Gui

step "Done"
info "$OUT"
info "$(du -h "$OUT" | cut -f1) — unpacks to Audio-Gui/"
