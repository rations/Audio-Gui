// The editing keys, named without reference to X11.
//
// src/gfx/ links cairo and nothing else -- that is what lets tools/uirender compose and audit
// the real panel with no X server running, and it stops being true the moment a widget needs
// XK_BackSpace. So the window translates a KeySym into one of these (platform/keymap.h) and the
// widgets never see an X type.
//
// Text is carried separately, as UTF-8 bytes, because a keystroke can produce both (Return) or
// neither (a bare Shift) or text with no editing key at all (any ordinary character, and every
// character that arrives from a dead-key or Compose sequence).

#pragma once

namespace audiogui
{

enum class Key {
    // A key with no editing meaning; the text it produced, if any, is what matters.
    //
    // NOT called None. X11/X.h has `#define None 0L` (cites: X11/X.h:141), and a macro does
    // not care that this is a scoped enumeration -- `Key::None` expands to `Key::0L` in every
    // translation unit that has seen an X header, which is every one that translates a
    // keysym. The collision is written down in this project's rules as a known hazard rather
    // than something to rediscover; this is the rediscovery it was written down to prevent.
    Plain,
    Backspace,
    Delete,
    Left,
    Right,
    Home,
    End,
    Tab,
    BackTab, // Shift+Tab, which X reports as its own keysym rather than as a modifier
    Enter,
    Escape,
    Up,
    Down,
};

} // namespace audiogui
