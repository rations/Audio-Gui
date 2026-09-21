// BridgeManager.h
// Owns the PulseAudio-compat routing choice and the (mutually exclusive) bridge
// process. Exactly one bridge runs at a time because both bridges bind the same
// PA socket:
//   PulseToAlsa  -> pa-alsa-bridge  (default)
//   PureAlsa     -> no bridge running
//   PulseToJack  -> pulse-jack-bridge
//
// The bridge is launched DETACHED, so it keeps running after the GUI closes —
// audio for PA-compat apps must work whether or not the GUI is open. The chosen
// mode is persisted (config.h) and restored at login via `audio-gui --restore`,
// so a reboot comes back to whatever was selected before. The currently running
// bridge is tracked through a runtime pidfile so a relaunched GUI can still
// stop/switch it.
//
// Bridges are launched by absolute path next to the GUI binary (never a shell).
// JACK availability is probed with a runtime-loaded libjack, so the JACK option
// enables/disables live as jackd starts or stops.
//
// THIS CLASS OWNS NO TIMER. The Qt build drove probeJack() from a QTimer; there is
// no Qt event loop any more, so the owner ticks it -- main.cpp registers a
// kJackProbeIntervalMs timer on the window and calls probeJack() from it. The
// headless --restore path simply never does, which is what the old
// `enableJackProbe` constructor flag bought.
#pragma once

#include <functional>
#include <string>

class BridgeManager
{
public:
    enum class Mode { PulseToAlsa = 0, PureAlsa = 1, PulseToJack = 2 };

    // How often the owner should call probeJack(). Live detection is the point: the
    // JACK radio has to enable itself when the user starts jackd in another window.
    static constexpr int kJackProbeIntervalMs = 2000;

    // loadJack: the interactive GUI wants jackd detection; the headless --restore
    // path does not, and dlopening libjack there would be work for nothing.
    explicit BridgeManager(bool loadJack);
    ~BridgeManager();

    BridgeManager(const BridgeManager &) = delete;
    BridgeManager &operator=(const BridgeManager &) = delete;

    // Replace the Qt signals of the same names.
    std::function<void(bool available)> onJackAvailabilityChanged;
    std::function<void(Mode mode)> onModeChanged;
    std::function<void(const std::string &token)> onDeviceChanged;

    // The persisted choice (what a reboot should restore). Defaults to PulseToAlsa.
    Mode savedMode() const;

    // What is actually active now: the running bridge's mode if one is running,
    // else the saved mode.
    Mode currentMode() const;

    bool jackAvailable() const
    {
        return m_jackAvailable;
    }

    // Ask libjack whether a server is up, firing onJackAvailabilityChanged if the
    // answer changed. Cheap enough to call on a 2 s timer; does nothing if libjack
    // was not loaded.
    void probeJack();

    // The persisted output-device choice as a stable token ("<cardId>:<pcmIndex>").
    // Empty means the ALSA "default" PCM (Internal). PulseToAlsa points the bridge
    // at it (live); PureAlsa points ALSA's default PCM at it (effective on app
    // restart). PulseToJack ignores it (audio routes through jackd).
    std::string currentDevice() const;

    // Ensure the saved/default mode is active without needless restarts: if the
    // right bridge is already running (e.g. started at login) it is left alone.
    void ensureActive();

    // Headless login restore: force the saved mode to be (re)applied.
    void restoreSavedMode();

    // User picked a routing option: apply it now and persist the choice.
    void setMode(Mode mode);

    // User picked an output device (stable token; empty = "default"): persist it.
    // PulseToAlsa switches the running bridge live; PureAlsa rewrites ALSA's
    // default PCM (effective when apps restart).
    void setDevice(const std::string &token);

private:
    void applyMode(Mode mode, bool force);
    void stopRunningBridge();
    long startBridgeDetached(const std::string &binary, const std::string &alsaDevice);
    std::string bridgePath(const std::string &name) const;

    // Resolve the persisted card id to a "PULSE_BRIDGE_ALSA_DEV" string at start
    // time (so a card that returned at a new index still works). Empty = default.
    std::string resolveAlsaDevice() const;

    // Write a baseline dmix/dsnoop ~/.asoundrc, but only if the user has no ALSA
    // config yet (~/.asoundrc and /etc/asound.conf both absent). Never overwrites.
    void ensureBaselineAsoundrc() const;

    // Pure-ALSA device routing: rewrite ALSA's default PCM to play through the
    // persisted device (capture stays on the internal card). Native ALSA apps
    // started afterwards open the new default — not live, by design (no snd-aloop).
    void applyPureAlsaDefault() const;

    // The dmix/dsnoop "default" asoundrc body, slaved to the given playback/capture
    // PCMs with ctl on the given card.
    std::string asoundrcContents(const std::string &playbackSlave, const std::string &captureSlave,
                                 const std::string &ctlCard) const;

    // Write the managed ~/.asoundrc, but only when it is absent or a file we wrote
    // ourselves — never clobber the user's or distro's hand-rolled config.
    void writeAsoundrc(const std::string &playbackSlave, const std::string &captureSlave,
                       const std::string &ctlCard) const;

    // Persisted choices.
    void persistMode(Mode mode);
    void persistDevice(const std::string &cardId);

    // Tell the running PA→ALSA bridge to switch output device in place (write the
    // control file + SIGUSR1), so client apps keep playing. False if it could not.
    bool liveSwitchDevice(long pid) const;
    // Control file the bridge reads on SIGUSR1 (alongside the runtime pidfile).
    std::string ctlFilePath() const;

    // Runtime state (pidfile) — survives a GUI restart, cleared on reboot.
    std::string runStatePath() const;
    void writeRunState(Mode mode, long pid);
    void clearRunState();
    bool readRunState(Mode *mode, long *pid) const;
    static bool pidAlive(long pid);

    bool m_jackAvailable = false;
    void *m_jackLib = nullptr; // runtime-loaded libjack (null if absent)
};
