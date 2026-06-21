#!/usr/bin/env bash
#
# Audio-Gui installer (per-user, no root).
#
# Installs Audio-Gui into a per-user prefix, registers a desktop menu entry,
# installs a generic ~/.asoundrc (backing up any existing one), and installs the
# packages Audio-Gui needs via the system package manager.
#
# By default it uses the bundled prebuilt binaries when they resolve their
# libraries on this system; otherwise (or with --from-source) it builds from the
# bundled source tree. Only dependency installation is privileged (uses sudo);
# every file this script writes lands under $PREFIX (default ~/.local) and ~/.
#
# Usage: ./install.sh [--from-source] [--no-deps] [--no-asoundrc] [--help]
#   PREFIX=/usr/local ./install.sh   # system-wide install (needs write access)

set -euo pipefail

# Directory this script lives in (the unpacked Audio-Gui/ tree).
SELF_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

PREFIX="${PREFIX:-$HOME/.local}"
BINDIR="$PREFIX/bin"
APPDIR="$PREFIX/share/applications"

ASOUNDRC="$HOME/.asoundrc"
ASOUND_MARKER="Written by Audio-Gui"

# The bridge binaries the GUI launches from alongside itself; pulse-jack-bridge
# is optional (only present when libjack was available at build time).
BINS=(audio-gui pa-alsa-bridge)
OPT_BINS=(pulse-jack-bridge)

DO_DEPS=1
DO_ASOUNDRC=1
FROM_SOURCE=0

# Set once the build/prebuilt source of binaries is chosen.
SRC_BIN_DIR=""
BUILD_TMP=""

usage()
{
  cat <<'EOF'
Audio-Gui installer

Usage: ./install.sh [options]

Options:
  --from-source   Build from the bundled source instead of using the prebuilt
                  binaries (also done automatically if the prebuilt ones cannot
                  run on this system).
  --no-deps       Do not install system packages (install dependencies yourself).
  --no-asoundrc   Do not write a generic ~/.asoundrc.
  --help          Show this help.

Environment:
  PREFIX          Install prefix (default: ~/.local). Set to e.g. /usr/local for
                  a system-wide install (requires write access to that prefix).
EOF
}

for arg in "$@"; do
  case "$arg" in
    --from-source) FROM_SOURCE=1 ;;
    --no-deps) DO_DEPS=0 ;;
    --no-asoundrc) DO_ASOUNDRC=0 ;;
    -h | --help)
      usage
      exit 0
      ;;
    *)
      echo "install.sh: unknown option '$arg'" >&2
      usage >&2
      exit 2
      ;;
  esac
done

info() { printf '  %s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; }
warn() { printf 'WARNING: %s\n' "$*" >&2; }
die()
{
  printf 'ERROR: %s\n' "$*" >&2
  exit 1
}

cleanup() { [ -n "$BUILD_TMP" ] && rm -rf -- "$BUILD_TMP"; }
trap cleanup EXIT

# Refuse to run as root for the default per-user prefix: it would install into
# root's home and write root's ~/.asoundrc. A system PREFIX may legitimately run
# as root.
if [ "$(id -u)" -eq 0 ] && [ "$PREFIX" = "$HOME/.local" ]; then
  die "do not run as root for a per-user install. Run as your normal user, or set PREFIX=/usr/local."
fi

# ---- dependency installation ------------------------------------------------

# Echo the privilege-escalation prefix ("sudo" or ""), or fail if unavailable.
sudo_prefix()
{
  if [ "$(id -u)" -eq 0 ]; then
    printf ''
  elif command -v sudo >/dev/null 2>&1; then
    printf 'sudo'
  else
    return 1
  fi
}

# install_pkgs <kind> <apt...> -- <dnf...> -- <pacman...> -- <zypper...> -- <apk...>
# Picks the list matching the detected package manager. <kind> is for messages.
install_runtime_deps()
{
  local sudo
  if ! sudo="$(sudo_prefix)"; then
    warn "no sudo found; skipping dependency install. Install the runtime libraries manually."
    return 0
  fi
  if command -v apt-get >/dev/null 2>&1; then
    $sudo apt-get install -y alsa-utils libasound2 libqt6widgets6 libjack-jackd2-0 \
      || warn "apt-get could not install all packages; install them manually."
  elif command -v dnf >/dev/null 2>&1; then
    $sudo dnf install -y alsa-utils alsa-lib qt6-qtbase jack-audio-connection-kit \
      || warn "dnf could not install all packages; install them manually."
  elif command -v yum >/dev/null 2>&1; then
    $sudo yum install -y alsa-utils alsa-lib qt6-qtbase jack-audio-connection-kit \
      || warn "yum could not install all packages; install them manually."
  elif command -v pacman >/dev/null 2>&1; then
    $sudo pacman -S --needed --noconfirm alsa-utils alsa-lib qt6-base jack2 \
      || warn "pacman could not install all packages; install them manually."
  elif command -v zypper >/dev/null 2>&1; then
    $sudo zypper install -y alsa-utils libasound2 libQt6Widgets6 libjack0 \
      || warn "zypper could not install all packages; install them manually."
  elif command -v apk >/dev/null 2>&1; then
    $sudo apk add alsa-utils alsa-lib qt6-qtbase jack \
      || warn "apk could not install all packages; install them manually."
  else
    warn "no supported package manager found. Install these runtime libraries manually:"
    warn "  ALSA (libasound + alsa-utils), Qt6 Widgets, and optionally libjack for JACK routing."
  fi
}

install_build_deps()
{
  local sudo
  if ! sudo="$(sudo_prefix)"; then
    warn "no sudo found; skipping build-dependency install. Install the toolchain manually."
    return 0
  fi
  if command -v apt-get >/dev/null 2>&1; then
    $sudo apt-get install -y cmake pkg-config g++ qt6-base-dev libasound2-dev libjack-jackd2-dev \
      || warn "apt-get could not install all build packages; install them manually."
  elif command -v dnf >/dev/null 2>&1; then
    $sudo dnf install -y cmake pkgconf-pkg-config gcc-c++ qt6-qtbase-devel alsa-lib-devel \
      jack-audio-connection-kit-devel || warn "dnf could not install all build packages; install them manually."
  elif command -v yum >/dev/null 2>&1; then
    $sudo yum install -y cmake pkgconf-pkg-config gcc-c++ qt6-qtbase-devel alsa-lib-devel \
      jack-audio-connection-kit-devel || warn "yum could not install all build packages; install them manually."
  elif command -v pacman >/dev/null 2>&1; then
    $sudo pacman -S --needed --noconfirm cmake pkgconf gcc qt6-base alsa-lib jack2 \
      || warn "pacman could not install all build packages; install them manually."
  elif command -v zypper >/dev/null 2>&1; then
    $sudo zypper install -y cmake pkg-config gcc-c++ qt6-base-devel alsa-devel libjack-devel \
      || warn "zypper could not install all build packages; install them manually."
  elif command -v apk >/dev/null 2>&1; then
    $sudo apk add cmake pkgconf g++ make qt6-qtbase-dev alsa-lib-dev jack-dev \
      || warn "apk could not install all build packages; install them manually."
  else
    warn "no supported package manager found. Install a C/C++ toolchain, cmake, pkg-config,"
    warn "  Qt6 Widgets dev, ALSA dev, and (optional) libjack dev manually."
  fi
}

# ---- build from source ------------------------------------------------------

build_from_source()
{
  local srcdir="$SELF_DIR/source"
  [ -f "$srcdir/CMakeLists.txt" ] || die "no bundled source tree; cannot build from source."
  command -v cmake >/dev/null 2>&1 || die "cmake not found. Install build dependencies (drop --no-deps) and retry."

  step "Building from source"
  BUILD_TMP="$(mktemp -d)"
  cmake -S "$srcdir" -B "$BUILD_TMP" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$BUILD_TMP" -j"$(nproc 2>/dev/null || echo 2)"
  SRC_BIN_DIR="$BUILD_TMP"
}

# True if the prebuilt audio-gui exists and resolves all its libraries here.
prebuilt_runnable()
{
  local b="$SELF_DIR/bin/audio-gui"
  [ -f "$b" ] || return 1
  command -v ldd >/dev/null 2>&1 || return 0 # can't check; assume usable
  ! ldd "$b" 2>/dev/null | grep -q 'not found'
}

# ---- 1. dependencies + choose binary source ---------------------------------

if [ "$DO_DEPS" -eq 1 ]; then
  step "Installing runtime dependencies"
  install_runtime_deps
else
  step "Skipping dependency install (--no-deps)"
fi

# Decide prebuilt vs source *after* runtime deps are in place, so the ldd check
# reflects the libraries actually available.
if [ "$FROM_SOURCE" -eq 0 ]; then
  if prebuilt_runnable; then
    SRC_BIN_DIR="$SELF_DIR/bin"
    info "using prebuilt binaries"
  elif [ -f "$SELF_DIR/source/CMakeLists.txt" ]; then
    warn "prebuilt binaries are missing or cannot run here — building from source instead."
    FROM_SOURCE=1
  else
    die "prebuilt binaries cannot run here and no source tree is bundled."
  fi
fi

if [ "$FROM_SOURCE" -eq 1 ]; then
  [ "$DO_DEPS" -eq 1 ] && { step "Installing build dependencies"; install_build_deps; }
  build_from_source
fi

# ---- 2. binaries ------------------------------------------------------------

step "Installing binaries to $BINDIR"
mkdir -p -- "$BINDIR"

for b in "${BINS[@]}"; do
  [ -f "$SRC_BIN_DIR/$b" ] || die "missing binary: $b (build failed or incomplete tarball?)"
  install -m 0755 -- "$SRC_BIN_DIR/$b" "$BINDIR/$b"
  info "installed $b"
done

for b in "${OPT_BINS[@]}"; do
  if [ -f "$SRC_BIN_DIR/$b" ]; then
    install -m 0755 -- "$SRC_BIN_DIR/$b" "$BINDIR/$b"
    info "installed $b"
  else
    info "skipped $b (not built; JACK routing stays disabled)"
  fi
done

# Best-effort: warn if the installed binary can't resolve its shared libraries.
if command -v ldd >/dev/null 2>&1 && ldd "$BINDIR/audio-gui" 2>/dev/null | grep -q 'not found'; then
  warn "audio-gui has unresolved libraries (Qt6/ALSA may be missing):"
  ldd "$BINDIR/audio-gui" 2>/dev/null | grep 'not found' >&2 || true
fi

# ---- 3. desktop menu entry --------------------------------------------------

step "Installing desktop menu entry to $APPDIR"
mkdir -p -- "$APPDIR"
sed "s|@EXEC@|\"$BINDIR/audio-gui\"|" "$SELF_DIR/audio-gui.desktop.in" >"$APPDIR/audio-gui.desktop"
chmod 0644 -- "$APPDIR/audio-gui.desktop"
info "installed audio-gui.desktop"
if command -v update-desktop-database >/dev/null 2>&1; then
  update-desktop-database "$APPDIR" >/dev/null 2>&1 || true
fi

# ---- 4. generic ~/.asoundrc -------------------------------------------------

is_audiogui_asoundrc()
{
  [ -f "$ASOUNDRC" ] && grep -q "$ASOUND_MARKER" "$ASOUNDRC"
}

write_generic_asoundrc()
{
  cat >"$ASOUNDRC" <<'EOF'
# Written by Audio-Gui: software-mixing default output.
# Generic baseline installed by install.sh — Audio-Gui refines this per card.
# Delete this file to fall back to ALSA's built-in default.
pcm.!default {
  type plug
  slave.pcm "audiogui_asym"
}
ctl.!default {
  type hw
  card 0
}
pcm.audiogui_asym {
  type asym
  playback.pcm "audiogui_dmix"
  capture.pcm "audiogui_dsnoop"
}
pcm.audiogui_dmix {
  type dmix
  ipc_key 1024
  slave {
    pcm "hw:0,0"
    rate 48000
  }
}
pcm.audiogui_dsnoop {
  type dsnoop
  ipc_key 1025
  slave.pcm "hw:0,0"
}
EOF
}

if [ "$DO_ASOUNDRC" -eq 1 ]; then
  step "Installing generic ~/.asoundrc"
  if [ -e "$ASOUNDRC" ] && ! is_audiogui_asoundrc; then
    # Back up a hand-rolled config; never clobber an earlier backup.
    backup="$ASOUNDRC.bak"
    if [ -e "$backup" ]; then
      backup="$ASOUNDRC.bak.$(date +%Y%m%d%H%M%S)"
    fi
    cp -p -- "$ASOUNDRC" "$backup"
    info "backed up existing ~/.asoundrc to $backup"
  fi
  write_generic_asoundrc
  info "wrote generic ~/.asoundrc"
else
  step "Skipping ~/.asoundrc (--no-asoundrc)"
fi

# ---- summary ----------------------------------------------------------------

step "Done"
info "Binaries:     $BINDIR"
info "Menu entry:   $APPDIR/audio-gui.desktop"
[ "$DO_ASOUNDRC" -eq 1 ] && info "ALSA config:  $ASOUNDRC"
case ":$PATH:" in
  *":$BINDIR:"*) ;;
  *)
    info "Note: $BINDIR is not on your PATH. Add it with:"
    info "      echo 'export PATH=\"$BINDIR:\$PATH\"' >> ~/.profile"
    ;;
esac
info "Launch 'audio-gui' or pick \"Audio Control\" from your menu."
info "On first launch it adds a login autostart entry so audio works after reboot."
