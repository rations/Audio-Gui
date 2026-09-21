// The window's one screen: layout, drawing and hit-testing.
//
// CAIRO ONLY. No Xlib, no libasound, no knowledge of bridges or shared memory. It is handed
// display strings and widget states by app.cpp and hands back semantic events, which is what lets
// tools/uirender compose and audit the real panel with no X server and no sound card.
//
// THERE IS A LAYOUT PASS, which neither sibling project needs. CPU-Power resolves every rectangle
// at compile time in its geometry.h; this window cannot, because the mixer strips and the switch
// checkboxes come from whatever ALSA elements the selected device exposes. So layout() walks the
// sections top to bottom, gives each widget its rect, and returns the height the window should
// be. geometry.h still fixes every constant it uses and asserts the worst case.
//
// PRESS AND RELEASE ARE PAIRED, following simple-login-gui's panel: one targetAt() serves both
// halves, press only records what was under the pointer, and release acts only if it is still the
// same thing. Acting on press means a control fires under the finger with no way to change your
// mind once it is down.

#pragma once

#include "gfx/canvas.h"
#include "gfx/combo.h"
#include "gfx/keys.h"
#include "gfx/levelmeter.h"
#include "gfx/palette.h"
#include "gfx/widgets.h"

#include <functional>
#include <string>
#include <vector>

namespace audiogui
{

// Mirrors BridgeManager::Mode, so this file needs no header from the audio side.
enum class Routing { PulseToAlsa = 0, PureAlsa = 1, PulseToJack = 2 };

class Panel
{
public:
    // One mixer strip: a label, a volume slider, and the mute/capture button.
    struct Strip {
        std::string label;
        Slider slider;
        IconButton button;
        bool hasVolume = false;
        bool hasSwitch = false;
        int element = -1; // index into AlsaMixer::elements(), for the app to act on
    };

    struct Callbacks {
        std::function<void(int strip, int percent)> volume;
        std::function<void(int strip, bool on)> mute;
        std::function<void(int index, bool on)> switchToggled;
        std::function<void(Routing)> routing;
        std::function<void(int deviceIndex)> device;
        std::function<void(int accentIndex)> accent;
        std::function<void()> toggleTheme;
    };
    Callbacks cb;

    // --- content -----------------------------------------------------------
    // Replaces every strip and switch. Called on every device change, exactly as the Qt build
    // destroyed and rebuilt its widgets.
    void setStrips(std::vector<Strip> strips);
    void setSwitches(std::vector<std::string> labels, std::vector<bool> states);

    // Shown instead of the strips when the device exposes nothing we can drive (USB and HDMI on a
    // card shared with the internal codec).
    void setPlaceholder(const std::string &text);

    // When set, the whole window is this message and nothing else: the mixer could not be opened.
    void setFatalError(const std::string &text);
    bool hasFatalError() const
    {
        return !mFatalError.empty();
    }

    void setDevices(std::vector<ComboItem> items, int currentIndex);
    void setRouting(Routing r);
    void setJackAvailable(bool on);
    void setDeviceComboEnabled(bool on, const std::string &tooltipUnused);
    void setMeterVisible(bool on);
    void setAccent(Accent a);
    void setMode(Mode m);

    Mode mode() const
    {
        return mMode;
    }
    Accent accent() const
    {
        return mAccent;
    }
    const Palette &palette() const
    {
        return mPalette;
    }
    LevelMeter &meter()
    {
        return mMeter;
    }
    bool meterVisible() const
    {
        return mMeterVisible;
    }

    // The device combo's current token, for the live device refresh: a rebuild has to preserve
    // what is selected NOW, which is not necessarily what is persisted.
    const std::string &deviceValue() const
    {
        return mDeviceCombo.value();
    }
    bool deviceComboOpen() const
    {
        return mDeviceCombo.isOpen();
    }

    std::vector<Strip> &strips()
    {
        return mStrips;
    }
    const std::vector<Strip> &strips() const
    {
        return mStrips;
    }

    // --- layout and paint ---------------------------------------------------
    // Assigns every widget its rect and returns the window height the content needs.
    float layout();
    float height() const
    {
        return mHeight;
    }

    void draw(Canvas &c) const;

    // --- input --------------------------------------------------------------
    // (-1, -1) means the pointer left the window, and clears every hover and drag.
    void motion(float x, float y);
    void press(float x, float y);
    void release(float x, float y);
    bool key(Key k);

    // True while a slider is being dragged, so the app can refuse to overwrite the value
    // underneath the pointer when the mixer reports an external change.
    bool dragging() const
    {
        return mDragStrip >= 0;
    }

    // Set by motion() when a hover or a dragged value actually changed, so a pointer merely
    // crossing the window does not recompose it thirty times on the way past. Read and cleared
    // by the app's dirty flag.
    std::function<void()> onNeedsRepaint;

private:
    enum class Target {
        // NOT called None: X11/X.h defines None as 0L, and a macro does not care that this is a
        // scoped enumeration. Both sibling projects hit this and renamed the same way.
        Nothing,
        Slider,
        MuteButton,
        Radio,
        Switch,
        DeviceCombo,
        AccentCombo,
        ThemeToggle,
    };

    Target targetAt(float x, float y, int &index) const;
    void clearHover();

    // Everything the pointer can light up, collapsed to something comparable, so motion() can
    // ask "did anything change?" without each control having to report it.
    struct HoverState {
        int slider = -1;
        int mute = -1;
        int radio = -1;
        int sw = -1;
        bool device = false;
        bool accent = false;
        bool theme = false;
        bool operator==(const HoverState &o) const
        {
            return slider == o.slider && mute == o.mute && radio == o.radio && sw == o.sw &&
                   device == o.device && accent == o.accent && theme == o.theme;
        }
    };
    HoverState hoverState() const;

    // Sections, in the order they are stacked.
    Rect mMixerFrame, mDeviceFrame, mRoutingFrame;
    Rect mPlaceholderRow, mMeterLabelRow, mBarRow;
    Rect mThemeToggleRect;

    std::vector<Strip> mStrips;
    std::vector<Toggle> mSwitches;
    Toggle mRadios[3];
    Combo mDeviceCombo;
    Combo mAccentCombo;

    LevelMeter mMeter;
    bool mMeterVisible = true;
    bool mThemeHovered = false;

    std::string mPlaceholder;
    std::string mFatalError;

    Routing mRouting = Routing::PulseToAlsa;
    bool mJackAvailable = false;

    Mode mMode = Mode::Dark;
    Accent mAccent = Accent::Green;
    Palette mPalette = paletteFor(Mode::Dark, Accent::Green);

    float mHeight = 0.0f;

    // Press/release pairing.
    Target mPressTarget = Target::Nothing;
    int mPressIndex = -1;
    int mDragStrip = -1;
};

} // namespace audiogui
