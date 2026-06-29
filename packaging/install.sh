#!/usr/bin/env bash
#
# Audio-Gui installer (system-wide or per-user).
#
# Installs Audio-Gui into the chosen prefix, registers a desktop menu entry,
# installs a login autostart entry (audio-gui --restore), and installs the
# packages Audio-Gui needs via the system package manager.
#
# Install mode is chosen interactively (system-wide /usr/local — the default —
# or per-user ~/.local), unless PREFIX is given in the environment. A system
# install serves every user: its autostart entry goes in /etc/xdg/autostart so
# the generic live user on a snapshot ISO gets audio routing at login too.
#
# Audio-Gui manages ~/.asoundrc: it (re)writes a correct, per-card ALSA "default"
# whenever it runs — at login (via the headless `--restore` autostart entry) and
# on every device switch. Because the running app refuses to overwrite a config
# it did not write, the *installer* is what takes ownership: it moves any existing
# ~/.asoundrc aside to ~/.asoundrc.bak (the uninstaller restores it), then runs
# `audio-gui --restore` so the app writes its own config and activates routing
# right away — no need to log out first.
#
# By default it uses the bundled prebuilt binaries when they resolve their
# libraries on this system; otherwise (or with --from-source) it builds from the
# bundled source tree. Only dependency installation is privileged (uses sudo);
# every file this script writes lands under $PREFIX (a per-user install also
# touches ~/ for autostart and ~/.asoundrc).
#
# Usage: ./install.sh [--from-source] [--no-deps] [--no-autostart] [--help]
#   sudo PREFIX=/usr/local ./install.sh   # system-wide, no prompt (needs root)
#   PREFIX=$HOME/.local ./install.sh      # per-user, no prompt

set -euo pipefail

# Directory this script lives in (the unpacked Audio-Gui/ tree).
SELF_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

# PREFIX may be given in the environment (wins, no prompt). Otherwise the install
# mode is chosen interactively below (system-wide vs per-user), mirroring
# install-jackdaw.sh. All PREFIX-derived paths are resolved in set_paths(), after
# the mode is settled.
PREFIX_FROM_ENV=0
[ -n "${PREFIX:-}" ] && PREFIX_FROM_ENV=1
PREFIX="${PREFIX:-}"

SYSTEM_INSTALL=0   # set by set_paths(): 1 when PREFIX is outside $HOME
BINDIR=""
APPDIR=""
AUTOSTART_DIR=""
AUTOSTART_FILE=""

ASOUNDRC="$HOME/.asoundrc"
ASOUND_MARKER="Written by Audio-Gui"

# The bridge binaries the GUI launches from alongside itself; pulse-jack-bridge
# is optional (only present when libjack was available at build time).
BINS=(audio-gui pa-alsa-bridge)
OPT_BINS=(pulse-jack-bridge)

DO_DEPS=1
DO_AUTOSTART=1
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
  --no-autostart  Do not install the login autostart entry. Audio-Gui then only
                  generates ~/.asoundrc and applies routing when you launch it.
  --help          Show this help.

Environment:
  PREFIX          Install prefix. If set, skips the interactive system/user
                  prompt. Use /usr/local for a system-wide install (run as root;
                  autostart goes to /etc/xdg/autostart for all users) or
                  $HOME/.local for a per-user install.
EOF
}

for arg in "$@"; do
  case "$arg" in
    --from-source) FROM_SOURCE=1 ;;
    --no-deps) DO_DEPS=0 ;;
    --no-autostart) DO_AUTOSTART=0 ;;
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

# Choose system-wide vs per-user when PREFIX was not given in the environment.
# Mirrors install-jackdaw.sh: system-wide (/usr/local) is the default. A system
# install serves every user — including the generic live user on a snapshot ISO —
# so its files land outside any one home and survive the home rename.
choose_prefix()
{
  if [ "$PREFIX_FROM_ENV" -eq 1 ]; then
    info "Using PREFIX from environment: $PREFIX"
    return
  fi
  if [ ! -t 0 ]; then
    PREFIX="$HOME/.local"   # non-interactive, no PREFIX: keep the safe default
    return
  fi
  printf '%s\n' "Where should Audio-Gui be installed?" >&2
  printf '  %s\n' "[1] system-wide  (/usr/local) - every user; needs root/sudo" >&2
  printf '  %s\n' "[2] this user    ($HOME/.local) - files in your home only" >&2
  printf '%s' "Choice [1]: " >&2
  read -r _c || _c=1
  case "${_c:-1}" in
    "" | 1) PREFIX="/usr/local" ;;
    2) PREFIX="$HOME/.local" ;;
    *) die "invalid choice: $_c" ;;
  esac
}

# Resolve all PREFIX-derived paths and the install mode. A system install (PREFIX
# outside the invoking user's home) puts its autostart entry in the XDG *system*
# autostart dir (/etc/xdg/autostart), which every user's session reads, with an
# absolute Exec — so the generic live user gets `audio-gui --restore` at login.
# A per-user install keeps the entry in ~/.config/autostart as before.
set_paths()
{
  BINDIR="$PREFIX/bin"
  APPDIR="$PREFIX/share/applications"
  case "$PREFIX/" in
    "$HOME"/*) SYSTEM_INSTALL=0 ;;
    *) SYSTEM_INSTALL=1 ;;
  esac
  if [ "$SYSTEM_INSTALL" -eq 1 ]; then
    AUTOSTART_DIR="/etc/xdg/autostart"
  else
    AUTOSTART_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/autostart"
  fi
  AUTOSTART_FILE="$AUTOSTART_DIR/audio-gui-restore.desktop"
}

choose_prefix
set_paths

# A system install writes to /usr/local and /etc/xdg — that needs root. Rather
# than bail, re-exec under sudo (which prompts for the password). PREFIX is passed
# through so the re-exec skips the prompt and goes straight to the system install.
if [ "$SYSTEM_INSTALL" -eq 1 ] && [ "$(id -u)" -ne 0 ]; then
  if command -v sudo >/dev/null 2>&1; then
    info "system-wide install needs root — re-running under sudo (you may be prompted for your password)."
    exec sudo PREFIX="$PREFIX" "$SELF_DIR/$(basename -- "${BASH_SOURCE[0]}")" "$@"
  fi
  die "system-wide install needs root and sudo was not found. Re-run as root: PREFIX=$PREFIX $0"
fi

# Refuse to run as root for a per-user prefix: it would install into root's home
# and root's autostart. A system PREFIX may legitimately run as root.
if [ "$(id -u)" -eq 0 ] && [ "$SYSTEM_INSTALL" -eq 0 ]; then
  die "do not run as root for a per-user install. Run as your normal user, or choose the system-wide option (PREFIX=/usr/local)."
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
  elif command -v xbps-install >/dev/null 2>&1; then
    $sudo xbps-install -Sy alsa-utils alsa-lib qt6-base jack \
      || warn "xbps could not install all packages; install them manually."
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
  elif command -v xbps-install >/dev/null 2>&1; then
    $sudo xbps-install -Sy base-devel cmake qt6-base-devel alsa-lib-devel jack-devel \
      || warn "xbps could not install all build packages; install them manually."
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

# ---- 4. take ownership of ~/.asoundrc (backing up the user's own) -----------

# Audio-Gui rewrites ~/.asoundrc at login and on every device switch, but the
# running app only ever rewrites a file it wrote itself — it will not overwrite a
# hand-rolled config. So the installer performs the one-time takeover: move the
# user's existing ~/.asoundrc to ~/.asoundrc.bak so it survives, then (below) run
# `audio-gui --restore` to write the correct per-card replacement. The uninstaller
# restores the backup. We do NOT write a replacement by hand here (the generic one
# we used to write did not match the user's card).
back_up_user_asoundrc()
{
  # Nothing to do if there is no file, or it is already one we manage.
  [ -e "$ASOUNDRC" ] || return 0
  grep -q "$ASOUND_MARKER" "$ASOUNDRC" 2>/dev/null && return 0

  # Never clobber an earlier ~/.asoundrc.bak — it holds the *real* original from a
  # previous install. Keep any later config as a timestamped copy instead.
  local backup="$ASOUNDRC.bak"
  if [ -e "$backup" ]; then
    backup="$ASOUNDRC.bak.$(date +%Y%m%d%H%M%S)"
    cp -p -- "$ASOUNDRC" "$backup"
    rm -f -- "$ASOUNDRC"
  else
    mv -- "$ASOUNDRC" "$backup"
  fi
  info "backed up your existing ~/.asoundrc to $backup (the uninstaller restores it)"
}

# Only meaningful for a per-user install. A system install serves every user, so
# there is no single ~/.asoundrc to take over here; each user's per-card config is
# written at their first login by the /etc/xdg/autostart `--restore` entry below.
if [ "$SYSTEM_INSTALL" -eq 0 ]; then
  step "Backing up any existing ~/.asoundrc"
  back_up_user_asoundrc
fi

# ---- 5. login autostart entry (this is what writes ~/.asoundrc) -------------

# We deliberately do NOT write ~/.asoundrc here. Audio-Gui writes a correct,
# per-card ~/.asoundrc itself (and never clobbers a hand-rolled one) the first
# time it runs — including the headless `audio-gui --restore` this entry runs at
# login. Installing the entry now means a log out / log back in is enough to get
# your ALSA "default"; the user never has to open the GUI.
write_autostart_entry()
{
  mkdir -p -- "$AUTOSTART_DIR"
  # Keep this byte-for-byte identical to ensureAutostartEntry() in MainWindow.cpp
  # so the GUI sees it as up to date and does not rewrite it on first launch.
  cat >"$AUTOSTART_FILE" <<EOF
[Desktop Entry]
Type=Application
Name=Audio routing (restore)
Comment=Restore the saved PulseAudio-compat audio routing
Exec="$BINDIR/audio-gui" --restore
Terminal=false
NoDisplay=true
X-GNOME-Autostart-enabled=true
EOF
  chmod 0644 -- "$AUTOSTART_FILE"
}

if [ "$DO_AUTOSTART" -eq 1 ]; then
  step "Installing login autostart entry to $AUTOSTART_DIR"
  write_autostart_entry
  info "installed audio-gui-restore.desktop"
else
  step "Skipping login autostart entry (--no-autostart)"
fi

# ---- 6. activate routing now ------------------------------------------------

# Run the same headless path the autostart entry uses at login, so Audio-Gui
# writes its per-card ~/.asoundrc and brings up the default routing immediately —
# the user does not have to log out or open the GUI first. Best-effort: this can
# fail in a session with no audio access (e.g. plain SSH), in which case the
# autostart entry handles it at the next login.
ASOUNDRC_WRITTEN=0
if [ "$SYSTEM_INSTALL" -eq 1 ]; then
  # System install: don't write the installing account's ~/.asoundrc (it would be
  # root's, and is per-card so it must be generated on the target hardware). Each
  # user gets it at their first login via the /etc/xdg/autostart entry.
  step "Audio routing"
  info "system install: each user's ~/.asoundrc is written at their first login (autostart)."
elif "$BINDIR/audio-gui" --restore >/dev/null 2>&1; then
  step "Activating audio routing"
  ASOUNDRC_WRITTEN=1
  info "wrote ~/.asoundrc and started the default routing"
else
  step "Activating audio routing"
  warn "could not activate routing now; it will start at your next login."
fi

# ---- summary ----------------------------------------------------------------

step "Done"
info "Binaries:     $BINDIR"
info "Menu entry:   $APPDIR/audio-gui.desktop"
[ "$DO_AUTOSTART" -eq 1 ] && info "Autostart:    $AUTOSTART_FILE"
case ":$PATH:" in
  *":$BINDIR:"*) ;;
  *)
    info "Note: $BINDIR is not on your PATH. Add it with:"
    info "      echo 'export PATH=\"$BINDIR:\$PATH\"' >> ~/.profile"
    ;;
esac
if [ -e "$ASOUNDRC.bak" ]; then
  info "Saved config: $ASOUNDRC.bak (restored by uninstall.sh)"
fi
info "Launch 'audio-gui' or pick \"Audio Control\" from your menu."
if [ "$ASOUNDRC_WRITTEN" -eq 0 ]; then
  printf '\n'
  warn "Audio routing is not active yet."
  info "Log out and back in to start it."
fi
