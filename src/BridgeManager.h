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
// mode is persisted (QSettings) and restored at login via `audio-gui --restore`,
// so a reboot comes back to whatever was selected before. The currently running
// bridge is tracked through a runtime pidfile so a relaunched GUI can still
// stop/switch it.
//
// Bridges are launched by absolute path next to the GUI binary (never a shell).
// JACK availability is probed periodically with a runtime-loaded libjack, so the
// JACK option enables/disables live as jackd starts or stops.
#pragma once

#include <QObject>
#include <QString>

class QTimer;

class BridgeManager : public QObject
{
  Q_OBJECT

public:
  enum class Mode
  {
    PulseToAlsa = 0,
    PureAlsa = 1,
    PulseToJack = 2
  };

  // enableJackProbe: the interactive GUI wants live jackd detection; the headless
  // --restore path does not (it has no event loop).
  explicit BridgeManager(bool enableJackProbe, QObject* parent = nullptr);
  ~BridgeManager() override;

  // The persisted choice (what a reboot should restore). Defaults to PulseToAlsa.
  Mode savedMode() const;

  // What is actually active now: the running bridge's mode if one is running,
  // else the saved mode.
  Mode currentMode() const;

  bool jackAvailable() const { return m_jackAvailable; }

  // The persisted output-device choice as a stable token ("<cardId>:<pcmIndex>").
  // Empty means the ALSA "default" PCM (Internal). Only PulseToAlsa honours it.
  QString currentDevice() const;

  // Ensure the saved/default mode is active without needless restarts: if the
  // right bridge is already running (e.g. started at login) it is left alone.
  void ensureActive();

  // Headless login restore: force the saved mode to be (re)applied.
  void restoreSavedMode();

public slots:
  // User picked a routing option: apply it now and persist the choice.
  void setMode(Mode mode);

  // User picked an output device (stable token; empty = "default"): persist it
  // and restart the bridge so it takes effect live.
  void setDevice(const QString& token);

signals:
  void jackAvailabilityChanged(bool available);
  void modeChanged(Mode mode);
  void deviceChanged(const QString& token);

private slots:
  void probeJack();

private:
  void applyMode(Mode mode, bool force);
  void stopRunningBridge();
  qint64 startBridgeDetached(const QString& binary, const QString& alsaDevice);
  QString bridgePath(const QString& name) const;

  // Resolve the persisted card id to a "PULSE_BRIDGE_ALSA_DEV" string at start
  // time (so a card that returned at a new index still works). Empty = default.
  QString resolveAlsaDevice() const;

  // Write a baseline dmix/dsnoop ~/.asoundrc, but only if the user has no ALSA
  // config yet (~/.asoundrc and /etc/asound.conf both absent). Never overwrites.
  void ensureBaselineAsoundrc() const;

  // Persisted choices.
  void persistMode(Mode mode);
  void persistDevice(const QString& cardId);

  // Tell the running PA→ALSA bridge to switch output device in place (write the
  // control file + SIGUSR1), so client apps keep playing. False if it could not.
  bool liveSwitchDevice(qint64 pid) const;
  // Control file the bridge reads on SIGUSR1 (alongside the runtime pidfile).
  QString ctlFilePath() const;

  // Runtime state (pidfile) — survives a GUI restart, cleared on reboot.
  QString runStatePath() const;
  void writeRunState(Mode mode, qint64 pid);
  void clearRunState();
  bool readRunState(Mode* mode, qint64* pid) const;
  static bool pidAlive(qint64 pid);

  QTimer* m_jackTimer = nullptr;
  bool m_jackAvailable = false;
  void* m_jackLib = nullptr; // runtime-loaded libjack (null if absent)
};
