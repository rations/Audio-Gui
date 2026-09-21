// Non-fatal X error handling.
//
// Xlib's default error handler calls exit(). One BadWindow -- a window manager racing us on
// shutdown, a stale resource id -- would therefore take the process down mid-session, and this
// process is holding a pipe whose closure restores the machine. Ending abruptly is not a
// correctness problem here (the helper restores on EOF either way, by design), but it is a
// terrible way to find out about a recoverable error, and it loses the diagnostic.
//
// Ported from the rations-amp plug-in's x11plugview.cpp, where it exists because a plug-in that
// calls exit() takes the HOST down with it. The mechanism is the same; only the stakes differ.
//
// Errors on a display that is not ours are forwarded to whatever handler was installed before, so
// this never swallows another library's diagnostics.

#pragma once

#include <X11/Xlib.h>

namespace audiogui
{

// Install the process-wide handler (once) and start treating errors on `display` as ours.
void registerDisplay(::Display *display);
void unregisterDisplay(::Display *display);

// X requests are asynchronous, so a rejected XCreateWindow does not fail in place: the only way
// to find out is to sample this, round-trip with XSync, and sample it again. That is what makes
// asynchronous window-creation failure detectable at all.
unsigned long errorCount();

} // namespace audiogui
