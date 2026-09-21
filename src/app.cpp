// See app.h.

#include "app.h"

#include "config.h"
#include "geometry.h"
#include "platform/fs.h"

#include <algorithm>

namespace audiogui
{
namespace
{

constexpr const char *kModeKey = "appearance/mode";
constexpr const char *kAccentKey = "appearance/accent";

Routing toRouting(BridgeManager::Mode m)
{
    return static_cast<Routing>(static_cast<int>(m));
}
BridgeManager::Mode toBridgeMode(Routing r)
{
    return static_cast<BridgeManager::Mode>(static_cast<int>(r));
}

} // namespace

// The GUI is started at login by an autostart entry rather than a service, so a machine that
// never opens the window still restores the saved routing after a reboot — using a plain
// .desktop file, no systemd.
void ensureAutostartEntry()
{
    const std::string dir = fs::configHome() + "/autostart";
    if (!fs::makeDirs(dir))
        return;
    const std::string path = dir + "/audio-gui-restore.desktop";

    const std::string contents =
        std::string("[Desktop Entry]\n"
                    "Type=Application\n"
                    "Name=Audio routing (restore)\n"
                    "Comment=Restore the saved PulseAudio-compat audio routing\n"
                    "Exec=\"") +
        fs::exePath() +
        "\" --restore\n"
        "Terminal=false\n"
        "NoDisplay=true\n"
        "X-GNOME-Autostart-enabled=true\n";

    // Only rewrite if missing or stale (e.g. the binary moved), to avoid churn.
    std::string existing;
    if (fs::readFile(path, existing) && existing == contents)
        return;
    fs::writeFileAtomic(path, contents);
}

App::App() : mBridges(/*loadJack=*/true)
{
    mPanel.setMode(static_cast<Mode>(
        std::clamp(config::getInt(kModeKey, static_cast<int>(Mode::Dark)), 0, kModeCount - 1)));
    mPanel.setAccent(static_cast<Accent>(std::clamp(
        config::getInt(kAccentKey, static_cast<int>(Accent::Green)), 0, kAccentCount - 1)));

    mPanel.setPlaceholder("No mixer controls for this output.");
    mPanel.onNeedsRepaint = [this] { markDirty(); };

    mMixer.onChanged = [this] { syncFromMixer(); };
    mMixer.onDescriptorsChanged = [this] {
        if (onMixerFdsChanged)
            onMixerFdsChanged();
    };

    mBridges.onJackAvailabilityChanged = [this](bool available) {
        mPanel.setJackAvailable(available);
        markDirty();
    };
    mBridges.onModeChanged = [this](BridgeManager::Mode) {
        updateDeviceComboEnabled();
        updateMeterVisibility();
        relayout();
        markDirty();
    };

    // --- panel events -> actions ---
    mPanel.cb.volume = [this](int strip, int percent) {
        if (const AlsaMixer::Element *e = elementForStrip(strip))
            mMixer.setVolume(*e, percent);
        markDirty();
    };
    mPanel.cb.mute = [this](int strip, bool on) {
        if (const AlsaMixer::Element *e = elementForStrip(strip))
            mMixer.setSwitchOn(*e, on);
        markDirty();
    };
    mPanel.cb.switchToggled = [this](int index, bool on) {
        if (const AlsaMixer::Element *e = elementForSwitch(index))
            mMixer.setSwitchOn(*e, on);
        markDirty();
    };
    mPanel.cb.routing = [this](Routing r) {
        // setMode can block for up to three seconds waiting for the outgoing bridge to exit, so
        // the new radio state is put on screen before that starts rather than after.
        if (paintNow)
            paintNow();
        mBridges.setMode(toBridgeMode(r));
        updateDeviceComboEnabled();
        updateMeterVisibility();
        relayout();
        markDirty();
    };
    mPanel.cb.device = [this](int index) {
        if (index < 0 || index >= static_cast<int>(mDevices.size()))
            return;
        const std::string token = AlsaDevices::tokenFor(mDevices[static_cast<size_t>(index)]);
        mBridges.setDevice(token);   // live-switches the running bridge, or restarts it
        reopenMixerForDevice(token); // the mixer follows the device
        relayout();
        markDirty();
    };
    mPanel.cb.accent = [this](int index) {
        mPanel.setAccent(static_cast<Accent>(std::clamp(index, 0, kAccentCount - 1)));
        applyAppearance();
    };
    mPanel.cb.toggleTheme = [this] {
        mPanel.setMode(mPanel.mode() == Mode::Dark ? Mode::Light : Mode::Dark);
        applyAppearance();
    };
}

void App::applyAppearance()
{
    config::setInt(kModeKey, static_cast<int>(mPanel.mode()));
    config::setInt(kAccentKey, static_cast<int>(mPanel.accent()));
    markDirty();
}

bool App::takeDirty()
{
    const bool d = mDirty;
    mDirty = false;
    return d;
}

void App::relayout()
{
    const float h = mPanel.layout();
    if (h != mLastHeight) {
        mLastHeight = h;
        if (requestResize)
            requestResize(h);
    }
}

void App::start()
{
    // Routing first, then the mixer: applying a mode can rewrite ~/.asoundrc, and the mixer
    // should open the result rather than whatever was there before. Same order as the Qt build.
    mBridges.ensureActive();

    mMixerOpen = mMixer.open("default");
    if (!mMixerOpen) {
        mPanel.setFatalError("Could not open the ALSA mixer (\"default\").\n"
                             "Check that a sound card is present and alsa-utils is installed.");
        relayout();
        return;
    }

    refreshDevices(/*initialBuild=*/true);
    ensureAutostartEntry();
    mPanel.setJackAvailable(mBridges.jackAvailable());
    updateDeviceComboEnabled();
    updateMeterVisibility();
    relayout();
}

// --- elements ---------------------------------------------------------------

const AlsaMixer::Element *App::elementForStrip(int strip) const
{
    const std::vector<Panel::Strip> &strips = mPanel.strips();
    if (strip < 0 || strip >= static_cast<int>(strips.size()))
        return nullptr;
    const int idx = strips[static_cast<size_t>(strip)].element;
    if (idx < 0 || idx >= static_cast<int>(mMixer.elements().size()))
        return nullptr;
    return &mMixer.elements()[static_cast<size_t>(idx)];
}

const AlsaMixer::Element *App::elementForSwitch(int index) const
{
    if (index < 0 || index >= static_cast<int>(mSwitchElements.size()))
        return nullptr;
    const int idx = mSwitchElements[static_cast<size_t>(index)];
    if (idx < 0 || idx >= static_cast<int>(mMixer.elements().size()))
        return nullptr;
    return &mMixer.elements()[static_cast<size_t>(idx)];
}

// --- mixer ------------------------------------------------------------------

void App::populateMixerControls(bool suppressControls)
{
    std::vector<Panel::Strip> strips;
    std::vector<std::string> switchLabels;
    std::vector<bool> switchStates;
    mSwitchElements.clear();

    // USB interfaces (and any other suppressed card) show only the placeholder:
    // their software controls don't drive the hardware, so a slider would mislead.
    if (!suppressControls) {
        const std::vector<AlsaMixer::Element> &elems = mMixer.elements();

        // One strip per element that exposes a volume (Master, Headphone, Speaker, ...).
        for (size_t i = 0; i < elems.size(); ++i) {
            const AlsaMixer::Element &e = elems[i];
            if (!e.hasVolume)
                continue;
            Panel::Strip s;
            s.label = e.label;
            s.element = static_cast<int>(i);
            s.hasVolume = true;
            s.hasSwitch = e.hasSwitch;
            s.slider.value = mMixer.volume(e);
            s.button.on = mMixer.switchOn(e);

            const bool capture = e.kind == AlsaMixer::Kind::CaptureVolume ||
                                 e.kind == AlsaMixer::Kind::CaptureSwitch;
            s.button.iconOn = capture ? Icon::CaptureOn : Icon::SpeakerOn;
            s.button.iconOff = capture ? Icon::CaptureOff : Icon::SpeakerMuted;
            strips.push_back(std::move(s));
        }

        // Switch-only controls (Capture, IEC958) for this card.
        for (size_t i = 0; i < elems.size(); ++i) {
            const AlsaMixer::Element &e = elems[i];
            if (e.hasVolume || !e.hasSwitch)
                continue;
            switchLabels.push_back(e.label);
            switchStates.push_back(mMixer.switchOn(e));
            mSwitchElements.push_back(static_cast<int>(i));
        }
    }

    mPanel.setStrips(std::move(strips));
    mPanel.setSwitches(std::move(switchLabels), std::move(switchStates));
}

void App::reopenMixerForDevice(const std::string &token)
{
    // Internal / empty -> the ALSA "default" mixer; a specific card -> its controls.
    const std::string cardId = AlsaDevices::cardIdFromToken(token);
    const std::string card = cardId.empty() ? std::string("default") : "hw:CARD=" + cardId;

    // USB and HDMI outputs have no useful software mixer controls: USB gain is set
    // by hardware knobs; HDMI is a digital path with no per-channel volume stage.
    // On sof-hda-dsp, HDMI shares a card with internal speakers, so without this
    // suppression the internal-speaker controls would appear for the HDMI selection.
    const std::vector<AlsaDevices::OutputDevice> devices = AlsaDevices::enumerateOutputs();
    const AlsaDevices::OutputDevice *d = AlsaDevices::findByToken(devices, token);
    const bool suppressControls = d && (d->category == AlsaDevices::Category::Usb ||
                                        d->category == AlsaDevices::Category::Hdmi);

    // On failure the element list is empty, so the placeholder shows.
    mMixerOpen = mMixer.reopen(card);
    populateMixerControls(suppressControls);
}

void App::syncFromMixer()
{
    // External change (amixer, another app, a media key): re-sync every control.
    //
    // A slider being dragged is left alone. The pointer owns that value until the button comes
    // up, and writing the hardware's reading back into it mid-drag makes the thumb fight the
    // cursor -- the mixer is reporting the value this drag just set, one tick late.
    const bool dragging = mPanel.dragging();

    std::vector<Panel::Strip> &strips = mPanel.strips();
    for (size_t i = 0; i < strips.size(); ++i) {
        const AlsaMixer::Element *e = elementForStrip(static_cast<int>(i));
        if (!e)
            continue;
        if (!dragging && strips[i].hasVolume)
            strips[i].slider.value = mMixer.volume(*e);
        if (strips[i].hasSwitch)
            strips[i].button.on = mMixer.switchOn(*e);
    }
    markDirty();
}

void App::mixerEvent()
{
    mMixer.handleEvents(); // drains ALSA's queue, then calls syncFromMixer through onChanged
}

// --- devices ----------------------------------------------------------------

void App::refreshDevices(bool initialBuild)
{
    const std::vector<AlsaDevices::OutputDevice> devices = AlsaDevices::enumerateOutputs();

    // Cheap change detection: only rebuild when the set of devices actually changed. Without it
    // this rebuilds the combo every three seconds, which would also close it while it is open.
    // Never rebuild underneath an open popup: the list would be replaced while the user is
    // reading it, and the selection they were about to make would land on a different row.
    if (!initialBuild && mPanel.deviceComboOpen())
        return;

    auto signature = [](const std::vector<AlsaDevices::OutputDevice> &v) {
        std::string s;
        for (const AlsaDevices::OutputDevice &d : v)
            s += d.cardId + ":" + std::to_string(d.pcmIndex) + "|";
        return s;
    };
    if (!initialBuild && signature(devices) == signature(mDevices))
        return;
    mDevices = devices;

    // Preserve the user's selection across the rebuild; on first fill use the
    // persisted choice. Internal devices are stored as "" (== ALSA "default").
    const std::string want = initialBuild ? mBridges.currentDevice() : mPanel.deviceValue();

    std::vector<ComboItem> items;
    int wantIndex = -1;
    int internalIndex = -1;
    for (size_t i = 0; i < devices.size(); ++i) {
        const AlsaDevices::OutputDevice &d = devices[i];
        const std::string token = AlsaDevices::tokenFor(d);
        ComboItem it;
        it.label = d.displayName;
        it.value = token;
        items.push_back(std::move(it));
        if (token == want && wantIndex < 0)
            wantIndex = static_cast<int>(i);
        if (internalIndex < 0 && d.category == AlsaDevices::Category::Internal)
            internalIndex = static_cast<int>(i);
    }

    const int select = wantIndex >= 0 ? wantIndex : (internalIndex >= 0 ? internalIndex : 0);
    mPanel.setDevices(std::move(items), devices.empty() ? -1 : select);

    if (initialBuild) {
        // The saved preference is left intact even if the device is absent right now: the bridge
        // resolves a missing device to "default" on its own, so a device merely unplugged at
        // startup is not forgotten.
        reopenMixerForDevice(want);
    } else if (!devices.empty()) {
        // A live refresh: if the device that was selected vanished (unplug), switch the running
        // bridge and the mixer to the new selection so audio keeps playing.
        const std::string chosen = select >= 0 && select < static_cast<int>(devices.size())
                                       ? AlsaDevices::tokenFor(devices[static_cast<size_t>(select)])
                                       : std::string();
        if (chosen != mBridges.currentDevice()) {
            mBridges.setDevice(chosen);
            reopenMixerForDevice(chosen);
        }
    }

    relayout();
    markDirty();
}

void App::updateDeviceComboEnabled()
{
    // PA→ALSA switches live; Pure ALSA repoints ALSA's default (applies on app
    // restart). Only PA→JACK ignores the device (audio routes through jackd).
    const BridgeManager::Mode mode = mBridges.currentMode();
    mPanel.setDeviceComboEnabled(mode != BridgeManager::Mode::PulseToJack, std::string());
    mPanel.setRouting(toRouting(mode));
}

void App::updateMeterVisibility()
{
    // Only the pulse bridges publish peak data into the shared page the meter
    // reads; pure ALSA has no writer, so hide the meter rather than show a dead one.
    const bool bridged = mBridges.currentMode() != BridgeManager::Mode::PureAlsa;
    mPanel.setMeterVisible(bridged);
    if (!bridged)
        mPeaks.close();
}

// --- the clocks -------------------------------------------------------------

void App::tickMeter()
{
    if (!mPanel.meterVisible() || mPanel.hasFatalError())
        return;

    float l = 0.0f, r = 0.0f;
    const bool have = mPeaks.poll(l, r);

    // advance() returns false once everything has settled to silence, and then nothing is
    // invalidated -- which is what stops a 30 fps tick repainting the window forever behind a
    // silent bridge. Borrowed from rations-amp, whose onTick gates on the same idea.
    if (mPanel.meter().advance(have, l, r))
        markDirty();
}

void App::tickDevices()
{
    if (mPanel.hasFatalError())
        return;
    refreshDevices(/*initialBuild=*/false);
}

void App::tickJack()
{
    mBridges.probeJack();
}

} // namespace audiogui
