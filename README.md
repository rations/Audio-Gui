# Audio-Gui

A small, Qt6 desktop app for controlling Linux audio on **ALSA-only**
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
- **Output device** — a dropdown that switches the bridge's output between the
  detected cards (internal / USB / HDMI) **live**, without restarting the app.
  Devices are discovered straight from `libasound` (no shelling out) and the list
  refreshes automatically when you plug or unplug a USB interface. The mixer
  controls follow the selected card; HDMI outputs, which usually have no mixer
  controls, show a placeholder. The choice persists and is restored at login.
  (Applies to the PA Bridge → ALSA path; the dropdown is disabled in the other
  modes.)
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
