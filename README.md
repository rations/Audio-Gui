# Audio-Gui

A small desktop app for controlling Linux audio on **ALSA-only**
systems — no PulseAudio daemon, no PipeWire. It combines the everyday
`alsamixer` controls with a toggle for the bundled PulseAudio-compatibility
bridges, so apps that only speak PulseAudio still get sound.

**No widget toolkit.** The interface is drawn directly with Cairo and FreeType on
a plain X11 window — no Qt, no GTK, no GLib. The whole runtime dependency set is
`libasound2`, `libcairo2`, `libfreetype6` and `libX11`.

## What it does

- **Mixer** — horizontal volume sliders with mute toggles for Master, Headphone,
  Speaker, Microphone and Mic Boost (only the controls your card actually has are
  shown), driven directly through `libasound` (`snd_mixer`). External changes
  (e.g. `amixer`, media keys) are reflected live.
- **Output level meter** — a live stereo peak meter fed by whichever bridge is
  running (the bridge publishes its mixed peak in a shared-memory page).
  Output meters do not work in ALSA (no bridge)
- **Audio options** — a 3-way routing choice (the two bridges are
  mutually exclusive, they bind the same PA socket):
  - **PA Bridge → ALSA** *(default)* — runs `pa-alsa-bridge`.
  - **ALSA (no bridge)** — nothing runs; only native-ALSA apps get sound.
  - **PA Bridge → JACK** — runs `pulse-jack-bridge`, exposing `pulse_bridge`
    playback ports in a JACK patchbay. Needs to be connected manually in Jack Graph, Qjackctl  
    Enabled only while `jackd` is running (detected live).
- **Output device** — a dropdown that switches the bridge's output between the
  detected cards (internal / USB / HDMI) **live**, without restarting the app.
  Devices are discovered straight from `libasound` (no shelling out) and the list
  refreshes automatically when you plug or unplug a USB interface. The mixer
  controls follow the selected card; HDMI outputs, which usually have no mixer
  controls, show a placeholder. The choice persists and is restored at login.
  (Applies to the PA Bridge → ALSA path; the dropdown is disabled in the other
  modes.)
- **Switches** — Capture and IEC958 (S/PDIF) toggles, when the card provides them.
- **Level meters** — stereo output meters fed by the running bridge through a
  lock-free shared-memory page, with instant attack, exponential release and a
  peak marker that holds about a second before falling. They idle at zero cost:
  once everything has settled to silence the window stops repainting entirely.
- **Appearance** — a self-contained look that is the same on every system, because
  the program draws it itself and ships its own fonts rather than inheriting a
  desktop theme. A dark/light toggle (dark by default) and an accent colour
  (green / orange / blue / yellow) sit at the bottom of the window; both persist.

The bridge runs **independently of the GUI**: it keeps playing after you close the
window, the chosen mode is remembered, and it is brought back up at login by an
XDG autostart entry (`audio-gui --restore`) — so PA-compat audio works without the
GUI ever being opened and survives a reboot as whatever was last selected. The
autostart entry (`~/.config/autostart/audio-gui-restore.desktop`) is created/kept
current the first time you run the GUI. Switching modes in the GUI updates both the
running bridge and the saved choice.

There is **no PulseAudio server, no PipeWire** anywhere — the bridges are minimal
PA-protocol sockets that expose just enough for apps to believe PulseAudio is
present. Only UNIX sockets are used (no dependency on systemd).

The `pa-alsa-bridge` implements both **playback** and **capture**: besides games and
browsers, it negotiates the **extended format API** used by players like VLC (which
send the stream's PCM format as a format list rather than a plain sample spec) and
answers sink-input introspection, so those players connect and play. Each playback
stream is **resampled** from the app's own sample rate to the device rate and its
sample format converted (16-bit, 24-bit packed, 24-in-32-bit and 32-bit integer, and
32-bit float are all accepted), so players that don't adapt to the advertised rate or
format (e.g. Parole playing 24-bit FLAC/WAV) still play cleanly. Per-stream **volume and mute** are honoured (the cubic
PulseAudio mapping), so a player's own volume slider works. It also exposes a
**monitor source** (`alsa_sink.monitor`) with real record streams, so screen
recorders and capture apps (Simple Screen Recorder, OBS, Audacity, voice chat) can
record the mixed output. The monitor is taken post-mix, so it is unaffected by live
output-device switching. Record streams are likewise resampled/converted to whatever
sample rate and format the recording app requests.

On first run, if you have **no ALSA configuration at all** (neither `~/.asoundrc`
nor `/etc/asound.conf`), Audio-Gui writes a baseline `~/.asoundrc` that sets up a
software-mixing (`dmix`/`dsnoop`) `default` on your internal card, so the bridge
and ordinary ALSA apps can share the device on any distro. If you already have
either file it is **left untouched** — delete the generated `~/.asoundrc` to fall
back to ALSA's built-in default.

Everything runs at user privilege; opening a specific card (`hw:`/`plughw:`)
needs only membership of the `audio` group, exactly as opening `default` does.

## Install (release tarball)

Grab a release `audio-gui-<version>.tar.gz`, unpack it, and run the installer:

```sh
tar xzf audio-gui-<version>.tar.gz
cd Audio-Gui
chmod +x install.sh && ./install.sh
```

`install.sh` installs everything **per-user, no root** (binaries to `~/.local/bin`,
a menu entry to `~/.local/share/applications`) — only the dependency step uses
`sudo` to pull the runtime packages via your distro's package manager
(apt/dnf/yum/pacman/zypper/apk). It also writes a generic `~/.asoundrc`, backing
up any existing one to `~/.asoundrc.bak`. Useful flags:

- `--from-source` — build from the bundled source instead of the prebuilt binaries.
- `--no-deps` — don't touch the package manager (install dependencies yourself).
- `--no-asoundrc` — leave `~/.asoundrc` alone.
- `PREFIX=/usr/local ./install.sh` — system-wide instead of `~/.local`.

The tarball ships **prebuilt binaries plus the full source**. By default
`install.sh` uses the prebuilt binaries when they resolve their libraries on your
system; if they can't (different glibc/Cairo), it **automatically builds from the
bundled source** instead. Remove with `./uninstall.sh` (`--purge` also drops
saved settings).

### Prebuilt binary requirements

The released binaries are built on **Debian 12 (bookworm)**, so they need:

| Requirement | Minimum version |
|---|---|
| glibc | **≥ 2.36** (Debian 12 / Ubuntu 22.04 / Fedora 37 era or newer) |
| Cairo | `libcairo2` |
| FreeType | `libfreetype6` |
| X11 | `libX11` |
| ALSA | `libasound2` + `alsa-utils` |
| JACK *(optional)* | any `libjack` — only for JACK routing |

The two fonts the interface draws with (Roboto, Michroma) are **bundled and
installed with the program**, so text measures the same everywhere and does not
depend on what fontconfig happens to offer.

On older systems (e.g. RHEL 8's glibc 2.28) the prebuilt binaries won't run — use
`./install.sh --from-source`, which builds against your own libraries.

## Build

```sh
cmake -B build
cmake --build build
```

Binaries land in `build/`: `audio-gui`, `pa-alsa-bridge`, and (if libjack is
present) `pulse-jack-bridge`. The GUI launches the bridges from alongside its own
executable, so keep them in the same directory.

### Dependencies

| | Packages (Debian/Ubuntu names) |
|---|---|
| Build | `cmake`, `pkg-config`, `g++`, `libcairo2-dev`, `libfreetype6-dev`, `libx11-dev`, `libasound2-dev`, `libjack-jackd2-dev` *(optional, for JACK routing)* |
| Runtime | `libasound2`, `alsa-utils`, `libcairo2`, `libfreetype6`, `libx11-6`; `jackd2`/`libjack` only for JACK mode |

If libjack is missing at build time, `pulse-jack-bridge` is skipped and the GUI
keeps the JACK option disabled.

### Checking the layout (`uirender`)

The build produces a second binary, `uirender`, which draws **the real panel** to
PNG with no X server and no sound card, then audits it:

```sh
./build/uirender --out /tmp/ui     # exit status 0 only if everything fits
```

It exists because two kinds of layout mistake are silent. A label wider than its
slot is truncated with an ellipsis, so the window still draws and the developer
never sees it — only a user with different font metrics does. And a character the
bundled fonts have no glyph for measures as **zero width**, so it passes a
does-it-fit check and renders as a hole; that is exactly how the routing labels'
`→` was caught after the Qt build had been falling back to a system font for it.

`uirender` also writes the meter at several levels, which is the one part of the
window you cannot check from a screenshot of the running program — by the time
you have the screenshot, the level has moved.

This is why `src/gfx/` and `src/panel.cpp` link Cairo and **never** X11: an
`#include <X11/Xlib.h>` in the drawing layer costs the whole offline audit.

### Creating a release

```sh
./release-tarball.sh          # version read from the VERSION file
./release-tarball.sh 0.2.0    # or override it for a one-off build
```

This builds in Release mode and produces `audio-gui-<version>.tar.gz` (prebuilt
binaries + bundled source + `install.sh`/`uninstall.sh`) that unpacks to a single
`Audio-Gui/` directory. Bump [`VERSION`](VERSION) when cutting a release. For the
widest compatibility, run it on the **oldest** system you support (the project
ships releases built on Debian 12) so the binaries' glibc floor stays low.

## Run

```sh
./build/audio-gui
```

Run as a normal user — no root required.
