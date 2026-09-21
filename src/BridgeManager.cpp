// BridgeManager.cpp
#include "BridgeManager.h"

#include <csignal>
#include <cstdio>
#include <ctime>
#include <dlfcn.h>
#include <cerrno>

#include <alsa/asoundlib.h>

#include "AlsaDevices.h"
#include "config.h"
#include "platform/fs.h"
#include "platform/spawn.h"

using audiogui::fs::exeDir;
using audiogui::fs::runtimeDir;

namespace
{

// The channel count to fix the dmix slave at for a given hw device. dmix opens
// the card in one fixed config, so it must be one the hardware actually accepts:
// many pro USB interfaces (e.g. UMC204HD) expose playback as 4-channel only, and
// forcing stereo there misframes every sample into white noise. Prefer 2 when the
// device supports it (the common case), else fall back to its minimum. The plug
// wrapping "default" upmixes ordinary stereo apps to this count. Defaults to 2 if
// the device cannot be probed.
int dmixChannelsFor(const std::string &hwName)
{
    snd_pcm_t *pcm = nullptr;
    if (snd_pcm_open(&pcm, hwName.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) < 0)
        return 2;

    snd_pcm_hw_params_t *hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);

    unsigned int channels = 2;
    if (snd_pcm_hw_params_any(pcm, hw) >= 0) {
        if (snd_pcm_hw_params_test_channels(pcm, hw, 2) != 0) {
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
using JackOpenFn = jack_client_t *(*)(const char *, int /*options*/, int * /*status*/, ...);
using JackCloseFn = int (*)(jack_client_t *);

constexpr int kJackNoStartServer = 0x01; // JackNoStartServer

constexpr const char *kSettingsKey = "routing/mode";
constexpr const char *kDeviceKey = "routing/device";

// Swallow libjack's chatty stderr ("Cannot connect to server...", "JackShm...")
// that it prints on every failed probe when no jackd is running.
void jackSilent(const char *)
{
}

bool isValidMode(int v)
{
    return v == static_cast<int>(BridgeManager::Mode::PulseToAlsa) ||
           v == static_cast<int>(BridgeManager::Mode::PureAlsa) ||
           v == static_cast<int>(BridgeManager::Mode::PulseToJack);
}

// Replaces QThread::msleep in the SIGTERM wait loop.
void sleepMs(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

std::string joinPath(const std::string &dir, const std::string &name)
{
    if (dir.empty())
        return name;
    return dir.back() == '/' ? dir + name : dir + "/" + name;
}

} // namespace

BridgeManager::BridgeManager(bool loadJack)
{
    if (!loadJack)
        return;

    // Try to load libjack once; absence means JACK is simply never offered.
    m_jackLib = dlopen("libjack.so.0", RTLD_LAZY | RTLD_LOCAL);
    if (!m_jackLib)
        m_jackLib = dlopen("libjack.so", RTLD_LAZY | RTLD_LOCAL);

    // Redirect libjack's error/info messages to a no-op so a missing jackd does not
    // spam our terminal every probe interval.
    if (m_jackLib) {
        using SetMsgFn = void (*)(void (*)(const char *));
        if (auto setErr = reinterpret_cast<SetMsgFn>(dlsym(m_jackLib, "jack_set_error_function")))
            setErr(&jackSilent);
        if (auto setInfo = reinterpret_cast<SetMsgFn>(dlsym(m_jackLib, "jack_set_info_function")))
            setInfo(&jackSilent);
    }

    probeJack(); // seed initial availability
}

BridgeManager::~BridgeManager()
{
    // NOTE: we deliberately do NOT stop the bridge here — it must outlive the GUI.
    if (m_jackLib)
        dlclose(m_jackLib);
}

// ---- paths -----------------------------------------------------------------

std::string BridgeManager::bridgePath(const std::string &name) const
{
    // Resolve next to the GUI executable, not from $PATH.
    return joinPath(exeDir(), name);
}

std::string BridgeManager::ctlFilePath() const
{
    // Same per-user runtime dir as the pidfile; the bridge reads this on SIGUSR1.
    return joinPath(runtimeDir(), "audio-gui-bridge.dev");
}

std::string BridgeManager::runStatePath() const
{
    // Per-user runtime dir (XDG_RUNTIME_DIR); cleared on reboot, which is fine —
    // bridges do not survive a reboot, the persisted setting restores them.
    return joinPath(runtimeDir(), "audio-gui-bridge.state");
}

// ---- persisted choice ------------------------------------------------------

BridgeManager::Mode BridgeManager::savedMode() const
{
    const int v = audiogui::config::getInt(kSettingsKey, static_cast<int>(Mode::PulseToAlsa));
    return isValidMode(v) ? static_cast<Mode>(v) : Mode::PulseToAlsa;
}

void BridgeManager::persistMode(Mode mode)
{
    audiogui::config::setInt(kSettingsKey, static_cast<int>(mode));
}

std::string BridgeManager::currentDevice() const
{
    return audiogui::config::getString(kDeviceKey, std::string());
}

void BridgeManager::persistDevice(const std::string &cardId)
{
    audiogui::config::setString(kDeviceKey, cardId);
}

// ---- runtime pidfile -------------------------------------------------------

bool BridgeManager::pidAlive(long pid)
{
    if (pid <= 0)
        return false;
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
}

void BridgeManager::writeRunState(Mode mode, long pid)
{
    // Same one-line "<mode> <pid>\n" format the Qt build wrote, so a GUI of either
    // vintage can read a state file left by the other.
    const std::string body =
        std::to_string(static_cast<int>(mode)) + " " + std::to_string(pid) + "\n";
    audiogui::fs::writeFileAtomic(runStatePath(), body);
}

void BridgeManager::clearRunState()
{
    audiogui::fs::removeFile(runStatePath());
}

bool BridgeManager::readRunState(Mode *mode, long *pid) const
{
    std::string body;
    if (!audiogui::fs::readFile(runStatePath(), body))
        return false;
    int m = -1;
    long p = -1;
    if (sscanf(body.c_str(), "%d %ld", &m, &p) != 2)
        return false;
    if (!isValidMode(m))
        return false;
    *mode = static_cast<Mode>(m);
    *pid = p;
    return true;
}

BridgeManager::Mode BridgeManager::currentMode() const
{
    Mode m;
    long pid;
    if (readRunState(&m, &pid)) {
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
    long pid;
    if (readRunState(&m, &pid) && m != Mode::PureAlsa && pidAlive(pid)) {
        // SIGTERM: the bridge unlinks its PA socket and shm page on the way out.
        ::kill(static_cast<pid_t>(pid), SIGTERM);

        // Wait for it to actually exit before returning: the replacement bridge
        // reuses the same PA socket and shm name, so a still-shutting-down old
        // bridge would unlink the new bridge's socket/shm. Bounded; escalate to
        // SIGKILL if it ignores us.
        constexpr int kStepMs = 20;
        constexpr int kTimeoutMs = 3000;
        for (int waited = 0; waited < kTimeoutMs && pidAlive(pid); waited += kStepMs) {
            if (waited == kTimeoutMs / 2)
                ::kill(static_cast<pid_t>(pid), SIGKILL);
            sleepMs(kStepMs);
        }
    }
    clearRunState();
}

long BridgeManager::startBridgeDetached(const std::string &binary, const std::string &alsaDevice)
{
    // Detached: the bridge becomes independent and keeps running after the GUI
    // exits. Explicit program path, no shell, no arguments — everything the bridge
    // needs arrives through the environment. See platform/spawn.h for what
    // "detached" has to mean here and why it is a double fork.
    audiogui::EnvPairs env;
    if (!alsaDevice.empty())
        env.emplace_back("PULSE_BRIDGE_ALSA_DEV", alsaDevice);
    // Let the bridge accept live device switches (SIGUSR1 + this file).
    env.emplace_back("PULSE_BRIDGE_CTL_FILE", ctlFilePath());

    return audiogui::spawnDetached(bridgePath(binary), exeDir(), env);
}

std::string BridgeManager::resolveAlsaDevice() const
{
    // Map the persisted device token to a concrete ALSA device string now, so a
    // card that came back at a different index still resolves. Unknown/empty token
    // -> empty (the bridge falls back to "default").
    const std::string token = currentDevice();
    if (token.empty())
        return std::string();

    const std::vector<AlsaDevices::OutputDevice> devices = AlsaDevices::enumerateOutputs();
    const AlsaDevices::OutputDevice *d = AlsaDevices::findByToken(devices, token);
    if (!d)
        return std::string(); // device gone (unplugged): default keeps audio working
    return AlsaDevices::deviceStringFor(*d);
}

std::string BridgeManager::asoundrcContents(const std::string &playbackSlave,
                                            const std::string &captureSlave,
                                            const std::string &ctlCard) const
{
    // type plug "default" -> asym(playback dmix, capture dsnoop): software mixing so
    // several apps share the card, with plug adapting rate/format/channels. Rate is
    // a sane default — the bridge requests 44100 and adapts via set_rate_near. The
    // dmix channel count must match what the hardware accepts (see dmixChannelsFor).
    const int channels = dmixChannelsFor(playbackSlave);
    return std::string("# Written by Audio-Gui: software-mixing default output.\n"
                       "# Managed by Audio-Gui — rewritten when you pick a Pure-ALSA device.\n"
                       "# Delete this file to fall back to ALSA's built-in default.\n"
                       "pcm.!default {\n"
                       "  type plug\n"
                       "  slave.pcm \"audiogui_asym\"\n"
                       "}\n"
                       "ctl.!default {\n"
                       "  type hw\n"
                       "  card ") +
           ctlCard +
           "\n"
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
           "    pcm \"" +
           playbackSlave +
           "\"\n"
           "    rate 48000\n"
           "    channels " +
           std::to_string(channels) +
           "\n"
           "    period_size 512\n"
           "    buffer_size 4096\n"
           "  }\n"
           "}\n"
           "pcm.audiogui_dsnoop {\n"
           "  type dsnoop\n"
           "  ipc_key 1025\n"
           "  slave.pcm \"" +
           captureSlave +
           "\"\n"
           "}\n";
}

void BridgeManager::writeAsoundrc(const std::string &playbackSlave, const std::string &captureSlave,
                                  const std::string &ctlCard) const
{
    const std::string userRc = joinPath(audiogui::fs::homeDir(), ".asoundrc");

    // Only ever rewrite a file we wrote ourselves (or none at all): a hand-rolled
    // ~/.asoundrc (the user's or the distro's) is left untouched.
    if (audiogui::fs::exists(userRc)) {
        std::string existing;
        if (!audiogui::fs::readFile(userRc, existing))
            return;
        if (existing.find("Written by Audio-Gui") == std::string::npos)
            return;
    }

    // Atomic write; tolerate failure (audio still works if a usable default exists).
    audiogui::fs::writeFileAtomic(userRc, asoundrcContents(playbackSlave, captureSlave, ctlCard));
}

void BridgeManager::ensureBaselineAsoundrc() const
{
    // Only bootstrap when the user has no ALSA config at all — never clobber an
    // existing setup (theirs or the distro's). Slave the baseline dmix/dsnoop on
    // the internal card; fall back to hw:0 if we could not detect one.
    const std::string userRc = joinPath(audiogui::fs::homeDir(), ".asoundrc");
    if (audiogui::fs::exists(userRc) || audiogui::fs::exists("/etc/asound.conf"))
        return;

    const std::string internalId = AlsaDevices::firstInternalCardId();
    const std::string slave =
        internalId.empty() ? std::string("hw:0,0") : "hw:CARD=" + internalId + ",DEV=0";
    const std::string ctlCard = internalId.empty() ? std::string("0") : internalId;

    // File is known absent here, so write straight out (no managed-file guard).
    audiogui::fs::writeFileAtomic(userRc, asoundrcContents(slave, slave, ctlCard));
}

void BridgeManager::applyPureAlsaDefault() const
{
    // Pure ALSA runs no bridge: "selecting a device" means pointing ALSA's default
    // PCM at the chosen card so native ALSA apps started afterwards play through it.
    // Capture stays on the internal card so opening "default" to record still works
    // when the pick is a playback-only HDMI/USB output.
    const std::string internalId = AlsaDevices::firstInternalCardId();
    const std::string internalSlave =
        internalId.empty() ? std::string("hw:0,0") : "hw:CARD=" + internalId + ",DEV=0";

    std::string playbackSlave = internalSlave;
    std::string ctlCard = internalId.empty() ? std::string("0") : internalId;

    const std::string token = currentDevice();
    if (!token.empty()) {
        const std::vector<AlsaDevices::OutputDevice> devices = AlsaDevices::enumerateOutputs();
        if (const AlsaDevices::OutputDevice *d = AlsaDevices::findByToken(devices, token)) {
            if (d->category != AlsaDevices::Category::Internal && !d->cardId.empty()) {
                playbackSlave = "hw:CARD=" + d->cardId + ",DEV=" + std::to_string(d->pcmIndex);
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
    long pid;
    const bool haveRun = readRunState(&rm, &pid);
    const bool runningMatches = haveRun && rm == mode && (mode == Mode::PureAlsa || pidAlive(pid));
    if (!force && runningMatches) {
        persistMode(mode);
        return;
    }

    stopRunningBridge();

    switch (mode) {
        case Mode::PulseToAlsa:
            // "default" already ensured above; point the bridge at the chosen device.
            writeRunState(mode, startBridgeDetached("pa-alsa-bridge", resolveAlsaDevice()));
            break;
        case Mode::PulseToJack:
            // JACK bridge routes through jackd, not an ALSA device — no device env.
            writeRunState(mode, startBridgeDetached("pulse-jack-bridge", std::string()));
            break;
        case Mode::PureAlsa:
            // "default" already pointed at the chosen device above; no bridge runs.
            writeRunState(mode, -1);
            break;
    }

    persistMode(mode);
    if (onModeChanged)
        onModeChanged(mode);
}

void BridgeManager::setMode(Mode mode)
{
    applyMode(mode, /*force=*/false);
}

bool BridgeManager::liveSwitchDevice(long pid) const
{
    // The bridge opens "default" when the control file holds an empty/"default"
    // string, so resolve "" to "default" here.
    std::string dev = resolveAlsaDevice();
    if (dev.empty())
        dev = "default";

    // Atomic: the bridge reads this file from its SIGUSR1 path, and the signal goes
    // out on the very next line, so a half-written file would be read exactly when
    // it is torn.
    if (!audiogui::fs::writeFileAtomic(ctlFilePath(), dev + "\n"))
        return false;

    return ::kill(static_cast<pid_t>(pid), SIGUSR1) == 0;
}

void BridgeManager::setDevice(const std::string &token)
{
    if (token == currentDevice())
        return;
    persistDevice(token);

    // If the PA→ALSA bridge is already running, switch its output device IN PLACE
    // (SIGUSR1) so connected apps keep playing — no restart, no dropped streams.
    Mode rm;
    long pid;
    if (readRunState(&rm, &pid) && rm == Mode::PulseToAlsa && pidAlive(pid) &&
        liveSwitchDevice(pid)) {
        if (onDeviceChanged)
            onDeviceChanged(token);
        return;
    }

    // Nothing running to signal (or the live switch failed): (re)apply so the
    // choice takes effect when the bridge next starts.
    applyMode(currentMode(), /*force=*/true);
    if (onDeviceChanged)
        onDeviceChanged(token);
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

    if (m_jackLib) {
        auto open = reinterpret_cast<JackOpenFn>(dlsym(m_jackLib, "jack_client_open"));
        auto close = reinterpret_cast<JackCloseFn>(dlsym(m_jackLib, "jack_client_close"));
        if (open && close) {
            int status = 0;
            // Connect without starting a server: success means a server is running.
            jack_client_t *c = open("audio-gui-probe", kJackNoStartServer, &status);
            if (c) {
                close(c);
                available = true;
            }
        }
    }

    if (available != m_jackAvailable) {
        m_jackAvailable = available;
        if (onJackAvailabilityChanged)
            onJackAvailabilityChanged(available);
    }
}
