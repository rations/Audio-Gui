// X11Window -- one top-level window, painted by hand through Canvas.
//
// Simpler than the plug-in case this is ported from: a top-level window has no XEmbed, no host
// run loop to cooperate with, and no IRunLoop to register with. It owns its own select() loop.
//
// Four rules from the plug-in carry over unchanged, because they are the ones that bite:
//
//   * DOUBLE BUFFER, ALWAYS. Compose into an ARGB32 image surface, blit once with
//     CAIRO_OPERATOR_SOURCE. A partially drawn frame is never visible.
//   * NEVER PAINT SYNCHRONOUSLY FROM AN EVENT HANDLER. Handlers set a dirty flag; the paint
//     happens once per pass round the loop, so a burst of motion events costs one repaint.
//   * INSTALL THE NON-FATAL X ERROR HANDLER (xerror.h) before creating anything.
//   * DETECT ASYNCHRONOUS XCreateWindow FAILURE by counting X errors across an XSync. X requests
//     do not fail in place, so a window that was never created otherwise shows up much later as
//     an unrelated BadDrawable.
//
// LAYOUT IS IN LOGICAL UNITS. The window is created at logical size x scale, one cairo_scale is
// applied at compose time, and mouse coordinates are divided by the scale before they reach the
// callbacks. No geometry constant anywhere has a scale factor baked into it.
//
// FOUR THINGS HERE ARE NOT IN THE CPU-Power WINDOW THIS IS PORTED FROM, because Audio-Gui needs
// them and that panel did not:
//
//   * EXTRA DESCRIPTORS (addFd). The ALSA mixer publishes poll descriptors that report an
//     external volume change -- somebody moving Master in alsamixer, or a media key. The Qt build
//     wrapped them in QSocketNotifier; here they join the same select() the X connection is in.
//
//   * SEVERAL TIMERS, ON DEADLINES (addTimer). That window had one interval and fired it only
//     when select() TIMED OUT, so a stream of motion events starved it indefinitely. At 250 ms on
//     a static panel nobody notices. This window runs a 33 ms meter, and "the meter freezes while
//     you drag a slider" is exactly what that bug looks like here. So timers carry absolute
//     CLOCK_MONOTONIC deadlines and fire when due, whatever select() returned.
//
//   * A SIZE THAT CHANGES (resize). The mixer strips and switch checkboxes are rebuilt from
//     whatever the selected device exposes, so the window's height is a function of its content.
//
//   * A BACK BUFFER THAT PERSISTS. That window allocated an ARGB32 surface every frame, which is
//     free at 4 fps on a static panel and is thirty allocations a second here.

#pragma once

#include "gfx/canvas.h"
#include "gfx/fontstack.h"

#include <X11/Xlib.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace audiogui
{

class X11Window
{
public:
    X11Window() = default;
    ~X11Window();

    X11Window(const X11Window &) = delete;
    X11Window &operator=(const X11Window &) = delete;

    // All coordinates handed to these are LOGICAL units, already divided by the scale.
    struct Callbacks {
        std::function<void(Canvas &)> draw;
        // pressed = true on ButtonPress, false on ButtonRelease. Button 1 only.
        std::function<void(float x, float y, bool pressed)> button;
        std::function<void(float x, float y)> motion;
        // Return true if the key was handled. Unhandled Escape closes the window.
        std::function<bool(KeySym sym)> key;
    };

    // Returns false having already warned: no display, no window, or a font stack that fell back
    // to a system face when the caller said that mattered.
    //
    // minH/maxH bound what resize() will later be allowed to ask for, and become the window's
    // WM size hints. Passing logicalH for both pins the window, which is what the sibling
    // projects do.
    bool open(const std::string &title, const std::string &wmClass, float logicalW, float logicalH,
              float minH, float maxH, float scale);

    // Publish an _NET_WM_ICON before open(). The payload is EWMH 1.5 section 5.12 packed ARGB
    // cardinals; see the note on publishIcon in the .cpp. Never calling this simply publishes no
    // icon, and the desktop entry's Icon= key is then all a desktop environment has to go on.
    void setIcon(const uint32_t *data, size_t words)
    {
        mIconData = data;
        mIconWords = words;
    }

    // Change the window's logical height, clamped to the bounds given to open(). Cheap and
    // idempotent: a call that does not change the height does nothing at all.
    void resize(float logicalH);

    float logicalHeight() const
    {
        return mLogicalH;
    }

    // Wait on `fd` as well as the X connection, calling `onReady` when it is readable. The token
    // returned removes it again.
    //
    // Handlers are dispatched from a COPY of the list, because a handler may add or remove
    // descriptors -- which the mixer's does, every time the output device changes and the mixer
    // is reopened onto a different card.
    int addFd(int fd, std::function<void()> onReady);
    void removeFd(int token);

    // Call `fn` every intervalMs. The token returned removes it again.
    //
    // The deadline is absolute, so a slow frame does not make the next tick late as well, and a
    // busy pointer cannot starve it.
    int addTimer(int intervalMs, std::function<void()> fn);
    void removeTimer(int token);

    void run(const Callbacks &cb);
    void stop()
    {
        mRunning = false;
    }
    // Ask for a repaint on the next pass. Cheap and idempotent -- call it from any handler.
    void invalidate()
    {
        mDirty = true;
    }

    // Repaint NOW, from inside a handler, and only for the one case that needs it: a handler
    // that is about to block for a long time. Switching routing mode SIGTERMs the running bridge
    // and then waits for it to actually exit -- up to three seconds, because the replacement
    // binds the same PA socket and must not race a bridge still shutting down. Leaving a stale
    // frame up for that long, with the radio still showing the old mode, looks like the window
    // has hung. Every other repaint goes through invalidate() and the loop's one paint per pass,
    // which is the rule this is the deliberate exception to.
    void paintNow()
    {
        if (mActive) {
            mDirty = false;
            paint(*mActive);
        }
    }

    bool fontsAreBundled() const
    {
        return mFontsLoaded;
    }

private:
    void paint(const Callbacks &cb);
    void close();
    void applySizeHints();
    void releaseBuffer();

    struct FdWatch {
        int token;
        int fd;
        std::function<void()> onReady;
    };
    struct TimerEntry {
        int token;
        long intervalNs;
        long dueNs; // absolute CLOCK_MONOTONIC
        std::function<void()> fn;
    };

    ::Display *mDpy = nullptr;
    ::Window mWin = 0;
    Atom mWmDelete = 0;
    cairo_surface_t *mTarget = nullptr;

    FontStack mFonts;
    bool mFontsLoaded = false;

    float mLogicalW = 0, mLogicalH = 0, mScale = 1.0f;
    float mMinH = 0, mMaxH = 0;

    // Reallocated only when the pixel size changes, not once per frame.
    cairo_surface_t *mBuffer = nullptr;
    int mBufferW = 0, mBufferH = 0;

    const uint32_t *mIconData = nullptr;
    size_t mIconWords = 0;

    std::vector<FdWatch> mFds;
    std::vector<TimerEntry> mTimers;
    int mNextToken = 1;
    // The callbacks run() was given, so paintNow() can compose the same frame the loop
    // would. Null outside run().
    const Callbacks *mActive = nullptr;

    bool mRunning = false;
    bool mDirty = true;
};

} // namespace audiogui
