#!/usr/bin/env bash
#
# Build a Debian package (.deb) for Audio-Gui.
#
# Produces audio-gui_<version>_<arch>.deb that installs the binaries to
# /usr/bin, a desktop menu entry to /usr/share/applications, and a login
# autostart entry to /etc/xdg/autostart (so every user — including the generic
# live user on a snapshot ISO — gets `audio-gui --restore` at login).
#
# This mirrors the SYSTEM_INSTALL path of install.sh, with one deliberate
# difference: it is sysvinit-friendly. Audio-Gui's autostart is a plain XDG
# .desktop file, never a systemd unit, so the package declares no dependency on
# systemd and works as-is on Devuan, MX Linux and any other sysvinit/runit
# Debian derivative as well as on Debian/Ubuntu proper.
#
# Per-user ~/.asoundrc is intentionally NOT touched at package install time
# (install runs as root, and the config is per-card so it must be generated on
# the target hardware): each user's ~/.asoundrc is written at their first login
# by the autostart entry, exactly as a system-wide install.sh would do it.
#
# Usage: ./build-deb.sh [version]
#   With no argument the version is read from the VERSION file. Build deps
#   (cmake, g++, cairo/freetype/X11/ALSA/JACK dev, dpkg-deb) must already be installed.

set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd -- "$ROOT"

if [ "$#" -ge 1 ] && [ -n "$1" ]; then
  VERSION="$1"
elif [ -f "$ROOT/VERSION" ]; then
  VERSION="$(tr -d '[:space:]' <"$ROOT/VERSION")"
else
  VERSION="$(git describe --tags --always --dirty 2>/dev/null || date +%Y%m%d)"
fi
[ -n "$VERSION" ] || { echo "ERROR: could not determine a version (empty VERSION file?)" >&2; exit 1; }

info() { printf '  %s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; }
die()
{
  printf 'ERROR: %s\n' "$*" >&2
  exit 1
}

command -v dpkg-deb >/dev/null 2>&1 || die "dpkg-deb not found (install the 'dpkg' package)."
command -v cmake >/dev/null 2>&1 || die "cmake not found (install build dependencies first)."

ARCH="$(dpkg --print-architecture 2>/dev/null || echo amd64)"
BUILD_DIR="$ROOT/build-deb"
OUT="$ROOT/audio-gui_${VERSION}_${ARCH}.deb"

# ---- build ------------------------------------------------------------------

step "Building Audio-Gui ($VERSION, $ARCH)"
# Build for /usr, which is where a Debian package installs. respath.cpp finds the
# bundled fonts relative to the installed binary (/usr/bin -> /usr/share/audio-gui)
# and no longer depends on this being right, but a package whose compiled-in prefix
# names a path it does not ship is a trap for the next person to read it: keep the
# two saying the same thing. The fonts are not optional -- without them the GUI
# falls back to a system face whose metrics tools/uirender never audited.
#
# Hence also the deb's OWN build dir rather than the tarball's build-release, which
# is configured for /usr/local, where install.sh puts a system install.
cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || echo 2)"

[ -f "$BUILD_DIR/audio-gui" ] || die "missing build output: audio-gui"
[ -f "$BUILD_DIR/pa-alsa-bridge" ] || die "missing build output: pa-alsa-bridge"

# ---- stage the package tree -------------------------------------------------

STAGE="$(mktemp -d)"
trap 'rm -rf -- "$STAGE"' EXIT
chmod 0755 -- "$STAGE"   # archive root (./) must be world-readable, not 0700

step "Staging package tree"
mkdir -p -- "$STAGE/DEBIAN" \
            "$STAGE/usr/bin" \
            "$STAGE/usr/share/applications" \
            "$STAGE/usr/share/audio-gui/fonts" \
            "$STAGE/usr/share/icons/hicolor" \
            "$STAGE/usr/share/doc/audio-gui" \
            "$STAGE/etc/xdg/autostart"

# Binaries (strip to shrink, best-effort).
install -m 0755 -- "$BUILD_DIR/audio-gui" "$STAGE/usr/bin/audio-gui"
install -m 0755 -- "$BUILD_DIR/pa-alsa-bridge" "$STAGE/usr/bin/pa-alsa-bridge"
info "staged usr/bin/audio-gui, usr/bin/pa-alsa-bridge"
JACK_DEP=""
if [ -f "$BUILD_DIR/pulse-jack-bridge" ]; then
  install -m 0755 -- "$BUILD_DIR/pulse-jack-bridge" "$STAGE/usr/bin/pulse-jack-bridge"
  JACK_DEP="libjack-jackd2-0 | libjack0"
  info "staged usr/bin/pulse-jack-bridge"
else
  info "pulse-jack-bridge not built — JACK routing not packaged"
fi
if command -v strip >/dev/null 2>&1; then
  strip --strip-unneeded "$STAGE"/usr/bin/* 2>/dev/null || true
fi

# THE BUNDLED FONTS ARE NOT OPTIONAL. There is no toolkit to fall back on any
# more: the GUI draws its own text through FreeType, and without these it falls
# back to a cairo toy "sans-serif" whose metrics are whatever the machine has --
# which is exactly the case tools/uirender refuses to audit, because a label that
# fits Roboto may not fit a substitute. The licences travel with them: Roboto is
# Apache-2.0 and Michroma is OFL-1.1, and both require the notice to be shipped.
install -m 0644 -- "$ROOT/resources/fonts/Roboto-Regular.ttf" \
                   "$STAGE/usr/share/audio-gui/fonts/Roboto-Regular.ttf"
install -m 0644 -- "$ROOT/resources/fonts/Michroma-Regular.ttf" \
                   "$STAGE/usr/share/audio-gui/fonts/Michroma-Regular.ttf"
install -m 0644 -- "$ROOT/resources/fonts/Roboto-LICENSE.txt" \
                   "$STAGE/usr/share/audio-gui/fonts/Roboto-LICENSE.txt"
install -m 0644 -- "$ROOT/resources/fonts/Michroma-OFL.txt" \
                   "$STAGE/usr/share/audio-gui/fonts/Michroma-OFL.txt"
info "staged usr/share/audio-gui/fonts (Roboto, Michroma + licences)"

# The icon theme, found by NAME from the desktop entry's Icon= key. This is the
# mechanism a desktop environment uses; a plain window manager instead reads the
# _NET_WM_ICON the window publishes from src/platform/appicon.h, which is the
# same artwork compiled in. Both are generated by scripts/make-icons.sh.
if [ -d "$ROOT/packaging/icons/hicolor" ]; then
  cp -a -- "$ROOT/packaging/icons/hicolor/." "$STAGE/usr/share/icons/hicolor/"
  find "$STAGE/usr/share/icons/hicolor" -type d -exec chmod 0755 {} +
  find "$STAGE/usr/share/icons/hicolor" -type f -exec chmod 0644 {} +
  info "staged usr/share/icons/hicolor (audio-gui.png ladder)"
else
  die "packaging/icons/hicolor is missing -- run scripts/make-icons.sh"
fi

# Desktop menu entry (Exec points at the installed absolute path).
sed 's|@EXEC@|/usr/bin/audio-gui|' "$ROOT/packaging/audio-gui.desktop.in" \
  >"$STAGE/usr/share/applications/audio-gui.desktop"
chmod 0644 -- "$STAGE/usr/share/applications/audio-gui.desktop"
info "staged usr/share/applications/audio-gui.desktop"

# System-wide login autostart entry. Byte-for-byte identical body to
# ensureAutostartEntry() in src/app.cpp (with the absolute /usr/bin path) so
# the GUI sees it as up to date and does not churn it. Plain XDG .desktop —
# deliberately not a systemd unit, so this is sysvinit/runit friendly.
cat >"$STAGE/etc/xdg/autostart/audio-gui-restore.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Audio routing (restore)
Comment=Restore the saved PulseAudio-compat audio routing
Exec="/usr/bin/audio-gui" --restore
Terminal=false
NoDisplay=true
X-GNOME-Autostart-enabled=true
EOF
chmod 0644 -- "$STAGE/etc/xdg/autostart/audio-gui-restore.desktop"
info "staged etc/xdg/autostart/audio-gui-restore.desktop"

# Docs: README + copyright.
install -m 0644 -- "$ROOT/README.md" "$STAGE/usr/share/doc/audio-gui/README.md"
install -m 0644 -- "$ROOT/LICENSE.txt" "$STAGE/usr/share/doc/audio-gui/copyright"

# Installed-size in KiB (Debian policy: total size of installed files).
INSTALLED_SIZE="$(du -k -s "$STAGE/usr" "$STAGE/etc" | awk '{s+=$1} END{print s}')"

# ---- control metadata -------------------------------------------------------

step "Writing control metadata"

# Recommends (not Depends) for JACK: routing falls back to ALSA without it, and
# the GUI loads libjack via dlopen so audio-gui itself has no hard link to it.
RECOMMENDS="alsa-utils"
[ -n "$JACK_DEP" ] && RECOMMENDS="$RECOMMENDS, $JACK_DEP"

cat >"$STAGE/DEBIAN/control" <<EOF
Package: audio-gui
Version: $VERSION
Architecture: $ARCH
Maintainer: rations <ehqcar@proton.me>
Installed-Size: $INSTALLED_SIZE
Depends: libc6, libasound2, libcairo2, libfreetype6, libx11-6
Recommends: $RECOMMENDS
Section: sound
Priority: optional
Homepage: https://github.com/rations/Audio-Gui
Description: GUI for ALSA audio and PulseAudio-compat routing
 Audio-Gui is a control panel for ALSA mixing and for switching the
 PulseAudio-compatible audio routing between its ALSA and JACK bridges.
 .
 The interface is drawn directly with Cairo and FreeType on a plain X11
 window: there is no widget toolkit, no Qt, no GTK and no GLib.
 .
 It installs a plain XDG autostart entry (no systemd unit), so it works on
 systemd, sysvinit and runit systems alike — Debian, Devuan, MX Linux and
 derivatives. Each user's per-card ~/.asoundrc is generated at their first
 login by the bundled "audio-gui --restore" autostart entry.
EOF

# /etc files are configuration: list as conffiles so dpkg preserves local edits
# across upgrades and removes them cleanly on purge.
cat >"$STAGE/DEBIAN/conffiles" <<'EOF'
/etc/xdg/autostart/audio-gui-restore.desktop
EOF

# postinst / postrm: refresh the desktop database, best-effort. No init-system
# work whatsoever (this is what keeps the package systemd-independent).
cat >"$STAGE/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if command -v update-desktop-database >/dev/null 2>&1; then
  update-desktop-database -q /usr/share/applications 2>/dev/null || true
fi
# The icon theme cache. Best-effort and non-fatal: an unrefreshed cache costs a
# menu icon, not a working program, and the window's own _NET_WM_ICON is
# compiled in and needs nothing on disk at all.
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
  gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor 2>/dev/null || true
fi
exit 0
EOF

cat >"$STAGE/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
if command -v update-desktop-database >/dev/null 2>&1; then
  update-desktop-database -q /usr/share/applications 2>/dev/null || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
  gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor 2>/dev/null || true
fi
exit 0
EOF
chmod 0755 -- "$STAGE/DEBIAN/postinst" "$STAGE/DEBIAN/postrm"

# ---- build the .deb ---------------------------------------------------------

step "Building package"
# Root-owned files inside the archive (fakeroot not required for --root-owner-group).
dpkg-deb --root-owner-group --build "$STAGE" "$OUT"

step "Done"
info "$OUT"
info "$(du -h "$OUT" | cut -f1)"
info "Install with:  sudo apt install ./$(basename -- "$OUT")"
info "          or:  sudo dpkg -i $(basename -- "$OUT")  # then 'apt -f install' for deps"
