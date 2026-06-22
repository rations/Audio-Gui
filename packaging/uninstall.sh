#!/usr/bin/env bash
#
# Audio-Gui uninstaller.
#
# Removes the installed binaries, the desktop menu entry, and the login
# autostart entry, and restores the user's previous ALSA config. System
# packages are left in place (other apps may depend on them).
#
# Usage: ./uninstall.sh [--purge] [--help]
#   --purge  also remove saved settings (~/.config/AudioGui).
#   PREFIX=/usr/local ./uninstall.sh   # match a system-wide install

set -euo pipefail

PREFIX="${PREFIX:-$HOME/.local}"
BINDIR="$PREFIX/bin"
APPDIR="$PREFIX/share/applications"

ASOUNDRC="$HOME/.asoundrc"
ASOUND_BACKUP="$HOME/.asoundrc.bak"
ASOUND_MARKER="Written by Audio-Gui"
AUTOSTART="${XDG_CONFIG_HOME:-$HOME/.config}/autostart/audio-gui-restore.desktop"
SETTINGS_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/AudioGui"

BINS=(audio-gui pa-alsa-bridge pulse-jack-bridge)

PURGE=0

usage()
{
  cat <<'EOF'
Audio-Gui uninstaller

Usage: ./uninstall.sh [options]

Options:
  --purge   Also remove saved settings (~/.config/AudioGui).
  --help    Show this help.

Environment:
  PREFIX    Install prefix to remove from (default: ~/.local).

System packages installed earlier are NOT removed (other apps may need them).
EOF
}

for arg in "$@"; do
  case "$arg" in
    --purge) PURGE=1 ;;
    -h | --help)
      usage
      exit 0
      ;;
    *)
      echo "uninstall.sh: unknown option '$arg'" >&2
      usage >&2
      exit 2
      ;;
  esac
done

info() { printf '  %s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; }

if [ "$(id -u)" -eq 0 ] && [ "$PREFIX" = "$HOME/.local" ]; then
  echo "ERROR: do not run as root for a per-user uninstall." >&2
  exit 1
fi

# ---- stop running bridges ---------------------------------------------------

step "Stopping any running bridges"
if command -v pkill >/dev/null 2>&1; then
  for b in pa-alsa-bridge pulse-jack-bridge; do
    if pkill -x "$b" 2>/dev/null; then
      info "stopped $b"
    fi
  done
else
  info "pkill not found; skipping (a running bridge exits on next reboot)"
fi

# ---- binaries ---------------------------------------------------------------

step "Removing binaries from $BINDIR"
for b in "${BINS[@]}"; do
  if [ -e "$BINDIR/$b" ]; then
    rm -f -- "$BINDIR/$b"
    info "removed $b"
  fi
done

# ---- desktop + autostart entries --------------------------------------------

step "Removing desktop and autostart entries"
if [ -e "$APPDIR/audio-gui.desktop" ]; then
  rm -f -- "$APPDIR/audio-gui.desktop"
  info "removed $APPDIR/audio-gui.desktop"
  if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$APPDIR" >/dev/null 2>&1 || true
  fi
fi
if [ -e "$AUTOSTART" ]; then
  rm -f -- "$AUTOSTART"
  info "removed $AUTOSTART"
fi

# ---- ALSA config ------------------------------------------------------------

step "Restoring ALSA config"
if [ -e "$ASOUND_BACKUP" ]; then
  # install.sh moved the user's original here when Audio-Gui took over. Put it
  # back, replacing whatever Audio-Gui wrote (or restoring it if the app never
  # wrote one because they uninstalled before logging back in).
  mv -f -- "$ASOUND_BACKUP" "$ASOUNDRC"
  info "restored your original ~/.asoundrc from $ASOUND_BACKUP"
elif [ -f "$ASOUNDRC" ] && grep -q "$ASOUND_MARKER" "$ASOUNDRC"; then
  rm -f -- "$ASOUNDRC"
  info "removed Audio-Gui ~/.asoundrc (no backup; ALSA falls back to its built-in default)"
else
  info "~/.asoundrc is not Audio-Gui-managed; left untouched"
fi

# ---- settings (purge only) --------------------------------------------------

if [ "$PURGE" -eq 1 ]; then
  step "Purging saved settings"
  if [ -d "$SETTINGS_DIR" ]; then
    rm -rf -- "$SETTINGS_DIR"
    info "removed $SETTINGS_DIR"
  fi
fi

step "Done"
info "Audio-Gui removed. System packages were left installed."
[ "$PURGE" -eq 0 ] && info "Settings kept in $SETTINGS_DIR (use --purge to remove)."
