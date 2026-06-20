// BridgeManager.cpp
#include "BridgeManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#include <csignal>
#include <dlfcn.h>

namespace
{

// Minimal mirror of the libjack types/values we need to probe a running server,
// so we can dlopen libjack without pulling in <jack/jack.h> at build time.
using jack_client_t = void;
using JackOpenFn = jack_client_t* (*)(const char*, int /*options*/, int* /*status*/, ...);
using JackCloseFn = int (*)(jack_client_t*);

constexpr int kJackNoStartServer = 0x01; // JackNoStartServer
constexpr int kJackProbeIntervalMs = 2000;

constexpr const char* kSettingsKey = "routing/mode";

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

qint64 BridgeManager::startBridgeDetached(const QString& binary)
{
  qint64 pid = -1;
  // Detached: the bridge becomes independent and keeps running after the GUI
  // exits. Explicit argv, no shell.
  QProcess::startDetached(bridgePath(binary), QStringList{}, QCoreApplication::applicationDirPath(), &pid);
  return pid;
}

void BridgeManager::applyMode(Mode mode, bool force)
{
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
    case Mode::PulseToAlsa: writeRunState(mode, startBridgeDetached(QStringLiteral("pa-alsa-bridge"))); break;
    case Mode::PulseToJack: writeRunState(mode, startBridgeDetached(QStringLiteral("pulse-jack-bridge"))); break;
    case Mode::PureAlsa: writeRunState(mode, -1); break; // nothing runs
  }

  persistMode(mode);
  emit modeChanged(mode);
}

void BridgeManager::setMode(Mode mode)
{
  applyMode(mode, /*force=*/false);
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
