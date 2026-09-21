// See x11window.h.

#include "x11window.h"

#include "xerror.h"
#include "respath.h"

#include <cairo/cairo-xlib.h>

#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>

#include <sys/select.h>
#include <sys/time.h>

#include <cerrno>
#include <cstdio>
#include <ctime>
#include <vector>

namespace audiogui
{

namespace
{

//------------------------------------------------------------------------
// Publish the application icon on the window itself, as _NET_WM_ICON.
//
// THIS IS NOT THE SAME MECHANISM as the installed icon theme, and it is the one that matters for
// this program's audience. A desktop environment finds an icon through the desktop entry's Icon=
// key and the icon theme; a PLAIN WINDOW MANAGER never reads desktop entries at all, and takes a
// live window's icon from this property. CPU-Power is aimed at people running a window manager,
// so shipping only the theme copy would leave the titlebar and task list blank on exactly the
// systems it was written for.
//
// AUDIO-GUI SHIPS NO EMBEDDED ARTWORK TODAY, so setIcon() is simply never called and nothing is
// published; the desktop entry's Icon=multimedia-volume-control is all there is. The mechanism is
// kept whole rather than deleted because adding artwork later should be one call at start-up and
// not a re-derivation of what follows.
//
// The caller hands over the property's own layout, which EWMH 1.5 section 5.12 gives as an array
// of 32-bit packed ARGB cardinals -- high byte alpha, low byte blue -- with each image preceded
// by its width and height, rows left to right and top to bottom, and several images concatenated.
// The pixels are NOT premultiplied, which the specification does not say and which had to be
// measured; CPU-Power's scripts/emit-appicon.py records what was measured and how.
//
// The one conversion that is unavoidable is the width of the array. A property declared /32 goes
// over the wire as 32 bits per value, but Xlib takes it from the caller as an array of LONG,
// which is 64 bits here, and narrows it itself. Handing XChangeProperty the packed uint32_t
// array directly would read the right number of bytes and encode the wrong thing entirely --
// every second pixel becoming the top half of the pair before it -- so it is widened here.
static void publishIcon(Display *dpy, Window win, const uint32_t *icon, size_t words)
{
    if (!icon || words == 0)
        return;

    // One XChangeProperty is one X request, and a request that exceeds the server's limit is an
    // error rather than a silent truncation. XMaxRequestSize is in 4-byte units and this payload
    // is fixed at compile time, so the check is cheap and the failure says what happened instead
    // of surfacing later as a BadLength against an unrelated call.
    const long need = 6 + static_cast<long>(words); // request header + data, in words
    if (need > XMaxRequestSize(dpy)) {
        fprintf(stderr, "audio-gui: the window icon is too large for this X server, skipping it\n");
        return;
    }

    std::vector<unsigned long> prop(icon, icon + words);

    // Not error-checked here on purpose: XChangeProperty is asynchronous and its return value
    // carries no status. The round trip below -- sample errorCount(), XSync, sample again --
    // is what actually catches a rejected request, and it already covers this one.
    XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_ICON", False), XA_CARDINAL, 32,
                    PropModeReplace, reinterpret_cast<const unsigned char *>(prop.data()),
                    static_cast<int>(prop.size()));
}

} // namespace

//------------------------------------------------------------------------
X11Window::~X11Window()
{
    close();
}

//------------------------------------------------------------------------
bool X11Window::open(const std::string &title, const std::string &wmClass, float logicalW,
                     float logicalH, float minH, float maxH, float scale)
{
    mLogicalW = logicalW;
    mLogicalH = logicalH;
    mMinH = minH > 0.0f ? minH : logicalH;
    mMaxH = maxH > 0.0f ? maxH : logicalH;
    if (mMaxH < mMinH)
        mMaxH = mMinH;
    mScale = scale > 0.0f ? scale : 1.0f;

    mDpy = XOpenDisplay(nullptr);
    if (!mDpy) {
        fprintf(stderr, "audio-gui: cannot open the X display ($DISPLAY)\n");
        return false;
    }

    // BEFORE any window exists: Xlib's default handler calls exit(), and the failure this
    // function goes on to detect would otherwise be detected by dying.
    registerDisplay(mDpy);

    const int screen = DefaultScreen(mDpy);
    const unsigned w = static_cast<unsigned>(mLogicalW * mScale + 0.5f);
    const unsigned h = static_cast<unsigned>(mLogicalH * mScale + 0.5f);

    const unsigned long before = errorCount();
    mWin = XCreateSimpleWindow(mDpy, RootWindow(mDpy, screen), 0, 0, w, h, 0,
                               BlackPixel(mDpy, screen), BlackPixel(mDpy, screen));

    XStoreName(mDpy, mWin, title.c_str());

    // So a desktop entry's StartupWMClass can match this window. That is the DESKTOP
    // ENVIRONMENT half of being identifiable -- it is what lets a dock or a taskbar tie a
    // running window back to the installed .desktop file and its themed icon. The window
    // manager half is _NET_WM_ICON, published below, which needs no desktop entry at all.
    // BOTH halves are the class token, not the human title. ICCCM gives res_name as the name the
    // program was invoked with and res_class as the general class of application, and a desktop
    // entry's StartupWMClass is matched against one or the other depending on whose
    // implementation is reading it -- so making them the same string removes the question. VLC
    // on this machine advertises ("vlc", "vlc") for the same reason. A res_class carrying the
    // display title, spaces and all, would match on some desktops and silently not on others.
    std::vector<char> resName(wmClass.begin(), wmClass.end());
    resName.push_back('\0');
    std::vector<char> resClass(wmClass.begin(), wmClass.end());
    resClass.push_back('\0');
    XClassHint classHint = {};
    classHint.res_name = resName.data();
    classHint.res_class = resClass.data();
    XSetClassHint(mDpy, mWin, &classHint);

    applySizeHints();

    XSelectInput(mDpy, mWin,
                 ExposureMask | StructureNotifyMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | KeyPressMask | LeaveWindowMask);

    // Before the map, because a window manager reads a new window's properties when it is mapped
    // and is not obliged to notice one that turns up afterwards.
    publishIcon(mDpy, mWin, mIconData, mIconWords);

    mWmDelete = XInternAtom(mDpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(mDpy, mWin, &mWmDelete, 1);
    XMapWindow(mDpy, mWin);

    // THE ROUND TRIP. XCreateWindow is asynchronous: if it was rejected, nothing above has failed
    // yet and the first symptom would be an unrelated error against a window id that never
    // existed. Sample, sync, sample.
    XSync(mDpy, False);
    if (errorCount() != before) {
        fprintf(stderr, "audio-gui: the X server rejected the window (see the error above)\n");
        close();
        return false;
    }

    mTarget = cairo_xlib_surface_create(mDpy, mWin, DefaultVisual(mDpy, screen),
                                        static_cast<int>(w), static_cast<int>(h));
    if (cairo_surface_status(mTarget) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "audio-gui: could not create the drawing surface\n");
        close();
        return false;
    }

    mFontsLoaded = mFonts.load(resourceDir());
    return true;
}

//------------------------------------------------------------------------
void X11Window::releaseBuffer()
{
    if (mBuffer) {
        cairo_surface_destroy(mBuffer);
        mBuffer = nullptr;
    }
    mBufferW = mBufferH = 0;
}

void X11Window::close()
{
    releaseBuffer();
    if (mTarget) {
        cairo_surface_destroy(mTarget);
        mTarget = nullptr;
    }
    if (mDpy) {
        if (mWin) {
            XDestroyWindow(mDpy, mWin);
            mWin = 0;
        }
        unregisterDisplay(mDpy);
        XCloseDisplay(mDpy);
        mDpy = nullptr;
    }
}

//------------------------------------------------------------------------
void X11Window::paint(const Callbacks &cb)
{
    if (!cb.draw || !mTarget)
        return;

    const int pw = static_cast<int>(mLogicalW * mScale + 0.5f);
    const int ph = static_cast<int>(mLogicalH * mScale + 0.5f);

    // Compose offscreen. ARGB32 is premultiplied -- the one Cairo convention not pinned in Canvas,
    // because it belongs to whoever creates the surface, which is here.
    //
    // The buffer is KEPT between frames and reallocated only when the size changes. The window it
    // was ported from allocated one per frame, which is invisible on a panel that repaints when
    // something is clicked and is thirty allocations a second behind a running meter.
    if (!mBuffer || mBufferW != pw || mBufferH != ph) {
        releaseBuffer();
        mBuffer = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
        if (cairo_surface_status(mBuffer) != CAIRO_STATUS_SUCCESS) {
            releaseBuffer();
            return;
        }
        mBufferW = pw;
        mBufferH = ph;
    }
    cairo_surface_t *buf = mBuffer;

    cairo_t *cr = cairo_create(buf);
    // Reused surfaces hold the last frame, so clear before composing. CAIRO_OPERATOR_CLEAR rather
    // than painting the background colour: the panel paints its own ground and this only has to
    // guarantee nothing survives from the frame before.
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_restore(cr);
    // ONE scale, here. Nothing downstream of this knows the scale exists.
    cairo_scale(cr, mScale, mScale);
    {
        Canvas canvas(cr, &mFonts, mLogicalW, mLogicalH);
        cb.draw(canvas);
    }
    cairo_destroy(cr);

    // Blit once, SOURCE not OVER: the buffer is the frame, not a layer on top of the last one.
    cairo_t *out = cairo_create(mTarget);
    cairo_set_source_surface(out, buf, 0, 0);
    cairo_set_operator(out, CAIRO_OPERATOR_SOURCE);
    cairo_paint(out);
    cairo_destroy(out);

    cairo_surface_flush(mTarget);
    XFlush(mDpy);
}

//------------------------------------------------------------------------
// The width is fixed; the height is bounded by what the content can require. Telling the window
// manager the real range, rather than pinning both, is what lets resize() below be honoured
// instead of fought.
void X11Window::applySizeHints()
{
    if (!mDpy || !mWin)
        return;
    XSizeHints *hints = XAllocSizeHints();
    if (!hints)
        return;
    const int w = static_cast<int>(mLogicalW * mScale + 0.5f);
    hints->flags = PMinSize | PMaxSize;
    hints->min_width = hints->max_width = w;
    hints->min_height = static_cast<int>(mMinH * mScale + 0.5f);
    hints->max_height = static_cast<int>(mMaxH * mScale + 0.5f);
    XSetWMNormalHints(mDpy, mWin, hints);
    XFree(hints);
}

//------------------------------------------------------------------------
void X11Window::resize(float logicalH)
{
    if (!mDpy || !mWin)
        return;
    float h = logicalH;
    if (h < mMinH)
        h = mMinH;
    if (h > mMaxH)
        h = mMaxH;

    const int oldPx = static_cast<int>(mLogicalH * mScale + 0.5f);
    const int newPx = static_cast<int>(h * mScale + 0.5f);
    if (newPx == oldPx) {
        mLogicalH = h;
        return;
    }

    mLogicalH = h;

    // The hints go FIRST. A window manager that is still holding the old max_height may refuse
    // the resize request, and then the surface and the window would disagree about the size.
    applySizeHints();
    XResizeWindow(mDpy, mWin, static_cast<unsigned>(mLogicalW * mScale + 0.5f),
                  static_cast<unsigned>(newPx));

    // cites: cairo-xlib.h -- the surface must be told, it does not track the drawable.
    if (mTarget)
        cairo_xlib_surface_set_size(mTarget, static_cast<int>(mLogicalW * mScale + 0.5f), newPx);

    mDirty = true;
}

//------------------------------------------------------------------------
int X11Window::addFd(int fd, std::function<void()> onReady)
{
    if (fd < 0)
        return -1;
    const int token = mNextToken++;
    mFds.push_back({token, fd, std::move(onReady)});
    return token;
}

void X11Window::removeFd(int token)
{
    for (size_t i = 0; i < mFds.size(); ++i) {
        if (mFds[i].token == token) {
            mFds.erase(mFds.begin() + static_cast<long>(i));
            return;
        }
    }
}

//------------------------------------------------------------------------
namespace
{
long nowNs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long>(ts.tv_sec) * 1000000000L + ts.tv_nsec;
}
} // namespace

int X11Window::addTimer(int intervalMs, std::function<void()> fn)
{
    if (intervalMs <= 0)
        return -1;
    const int token = mNextToken++;
    const long interval = static_cast<long>(intervalMs) * 1000000L;
    mTimers.push_back({token, interval, nowNs() + interval, std::move(fn)});
    return token;
}

void X11Window::removeTimer(int token)
{
    for (size_t i = 0; i < mTimers.size(); ++i) {
        if (mTimers[i].token == token) {
            mTimers.erase(mTimers.begin() + static_cast<long>(i));
            return;
        }
    }
}

//------------------------------------------------------------------------
void X11Window::run(const Callbacks &cb)
{
    if (!mDpy || !mWin)
        return;

    mRunning = true;
    mDirty = true;
    mActive = &cb;

    const int xfd = ConnectionNumber(mDpy);

    while (mRunning) {
        // Drain everything the server has for us first, setting state but never painting: a drag
        // generates a MotionNotify per pixel and each one would otherwise be a full recompose.
        while (XPending(mDpy)) {
            XEvent ev;
            XNextEvent(mDpy, &ev);

            switch (ev.type) {
                case Expose:
                    mDirty = true;
                    break;

                case ClientMessage:
                    if (static_cast<Atom>(ev.xclient.data.l[0]) == mWmDelete)
                        mRunning = false;
                    break;

                case ConfigureNotify: {
                    // The window this was ported from selected StructureNotifyMask and then let
                    // this fall through to default:, because its window could not change size.
                    // This one can, and the surface does not track the drawable on its own, so a
                    // size it was not told about is drawn at the old size and clipped.
                    //
                    // Acted on whatever the source: this arrives as confirmation of our own
                    // XResizeWindow, and also when a window manager that does not honour the size
                    // hints gives us something else. Believing the server over our own intent is
                    // what keeps the two from disagreeing.
                    const int pw = static_cast<int>(mLogicalW * mScale + 0.5f);
                    const int ph = static_cast<int>(mLogicalH * mScale + 0.5f);
                    if (ev.xconfigure.width != pw || ev.xconfigure.height != ph) {
                        if (mTarget)
                            cairo_xlib_surface_set_size(mTarget, ev.xconfigure.width,
                                                        ev.xconfigure.height);
                        mLogicalH = static_cast<float>(ev.xconfigure.height) / mScale;
                        mDirty = true;
                    }
                    break;
                }

                case ButtonPress:
                case ButtonRelease:
                    if (ev.xbutton.button == Button1 && cb.button) {
                        cb.button(static_cast<float>(ev.xbutton.x) / mScale,
                                  static_cast<float>(ev.xbutton.y) / mScale,
                                  ev.type == ButtonPress);
                    }
                    break;

                case MotionNotify:
                    if (cb.motion) {
                        cb.motion(static_cast<float>(ev.xmotion.x) / mScale,
                                  static_cast<float>(ev.xmotion.y) / mScale);
                    }
                    break;

                case LeaveNotify:
                    // A pointer that left without a ButtonRelease would otherwise leave a control
                    // latched in its hover or dragging state.
                    if (cb.motion)
                        cb.motion(-1.0f, -1.0f);
                    break;

                case KeyPress: {
                    KeySym sym = NoSymbol;
                    char buf[16];
                    XLookupString(&ev.xkey, buf, sizeof(buf), &sym, nullptr);
                    const bool handled = cb.key ? cb.key(sym) : false;
                    if (!handled && sym == XK_Escape)
                        mRunning = false;
                    break;
                }

                default:
                    break;
            }
        }

        if (!mRunning)
            break;

        if (mDirty) {
            mDirty = false;
            paint(cb);
        }

        // Fire every timer that is due. THIS HAPPENS BEFORE THE WAIT AND UNCONDITIONALLY, which
        // is the whole difference from the window this was ported from. That one fired its single
        // tick only when select() returned 0, so any continuous stream of events -- a drag, in
        // practice -- reset the timeout every pass and the tick never ran at all. Here the
        // deadline is absolute and checked on its own terms, so dragging a volume slider cannot
        // stop the meter.
        long now = nowNs();
        {
            // A copy, because a timer's handler may add or remove timers.
            const std::vector<TimerEntry> due = mTimers;
            for (const TimerEntry &t : due) {
                if (t.dueNs > now)
                    continue;
                for (TimerEntry &live : mTimers) {
                    if (live.token != t.token)
                        continue;
                    // Re-arm from NOW rather than from the old deadline. Adding the interval to a
                    // deadline already in the past makes a timer that fell behind -- because a
                    // paint ran long, or the machine suspended -- try to catch up by firing back
                    // to back, which for a 33 ms meter is a burst of repaints nobody asked for.
                    live.dueNs = now + live.intervalNs;
                    break;
                }
                if (t.fn)
                    t.fn();
            }
        }

        if (!mRunning)
            break;

        // A timer handler may have dirtied the window (the meter does, every frame it moves) or
        // pushed more X requests out. Paint before sleeping rather than after waking.
        if (mDirty) {
            mDirty = false;
            paint(cb);
        }

        // Wait for the next event, descriptor or deadline, whichever comes first. XPending above
        // may have left events buffered inside Xlib that never reach the fd, so it is checked
        // again rather than slept through.
        if (XPending(mDpy))
            continue;

        fd_set r;
        FD_ZERO(&r);
        FD_SET(xfd, &r);
        int maxFd = xfd;
        for (const FdWatch &w : mFds) {
            if (w.fd < 0 || w.fd >= FD_SETSIZE)
                continue;
            FD_SET(w.fd, &r);
            if (w.fd > maxFd)
                maxFd = w.fd;
        }

        // Sleep only until the earliest deadline. With no timers at all, block indefinitely
        // rather than spinning on a zero timeout.
        now = nowNs();
        bool haveTimer = false;
        long waitNs = 0;
        for (const TimerEntry &t : mTimers) {
            const long left = t.dueNs - now;
            if (!haveTimer || left < waitNs) {
                haveTimer = true;
                waitNs = left;
            }
        }

        struct timeval tv;
        struct timeval *timeout = nullptr;
        if (haveTimer) {
            // A deadline can already be in the past -- a paint that ran long, a handler that
            // blocked. That is a zero timeout, not a negative one, and certainly not the
            // "block forever" a null timeout would mean.
            if (waitNs < 0)
                waitNs = 0;
            tv.tv_sec = waitNs / 1000000000L;
            tv.tv_usec = (waitNs % 1000000000L) / 1000L;
            timeout = &tv;
        }

        const int n = select(maxFd + 1, &r, nullptr, nullptr, timeout);
        if (n < 0 && errno != EINTR) {
            fprintf(stderr, "audio-gui: select on the X connection failed; closing\n");
            mRunning = false;
            break;
        }
        if (n <= 0)
            continue; // timed out, or interrupted: back round to the deadline check

        // Dispatch from a COPY: a handler may add or remove descriptors. The mixer's does,
        // every time the output device changes and the mixer is reopened onto another card,
        // which would otherwise invalidate the iterator underneath this loop.
        const std::vector<FdWatch> ready = mFds;
        for (const FdWatch &w : ready) {
            if (w.fd < 0 || w.fd >= FD_SETSIZE || !FD_ISSET(w.fd, &r))
                continue;
            // Still registered? A previous handler in this same pass may have removed it.
            bool live = false;
            for (const FdWatch &cur : mFds) {
                if (cur.token == w.token) {
                    live = true;
                    break;
                }
            }
            if (live && w.onReady)
                w.onReady();
        }
    }

    mActive = nullptr;
}

} // namespace audiogui
