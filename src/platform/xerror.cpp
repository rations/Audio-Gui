// See xerror.h. Ported from rations-amp's src/platform/x11plugview.cpp.

#include "xerror.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <vector>

namespace audiogui
{
namespace
{

// gOurDisplays, gPreviousErrorHandler and gErrorHandlerOnce are process-wide and guarded by
// gErrorMutex. Displays are added and removed on the GUI thread; the handler itself can be
// entered from any thread that makes an X call, which is why the lock is taken there too.
std::mutex gErrorMutex;
std::vector<::Display *> gOurDisplays;
XErrorHandler gPreviousErrorHandler = nullptr;
std::once_flag gErrorHandlerOnce;

std::atomic<unsigned long> gErrorCount{0};

int xErrorHandler(::Display *display, XErrorEvent *event)
{
    bool ours = false;
    XErrorHandler previous = nullptr;
    {
        std::lock_guard<std::mutex> lock(gErrorMutex);
        for (::Display *d : gOurDisplays) {
            if (d == display) {
                ours = true;
                break;
            }
        }
        previous = gPreviousErrorHandler;
    }

    if (!ours && previous)
        return previous(display, event);

    gErrorCount.fetch_add(1);

    char text[128];
    text[0] = '\0';
    XGetErrorText(display, event->error_code, text, sizeof(text));
    fprintf(stderr, "audio-gui: X error %u (%s) on request %u.%u, resource 0x%lx - ignored\n",
            static_cast<unsigned>(event->error_code), text,
            static_cast<unsigned>(event->request_code), static_cast<unsigned>(event->minor_code),
            static_cast<unsigned long>(event->resourceid));
    return 0;
}

} // namespace

//------------------------------------------------------------------------
void registerDisplay(::Display *display)
{
    std::call_once(gErrorHandlerOnce,
                   [] { gPreviousErrorHandler = XSetErrorHandler(xErrorHandler); });
    std::lock_guard<std::mutex> lock(gErrorMutex);
    gOurDisplays.push_back(display);
}

void unregisterDisplay(::Display *display)
{
    std::lock_guard<std::mutex> lock(gErrorMutex);
    for (size_t i = 0; i < gOurDisplays.size(); ++i) {
        if (gOurDisplays[i] == display) {
            gOurDisplays.erase(gOurDisplays.begin() + static_cast<ptrdiff_t>(i));
            return;
        }
    }
}

unsigned long errorCount()
{
    return gErrorCount.load();
}

} // namespace audiogui
