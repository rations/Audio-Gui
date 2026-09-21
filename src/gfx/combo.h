// A combo box: a closed control showing the current item, and a popup list to change it.
//
// Adapted from simple-login-gui's gfx/menu.{h,cpp}, which is the only popup in this family of
// projects. What carries over is everything that makes a popup behave -- anchor-relative
// placement that flips and then clamps, two independent cursors for pointer and keyboard, modal
// key swallowing, and a click outside that closes AND IS SWALLOWED so a mis-aimed dismissal does
// not also press whatever was underneath it.
//
// What is dropped: that menu's two-click confirmation for destructive rows, which exists because
// one of its rows powers the machine off. Nothing in this window is destructive.
//
// What is added: a tick against the current item, which is the whole difference between a menu
// (a list of actions) and a combo box (a choice with a current value).
//
// PLACEMENT PREFERS BELOW, not above. The login screen's menu opens upward because its button is
// in the panel's bottom row; both combos here sit in the upper half of a short window, so down is
// the natural direction and up is the fallback.

#pragma once

#include "canvas.h"
#include "keys.h"
#include "palette.h"

#include <functional>
#include <string>
#include <vector>

namespace audiogui
{

struct ComboItem {
    std::string label;
    std::string value;   // the stable token the app matches on, never the label
    uint32_t swatch = 0; // an accent swatch drawn before the label, when swatched is set
    bool swatched = false;
};

class Combo
{
public:
    // Fired with the chosen index once the popup has already closed, so a handler is free to
    // rebuild this combo's own items.
    std::function<void(int index)> choose;

    void setItems(std::vector<ComboItem> items);
    const std::vector<ComboItem> &items() const
    {
        return mItems;
    }

    void setIndex(int i);
    int index() const
    {
        return mIndex;
    }
    // Empty when there is no current item.
    const std::string &value() const;

    void setRect(const Rect &r)
    {
        mRect = r;
    }
    const Rect &rect() const
    {
        return mRect;
    }

    void setEnabled(bool on);
    bool enabled() const
    {
        return mEnabled;
    }

    // --- the closed control ---
    void drawClosed(Canvas &c, const Palette &p) const;
    bool hitClosed(float x, float y) const;

    // --- the popup ---
    // Drawn LAST, after everything else on the panel: it is the only thing allowed to paint
    // outside its own section.
    void drawPopup(Canvas &c, const Palette &p) const;
    void open(const Rect &screen);
    void close();
    bool isOpen() const
    {
        return mOpen;
    }
    const Rect &popupRect() const
    {
        return mPopup;
    }

    // Return true when the event belonged to the popup and must not be passed on.
    void motion(float x, float y);
    bool click(float x, float y);
    bool key(Key k);

    int rowAt(float x, float y) const;

private:
    Rect rowRect(int row) const;
    void activateRow(int row);

    std::vector<ComboItem> mItems;
    Rect mRect;
    Rect mPopup;
    int mIndex = -1;
    int mHover = -1;     // the row the pointer is over
    int mHighlight = -1; // the row the keyboard is on
    bool mOpen = false;
    bool mEnabled = true;

public:
    bool hovered = false; // the closed control, set by the panel
};

} // namespace audiogui
