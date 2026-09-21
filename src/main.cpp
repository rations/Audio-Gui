// main.cpp — Audio-Gui entry point.
//
// Normal launch shows the control window. `audio-gui --restore` is a headless path used by the
// XDG autostart entry at login: it re-applies the saved routing (starting the chosen bridge
// detached) and exits, so audio works before — and without — the GUI ever being opened.
//
// This file is the ONLY place the window and the application meet. App includes no X11 header and
// the window knows nothing about ALSA; everything below is the wiring between them, following
// CPU-Power's main.cpp: handlers never paint, they set a dirty flag, and one invalidate() per
// handler turns into one repaint per pass round the loop.

#include "BridgeManager.h"
#include "app.h"
#include "geometry.h"
#include "platform/appicon.h"
#include "platform/keymap.h"
#include "platform/x11window.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace audiogui;

namespace
{

float scaleFromArgs(int argc, char **argv)
{
    // --scale, then $AUDIO_GUI_SCALE, then 1. There is no automatic DPI detection here, for the
    // same reason the sibling projects have none: Xft.dpi, RandR's physical size and the
    // toolkit-specific overrides disagree with each other often enough that guessing produces a
    // window that is wrong in a way the user cannot correct.
    const char *v = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--scale") == 0 && i + 1 < argc)
            v = argv[i + 1];
    }
    if (!v)
        v = getenv("AUDIO_GUI_SCALE");
    if (!v || !*v)
        return 1.0f;

    char *end = nullptr;
    const double d = strtod(v, &end);
    if (end == v || d <= 0.0)
        return 1.0f;
    if (d < geo::kScaleMin)
        return geo::kScaleMin;
    if (d > geo::kScaleMax)
        return geo::kScaleMax;
    return static_cast<float>(d);
}

} // namespace

int main(int argc, char *argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--restore") == 0) {
            // No GUI, no event loop, no libjack: apply the saved mode (the bridge is detached)
            // and quit.
            BridgeManager mgr(/*loadJack=*/false);
            mgr.restoreSavedMode();
            return 0;
        }
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            printf("usage: audio-gui [--restore] [--scale N]\n"
                   "  --restore   re-apply the saved routing and exit (used at login)\n"
                   "  --scale N   render at N times the logical size (%.2f..%.2f)\n",
                   static_cast<double>(geo::kScaleMin), static_cast<double>(geo::kScaleMax));
            return 0;
        }
    }

    App app;

    X11Window win;
    win.setIcon(appicon::kData, appicon::kWords);

    // The window opens at the height the content needs and is bounded by what the content can
    // ever need; geometry.h asserts both against the worst case AlsaMixer's table allows.
    app.panel().layout();
    if (!win.open("Audio Control", "audio-gui", geo::kWinW, app.panel().height(), geo::kWinHMin,
                  geo::kWinHMax, scaleFromArgs(argc, argv)))
        return 1;

    if (!win.fontsAreBundled())
        fprintf(stderr, "audio-gui: drawing with a system font; labels may not fit their slots\n");

    app.requestResize = [&win](float h) { win.resize(h); };
    app.paintNow = [&win]() { win.paintNow(); };

    // The mixer's poll descriptors report an external volume change. They do NOT survive a
    // reopen -- snd_mixer_close invalidates them -- so they are torn down and re-registered
    // every time the output device changes and the mixer follows it onto another card.
    std::vector<int> mixerTokens;
    auto registerMixerFds = [&] {
        for (int t : mixerTokens)
            win.removeFd(t);
        mixerTokens.clear();
        for (int fd : app.mixerFds())
            mixerTokens.push_back(win.addFd(fd, [&app, &win] {
                app.mixerEvent();
                if (app.takeDirty())
                    win.invalidate();
            }));
    };
    app.onMixerFdsChanged = registerMixerFds;

    app.start();
    registerMixerFds();

    // Three clocks, on their own deadlines. The meter's is the one that matters: the window this
    // is ported from fired its single tick only when select() timed out, which a drag starves
    // indefinitely -- and "the meter stops while you move a slider" is what that looks like here.
    win.addTimer(App::kMeterTickMs, [&] {
        app.tickMeter();
        if (app.takeDirty())
            win.invalidate();
    });
    win.addTimer(App::kDeviceTickMs, [&] {
        app.tickDevices();
        if (app.takeDirty())
            win.invalidate();
    });
    win.addTimer(App::kJackTickMs, [&] {
        app.tickJack();
        if (app.takeDirty())
            win.invalidate();
    });

    X11Window::Callbacks cb;

    cb.draw = [&app](Canvas &c) { app.panel().draw(c); };

    cb.button = [&](float x, float y, bool pressed) {
        if (pressed)
            app.panel().press(x, y);
        else
            app.panel().release(x, y);
        // A press or release always changes something visible: a thumb, an outline, a popup.
        app.markDirty();
        if (app.takeDirty())
            win.invalidate();
    };

    cb.motion = [&](float x, float y) {
        // Motion fires continuously, so this does NOT dirty unconditionally: Panel::motion sets
        // the flag itself only when a hover actually moved or a drag changed a value. Marking
        // every pixel dirty would recompose the whole window for a pointer crossing it.
        app.panel().motion(x, y);
        if (app.takeDirty())
            win.invalidate();
    };

    cb.key = [&](KeySym sym) {
        const bool handled = app.panel().key(keyFromSym(sym));
        if (app.takeDirty())
            win.invalidate();
        return handled;
    };

    win.run(cb);
    return 0;
}
