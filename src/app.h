// The application: state, and turning panel events into actions.
//
// Sits between the toolkit-free Panel and the audio side (AlsaMixer, AlsaDevices, BridgeManager,
// PeakSource). It includes no X11 header, so everything except the window itself can be driven
// headless.
//
// This is where MainWindow's non-widget logic ended up: the device signature diff, the
// unplug-recovery branch, the USB/HDMI control suppression, the autostart entry, and the rules
// about which routing modes make the device combo and the meter meaningful.
//
// THE OWNER DRIVES THE CLOCK. App owns no timer and no event loop; main.cpp registers the three
// intervals on the window and calls the three tick methods below.

#pragma once

#include "AlsaDevices.h"
#include "AlsaMixer.h"
#include "BridgeManager.h"
#include "panel.h"
#include "peaksource.h"

#include <functional>
#include <string>
#include <vector>

namespace audiogui
{

class App
{
public:
    App();

    // How often main.cpp should call each tick. The meter's is the frame rate the Qt LevelMeter
    // ran at; the other two are the QTimer intervals MainWindow used.
    static constexpr int kMeterTickMs = 1000 / 30;
    static constexpr int kDeviceTickMs = 3000;
    static constexpr int kJackTickMs = BridgeManager::kJackProbeIntervalMs;

    // Wired by main.cpp to the window. App never touches X11 itself.
    std::function<void(float logicalHeight)> requestResize;
    std::function<void()> paintNow;

    Panel &panel()
    {
        return mPanel;
    }

    // Start-up, in the order the Qt build did it: routing is made active BEFORE the mixer opens
    // "default", because applying a mode can rewrite ~/.asoundrc and the mixer should see the
    // result rather than what was there before.
    void start();

    // The three clocks.
    void tickMeter();
    void tickDevices();
    void tickJack();

    // Called when one of the mixer's poll descriptors fires.
    void mixerEvent();

    // Descriptors change whenever the mixer is reopened onto another card; main.cpp re-registers
    // them through this.
    std::function<void()> onMixerFdsChanged;
    const std::vector<int> &mixerFds() const
    {
        return mMixer.pollDescriptors();
    }

    // Read-and-clear, as CPU-Power's App does: handlers set it, main.cpp turns it into exactly
    // one invalidate() per pass round the loop.
    bool takeDirty();
    void markDirty()
    {
        mDirty = true;
    }

    // The window's height follows its content; relayout and tell the window when it changed.
    void relayout();

private:
    void refreshDevices(bool initialBuild);
    void reopenMixerForDevice(const std::string &token);
    void populateMixerControls(bool suppressControls);
    void syncFromMixer();
    void updateDeviceComboEnabled();
    void updateMeterVisibility();
    void applyAppearance();

    // Index into mMixer.elements() for a panel strip / switch row.
    const AlsaMixer::Element *elementForStrip(int strip) const;
    const AlsaMixer::Element *elementForSwitch(int index) const;

    AlsaMixer mMixer;
    BridgeManager mBridges;
    PeakSource mPeaks;
    Panel mPanel;

    std::vector<AlsaDevices::OutputDevice> mDevices;
    std::vector<int> mSwitchElements; // element index per switch checkbox

    bool mDirty = true;
    bool mMixerOpen = false;
    float mLastHeight = 0.0f;
};

// Writes $XDG_CONFIG_HOME/autostart/audio-gui-restore.desktop if missing or stale.
//
// ITS BODY MUST STAY BYTE-FOR-BYTE what packaging/build-deb.sh installs to /etc/xdg/autostart/,
// or the two disagree and the GUI rewrites the file on every single launch.
void ensureAutostartEntry();

} // namespace audiogui
