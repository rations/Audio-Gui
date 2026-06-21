// BridgeManager.cpp
#include "BridgeManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#include <csignal>
#include <dlfcn.h>

#include <alsa/asoundlib.h>

#include "AlsaDevices.h"

namespace
{

// The channel count to fix the dmix slave at for a given hw device. dmix opens
// the card in one fixed config, so it must be one the hardware actually accepts:
// many pro USB interfaces (e.g. UMC204HD) expose playback as 4-channel only, and
// forcing stereo there misframes every sample into white noise. Prefer 2 when the
// device supports it (the common case), else fall back to its minimum. The plug
// wrapping "default" upmixes ordinary stereo apps to this count. Defaults to 2 if
// the device cannot be probed.
int dmixChannelsFor(const QString& hwName)
{
  snd_pcm_t* pcm = nullptr;
  if (snd_pcm_open(&pcm, hwName.toUtf8().constData(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) < 0)
    return 2;

  snd_pcm_hw_params_t* hw = nullptr;
  snd_pcm_hw_params_alloca(&hw);

  unsigned int channels = 2;
  if (snd_pcm_hw_params_any(pcm, hw) >= 0)
  {
    if (snd_pcm_hw_params_test_channels(pcm, hw, 2) != 0)
    {
      unsigned int minc = 2;
      if (snd_pcm_hw_params_get_channels_min(hw, &minc) >= 0 && minc > 0)
        channels = minc;
    }
  }
  snd_pcm_close(pcm);
  return static_cast<int>(channels);
}

// Minimal mirror of the libjack types/values we need to probe a running server,
// so we can dlopen libjack without pulling in <jack/jack.h> at build time.
using jack_client_t = void;
using JackOpenFn = jack_client_t* (*)(const char*, int /*options*/, int* /*status*/, ...);
using JackCloseFn = int (*)(jack_client_t*);

constexpr int kJackNoStartServer = 0x01; // JackNoStartServer
constexpr int kJackProbeIntervalMs = 2000;

constexpr const char* kSettingsKey = "routing/mode";
constexpr const char* kDeviceKey = "routing/device";

// Swallow libjack's chatty stderr ("Cannot connect to server...", "JackShm...")
// that it prints on every failed probe when no jackd is running.
void jackSilent(const char*) {}

bool isValidMode(int v)
{
  return v == static_cast<int>(BridgeManager::Mode::PulseToAlsa) || v == static_cast<int>(BridgeManager::Mode::PureAlsa)
         || v == static_cast<int>(BridgeManager::Mode::PulseToJack);
}

} // namespace

BridgeManager::BridgeManager(bool enableJackProbe, QObject* parent)
: QObject(parent)
{
  // Try to load libjack once; absence means JACK is simply never offered.
  m_jackLib = dlopen("libjack.so.0", RTLD_LAZY | RTLD_LOCAL);
  if (!m_jackLib)
    m_jackLib = dlopen("libjack.so", RTLD_LAZY | RTLD_LOCAL);

  // Redirect libjack's error/info messages to a no-op so a missing jackd does not
  // spam our terminal every probe interval.
  if (m_jackLib)
  {
    using SetMsgFn = void (*)(void (*)(const char*));
    if (auto setErr = reinterpret_cast<SetMsgFn>(dlsym(m_jackLib, "jack_set_error_function")))
      setErr(&jackSilent);
    if (auto setInfo = reinterpret_cast<SetMsgFn>(dlsym(m_jackLib, "jack_set_info_function")))
      setInfo(&jackSilent);
  }

  if (enableJackProbe)
  {
    m_jackTimer = new QTimer(this);
    m_jackTimer->setInterval(kJackProbeIntervalMs);
    connect(m_jackTimer, &QTimer::timeout, this, &BridgeManager::probeJack);
    m_jackTimer->start();
    probeJack(); // seed initial availability
  }
}

BridgeManager::~BridgeManager()
{
  // NOTE: we deliberately do NOT stop the bridge here — it must outlive the GUI.
  if (m_jackLib)
    dlclose(m_jackLib);
}

// ---- paths -----------------------------------------------------------------

QString BridgeManager::bridgePath(const QString& name) const
{
  // Resolve next to the GUI executable, not from $PATH.
  return QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(name);
}

QString BridgeManager::ctlFilePath() const
{
  // Same per-user runtime dir as the pidfile; the bridge reads this on SIGUSR1.
  QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
  if (dir.isEmpty())
    dir = QDir::tempPath();
  return QDir(dir).absoluteFilePath(QStringLiteral("audio-gui-bridge.dev"));
}

QString BridgeManager::runStatePath() const
{
  // Per-user runtime dir (XDG_RUNTIME_DIR); cleared on reboot, which is fine —
  // bridges do not survive a reboot, the persisted setting restores them.
  QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
  if (dir.isEmpty())
    dir = QDir::tempPath();
  return QDir(dir).absoluteFilePath(QStringLiteral("audio-gui-bridge.state"));
}

// ---- persisted choice ------------------------------------------------------

BridgeManager::Mode BridgeManager::savedMode() const
{
  QSettings s;
  int v = s.value(QString::fromLatin1(kSettingsKey), static_cast<int>(Mode::PulseToAlsa)).toInt();
  return isValidMode(v) ? static_cast<Mode>(v) : Mode::PulseToAlsa;
}

void BridgeManager::persistMode(Mode mode)
{
  QSettings s;
  s.setValue(QString::fromLatin1(kSettingsKey), static_cast<int>(mode));
}

QString BridgeManager::currentDevice() const
{
  QSettings s;
  return s.value(QString::fromLatin1(kDeviceKey)).toString();
}

void BridgeManager::persistDevice(const QString& cardId)
{
  QSettings s;
  s.setValue(QString::fromLatin1(kDeviceKey), cardId);
}

// ---- runtime pidfile -------------------------------------------------------

bool BridgeManager::pidAlive(qint64 pid)
{
  if (pid <= 0)
    return false;
  return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
}

void BridgeManager::writeRunState(Mode mode, qint64 pid)
{
  QFile f(runStatePath());
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return;
  QTextStream(&f) << static_cast<int>(mode) << ' ' << pid << '\n';
}

void BridgeManager::clearRunState()
{
  QFile::remove(runStatePath());
}

bool BridgeManager::readRunState(Mode* mode, qint64* pid) const
{
  QFile f(runStatePath());
  if (!f.open(QIODevice::ReadOnly))
    return false;
  int m = -1;
  qint64 p = -1;
  QTextStream(&f) >> m >> p;
  if (!isValidMode(m))
    return false;
  *mode = static_cast<Mode>(m);
  *pid = p;
  return true;
}

BridgeManager::Mode BridgeManager::currentMode() const
{
  Mode m;
  qint64 pid;
  if (readRunState(&m, &pid))
  {
    // PureAlsa has no process; a process-backed mode counts only if it's alive.
    if (m == Mode::PureAlsa || pidAlive(pid))
      return m;
  }
  return savedMode();
}

// ---- process lifecycle -----------------------------------------------------

void BridgeManager::stopRunningBridge()
{
  Mode m;
  qint64 pid;
  if (readRunState(&m, &pid) && m != Mode::PureAlsa && pidAlive(pid))
  {
    // SIGTERM: the bridge unlinks its PA socket and shm page on the way out.
    ::kill(static_cast<pid_t>(pid), SIGTERM);

    // Wait for it to actually exit before returning: the replacement bridge
    // reuses the same PA socket and shm name, so a still-shutting-down old
    // bridge would unlink the new bridge's socket/shm. Bounded; escalate to
    // SIGKILL if it ignores us.
    constexpr int kStepMs = 20;
    constexpr int kTimeoutMs = 3000;
    for (int waited = 0; waited < kTimeoutMs && pidAlive(pid); waited += kStepMs)
    {
      if (waited == kTimeoutMs / 2)
        ::kill(static_cast<pid_t>(pid), SIGKILL);
      QThread::msleep(kStepMs);
    }
  }
  clearRunState();
}

qint64 BridgeManager::startBridgeDetached(const QString& binary, const QString& alsaDevice)
{
  // Detached: the bridge becomes independent and keeps running after the GUI
  // exits. Explicit program/argv, no shell. A QProcess instance (not the static
  // overload) lets us inject PULSE_BRIDGE_ALSA_DEV so the bridge opens the chosen
  // output device.
  QProcess p;
  p.setProgram(bridgePath(binary));
  p.setWorkingDirectory(QCoreApplication::applicationDirPath());

  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  if (!alsaDevice.isEmpty())
    env.insert(QStringLiteral("PULSE_BRIDGE_ALSA_DEV"), alsaDevice);
  // Let the bridge accept live device switches (SIGUSR1 + this file).
  env.insert(QStringLiteral("PULSE_BRIDGE_CTL_FILE"), ctlFilePath());
  p.setProcessEnvironment(env);

  qint64 pid = -1;
  p.startDetached(&pid);
  return pid;
}

QString BridgeManager::resolveAlsaDevice() const
{
  // Map the persisted device token to a concrete ALSA device string now, so a
  // card that came back at a different index still resolves. Unknown/empty token
  // -> empty (the bridge falls back to "default").
  const QString token = currentDevice();
  if (token.isEmpty())
    return QString();

  const QVector<AlsaDevices::OutputDevice> devices = AlsaDevices::enumerateOutputs();
  const AlsaDevices::OutputDevice* d = AlsaDevices::findByToken(devices, token);
  if (!d)
    return QString(); // device gone (unplugged): default keeps audio working
  return AlsaDevices::deviceStringFor(*d);
}

QString BridgeManager::asoundrcContents(const QString& playbackSlave, const QString& captureSlave,
                                        const QString& ctlCard) const
{
  // type plug "default" -> asym(playback dmix, capture dsnoop): software mixing so
  // several apps share the card, with plug adapting rate/format/channels. Rate is
  // a sane default — the bridge requests 44100 and adapts via set_rate_near. The
  // dmix channel count must match what the hardware accepts (see dmixChannelsFor).
  const int channels = dmixChannelsFor(playbackSlave);
  return QStringLiteral(
           "# Written by Audio-Gui: software-mixing default output.\n"
           "# Managed by Audio-Gui — rewritten when you pick a Pure-ALSA device.\n"
           "# Delete this file to fall back to ALSA's built-in default.\n"
           "pcm.!default {\n"
           "  type plug\n"
           "  slave.pcm \"audiogui_asym\"\n"
           "}\n"
           "ctl.!default {\n"
           "  type hw\n"
           "  card %1\n"
           "}\n"
           "pcm.audiogui_asym {\n"
           "  type asym\n"
           "  playback.pcm \"audiogui_dmix\"\n"
           "  capture.pcm \"audiogui_dsnoop\"\n"
           "}\n"
           "pcm.audiogui_dmix {\n"
           "  type dmix\n"
           "  ipc_key 1024\n"
           "  slave {\n"
           "    pcm \"%2\"\n"
           "    rate 48000\n"
           "    channels %4\n"
           "  }\n"
           "}\n"
           "pcm.audiogui_dsnoop {\n"
           "  type dsnoop\n"
           "  ipc_key 1025\n"
           "  slave.pcm \"%3\"\n"
           "}\n")
    .arg(ctlCard, playbackSlave, captureSlave)
    .arg(channels);
}

void BridgeManager::writeAsoundrc(const QString& playbackSlave, const QString& captureSlave,
                                  const QString& ctlCard) const
{
  const QString userRc = QDir(QDir::homePath()).absoluteFilePath(QStringLiteral(".asoundrc"));

  // Only ever rewrite a file we wrote ourselves (or none at all): a hand-rolled
  // ~/.asoundrc (the user's or the distro's) is left untouched.
  if (QFileInfo::exists(userRc))
  {
    QFile r(userRc);
    if (!r.open(QIODevice::ReadOnly))
      return;
    const QString existing = QString::fromUtf8(r.readAll());
    r.close();
    if (!existing.contains(QStringLiteral("Written by Audio-Gui")))
      return;
  }

  // Atomic write; tolerate failure (audio still works if a usable default exists).
  QSaveFile f(userRc);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return;
  f.write(asoundrcContents(playbackSlave, captureSlave, ctlCard).toUtf8());
  f.commit();
}

void BridgeManager::ensureBaselineAsoundrc() const
{
  // Only bootstrap when the user has no ALSA config at all — never clobber an
  // existing setup (theirs or the distro's). Slave the baseline dmix/dsnoop on
  // the internal card; fall back to hw:0 if we could not detect one.
  const QString userRc = QDir(QDir::homePath()).absoluteFilePath(QStringLiteral(".asoundrc"));
  if (QFileInfo::exists(userRc) || QFileInfo::exists(QStringLiteral("/etc/asound.conf")))
    return;

  const QString internalId = AlsaDevices::firstInternalCardId();
  const QString slave =
    internalId.isEmpty() ? QStringLiteral("hw:0,0") : QStringLiteral("hw:CARD=%1,DEV=0").arg(internalId);
  const QString ctlCard = internalId.isEmpty() ? QStringLiteral("0") : internalId;

  // File is known absent here, so write straight out (no managed-file guard).
  QSaveFile f(userRc);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return;
  f.write(asoundrcContents(slave, slave, ctlCard).toUtf8());
  f.commit();
}

void BridgeManager::applyPureAlsaDefault() const
{
  // Pure ALSA runs no bridge: "selecting a device" means pointing ALSA's default
  // PCM at the chosen card so native ALSA apps started afterwards play through it.
  // Capture stays on the internal card so opening "default" to record still works
  // when the pick is a playback-only HDMI/USB output.
  const QString internalId = AlsaDevices::firstInternalCardId();
  const QString internalSlave =
    internalId.isEmpty() ? QStringLiteral("hw:0,0") : QStringLiteral("hw:CARD=%1,DEV=0").arg(internalId);

  QString playbackSlave = internalSlave;
  QString ctlCard = internalId.isEmpty() ? QStringLiteral("0") : internalId;

  const QString token = currentDevice();
  if (!token.isEmpty())
  {
    const QVector<AlsaDevices::OutputDevice> devices = AlsaDevices::enumerateOutputs();
    if (const AlsaDevices::OutputDevice* d = AlsaDevices::findByToken(devices, token))
    {
      if (d->category != AlsaDevices::Category::Internal && !d->cardId.isEmpty())
      {
        playbackSlave = QStringLiteral("hw:CARD=%1,DEV=%2").arg(d->cardId).arg(d->pcmIndex);
        ctlCard = d->cardId;
      }
    }
  }

  writeAsoundrc(playbackSlave, internalSlave, ctlCard);
}

void BridgeManager::applyMode(Mode mode, bool force)
{
  // Guarantee a usable ALSA "default" exists on every entry — before the no-op
  // return below. A first run, or a user who deleted ~/.asoundrc, must never be
  // left with a broken "default" (silent or erroring apps with no clue why).
  // Both writers are idempotent and never clobber a hand-rolled config. JACK
  // routes through jackd, so it keeps the prior behaviour of not bootstrapping a
  // dmix default.
  if (mode == Mode::PulseToAlsa)
    ensureBaselineAsoundrc();
  else if (mode == Mode::PureAlsa)
    applyPureAlsaDefault();

  // No-op only if the requested mode is what is *actually* running right now
  // (a bridge started at login, say) — never based on the saved-mode fallback,
  // or a fresh launch with nothing running would skip starting the bridge.
  Mode rm;
  qint64 pid;
  const bool haveRun = readRunState(&rm, &pid);
  const bool runningMatches = haveRun && rm == mode && (mode == Mode::PureAlsa || pidAlive(pid));
  if (!force && runningMatches)
  {
    persistMode(mode);
    return;
  }

  stopRunningBridge();

  switch (mode)
  {
    case Mode::PulseToAlsa:
      // "default" already ensured above; point the bridge at the chosen device.
      writeRunState(mode, startBridgeDetached(QStringLiteral("pa-alsa-bridge"), resolveAlsaDevice()));
      break;
    case Mode::PulseToJack:
      // JACK bridge routes through jackd, not an ALSA device — no device env.
      writeRunState(mode, startBridgeDetached(QStringLiteral("pulse-jack-bridge"), QString()));
      break;
    case Mode::PureAlsa:
      // "default" already pointed at the chosen device above; no bridge runs.
      writeRunState(mode, -1);
      break;
  }

  persistMode(mode);
  emit modeChanged(mode);
}

void BridgeManager::setMode(Mode mode)
{
  applyMode(mode, /*force=*/false);
}

bool BridgeManager::liveSwitchDevice(qint64 pid) const
{
  // The bridge opens "default" when the control file holds an empty/"default"
  // string, so resolve "" to "default" here.
  QString dev = resolveAlsaDevice();
  if (dev.isEmpty())
    dev = QStringLiteral("default");

  QSaveFile f(ctlFilePath());
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return false;
  f.write(dev.toUtf8());
  f.write("\n");
  if (!f.commit())
    return false;

  return ::kill(static_cast<pid_t>(pid), SIGUSR1) == 0;
}

void BridgeManager::setDevice(const QString& token)
{
  if (token == currentDevice())
    return;
  persistDevice(token);

  // If the PA→ALSA bridge is already running, switch its output device IN PLACE
  // (SIGUSR1) so connected apps keep playing — no restart, no dropped streams.
  Mode rm;
  qint64 pid;
  if (readRunState(&rm, &pid) && rm == Mode::PulseToAlsa && pidAlive(pid) && liveSwitchDevice(pid))
  {
    emit deviceChanged(token);
    return;
  }

  // Nothing running to signal (or the live switch failed): (re)apply so the
  // choice takes effect when the bridge next starts.
  applyMode(currentMode(), /*force=*/true);
  emit deviceChanged(token);
}

void BridgeManager::ensureActive()
{
  // GUI launch: bring the saved/default mode up only if it isn't already.
  applyMode(savedMode(), /*force=*/false);
}

void BridgeManager::restoreSavedMode()
{
  // Login restore: the runtime dir was cleared by the reboot, so force a clean
  // (re)start of the saved mode.
  applyMode(savedMode(), /*force=*/true);
}

// ---- jackd detection -------------------------------------------------------

void BridgeManager::probeJack()
{
  bool available = false;

  if (m_jackLib)
  {
    auto open = reinterpret_cast<JackOpenFn>(dlsym(m_jackLib, "jack_client_open"));
    auto close = reinterpret_cast<JackCloseFn>(dlsym(m_jackLib, "jack_client_close"));
    if (open && close)
    {
      int status = 0;
      // Connect without starting a server: success means a server is running.
      jack_client_t* c = open("audio-gui-probe", kJackNoStartServer, &status);
      if (c)
      {
        close(c);
        available = true;
      }
    }
  }

  if (available != m_jackAvailable)
  {
    m_jackAvailable = available;
    emit jackAvailabilityChanged(available);
  }
}
