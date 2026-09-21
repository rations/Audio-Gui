// The one place X11 keysyms and the widgets meet.
//
// src/gfx/ and src/panel.cpp link cairo and never X11, so they cannot name XK_BackSpace. The window
// calls this on every KeyPress and hands the widgets a gfx/keys.h value instead. Keeping the
// translation here, rather than a switch inside a widget, is also what makes the panel
// testable with no X server: tools/uirender drives it with Key values directly.
//
// Every constant below was read out of the installed keysymdef.h rather than recalled:
//   cites: X11/keysymdef.h:200 XK_BackSpace, :201 XK_Tab, :204 XK_Return, :208 XK_Escape,
//          :209 XK_Delete, :248-:257 XK_Home/Left/Up/Right/Down/End, :282 XK_KP_Enter,
//          :287-:299 the XK_KP_* duplicates, :435 XK_ISO_Left_Tab.
//   cites: X11/keysym.h:50 -- XK_XKB_KEYS is defined there, which is what makes
//          XK_ISO_Left_Tab visible at all. Without it, Shift+Tab silently falls through to
//          "no editing meaning".
//
// The keypad duplicates are not padding. With NumLock off a keypad arrow reports XK_KP_Left
// rather than XK_Left, and a field that ignored them would stop taking the arrow keys for a
// user who happens to be using the number pad -- which, on a login screen, is a user typing
// their password with one hand.

#pragma once

#include "../gfx/keys.h"

// X.h for the KeySym type itself (cites: X11/X.h:106, `typedef XID KeySym`), keysym.h for the
// constants. keysym.h does not pull in X.h, so naming only the one with the #defines in it
// compiles everywhere Xlib.h happens to have been included first and nowhere else.
#include <X11/X.h>
#include <X11/keysym.h>

namespace audiogui
{

inline Key keyFromSym(KeySym sym)
{
    switch (sym) {
        case XK_BackSpace:
            return Key::Backspace;
        case XK_Delete:
        case XK_KP_Delete:
            return Key::Delete;
        case XK_Left:
        case XK_KP_Left:
            return Key::Left;
        case XK_Right:
        case XK_KP_Right:
            return Key::Right;
        case XK_Up:
        case XK_KP_Up:
            return Key::Up;
        case XK_Down:
        case XK_KP_Down:
            return Key::Down;
        case XK_Home:
        case XK_KP_Home:
            return Key::Home;
        case XK_End:
        case XK_KP_End:
            return Key::End;
        case XK_Tab:
            return Key::Tab;
        case XK_ISO_Left_Tab:
            return Key::BackTab;
        case XK_Return:
        case XK_KP_Enter:
            return Key::Enter;
        case XK_Escape:
            return Key::Escape;
        default:
            return Key::Plain;
    }
}

} // namespace audiogui
