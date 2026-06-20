# Audio-Gui

A small, modern Qt6 desktop app for controlling Linux audio on **ALSA-only**
systems — no PulseAudio daemon, no PipeWire. It combines the everyday
`alsamixer` controls with a toggle for the bundled PulseAudio-compatibility
bridges, so apps that only speak PulseAudio still get sound.

## What it does

- **Mixer** — horizontal volume sliders with mute toggles for Master, Headphone,
  Speaker, Microphone and Mic Boost (only the controls your card actually has are
  shown), driven directly through `libasound` (`snd_mixer`). External changes
  (e.g. `amixer`, media keys) are reflected live.
- **Output level meter** — a live stereo peak meter fed by whichever bridge is
  running (the bridge publishes its mixed peak in a shared-memory page).
- **Audio options** — a 3-way routing choice (the two bridges are
  mutually exclusive, they bind the same PA socket):
  - **PA Bridge → ALSA** *(default)* — runs `pa-alsa-bridge`.
  - **Pure ALSA (no bridge)** — nothing runs; only native-ALSA apps get sound.
  - **PA Bridge → JACK** — runs `pulse-jack-bridge`, exposing `pulse_bridge`
    playback ports in a JACK patchbay. Enabled only while `jackd` is running
    (detected live).
- **Switches** — Capture and IEC958 (S/PDIF) toggles, when the card provides them.
- **Appearance** — a self-contained modern theme (Fusion + stylesheet) that looks the
  same on every system rather than inheriting the user's Qt/GTK theme. A dark/light
  toggle (dark by default) and an accent colour (green / orange / blue / yellow) sit at
  the top of the window; both choices persist.

The bridge runs **independently of the GUI**: it keeps playing after you close the
window, the chosen mode is remembered, and it is brought back up at login by an
XDG autostart entry (`audio-gui --restore`) — so PA-compat audio works without the
GUI ever being opened and survives a reboot as whatever was last selected. The
autostart entry (`~/.config/autostart/audio-gui-restore.desktop`) is created/kept
current the first time you run the GUI. Switching modes in the GUI updates both the
running bridge and the saved choice.

There is **no PulseAudio server, no PipeWire** anywhere — the bridges are minimal
PA-protocol sockets that expose just enough for apps to believe PulseAudio is
present. Only UNIX sockets are used (no dependency on systemd or Wayland).

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
| Build | `cmake`, `pkg-config`, `qt6-base-dev`, `libasound2-dev`, `libjack-jackd2-dev` *(optional, for JACK routing)* |
| Runtime | `libasound2`, `alsa-utils`, Qt6 Widgets runtime; `jackd2`/`libjack` only for JACK mode |

If libjack is missing at build time, `pulse-jack-bridge` is skipped and the GUI
keeps the JACK option disabled.

## Run

```sh
./build/audio-gui
```

Run as a normal user — no root required.
