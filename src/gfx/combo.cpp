// See combo.h.

#include "combo.h"

#include "../geometry.h"
#include "ink.h"

#include <algorithm>
#include <cmath>

namespace audiogui
{
namespace
{

// Popup edges land on whole units. A popup whose top edge falls on a half unit renders its first
// row's text half a pixel off from every other row's, which is visible as a wobble when the list
// is long.
float snap(float v)
{
    return std::floor(v + 0.5f);
}

const std::string kNoValue;

} // namespace

void Combo::setItems(std::vector<ComboItem> items)
{
    mItems = std::move(items);
    if (mIndex >= static_cast<int>(mItems.size()))
        mIndex = mItems.empty() ? -1 : static_cast<int>(mItems.size()) - 1;
    if (mIndex < 0 && !mItems.empty())
        mIndex = 0;
    mHover = -1;
    mHighlight = mIndex;
}

void Combo::setIndex(int i)
{
    mIndex = (i >= 0 && i < static_cast<int>(mItems.size())) ? i : -1;
}

const std::string &Combo::value() const
{
    if (mIndex < 0 || mIndex >= static_cast<int>(mItems.size()))
        return kNoValue;
    return mItems[static_cast<size_t>(mIndex)].value;
}

void Combo::setEnabled(bool on)
{
    mEnabled = on;
    if (!on)
        close();
}

// --- the closed control -----------------------------------------------------

void Combo::drawClosed(Canvas &c, const Palette &p) const
{
    c.setColor(mEnabled ? p.input : p.window);
    c.fillRoundRect(mRect, geo::kComboRadius);
    c.setColor(inkFor(p, mEnabled, mOpen),
               (hovered || mOpen) ? kOutlineAlphaHover : kOutlineAlphaIdle);
    c.setPenSize(1.0f);
    c.strokeRoundRect(mRect, geo::kComboRadius);

    float textX = mRect.x + geo::kComboPadX;
    float textW = mRect.w - geo::kComboArrowW - geo::kComboPadX;

    if (mIndex >= 0 && mIndex < static_cast<int>(mItems.size())) {
        const ComboItem &it = mItems[static_cast<size_t>(mIndex)];

        // The accent picker's swatch. It was a 12x12 QPixmap painted at run time and handed to
        // QComboBox as an item icon; here it is the same rounded square, drawn in place.
        if (it.swatched) {
            const Rect sw(textX, mRect.centerY() - geo::kSwatch / 2.0f, geo::kSwatch, geo::kSwatch);
            c.setColor(mEnabled ? it.swatch : p.disabled);
            c.fillRoundRect(sw, geo::kSwatchRadius);
            textX += geo::kSwatch + 6.0f;
            textW -= geo::kSwatch + 6.0f;
        }

        c.setFont(Font::Body);
        c.setFontSize(geo::kBodySize);
        c.setColor(mEnabled ? p.text : p.disabled);
        const std::string s = c.clipToWidth(it.label, std::max(0.0f, textW));
        c.drawString(s.c_str(), textX, mRect.centerY() + geo::kBodySize * geo::kLabelBaselineBias);
    }

    // The arrow. Two strokes, for the same reason the checkmark is two strokes.
    const float ax = mRect.right() - geo::kComboArrowW / 2.0f;
    const float ay = mRect.centerY();
    const float k = 3.5f;
    c.setColor(mEnabled ? p.subtext : p.disabled);
    c.setPenSize(1.6f);
    c.strokeLine(ax - k, ay - k / 2.0f, ax, ay + k / 2.0f);
    c.strokeLine(ax, ay + k / 2.0f, ax + k, ay - k / 2.0f);
}

bool Combo::hitClosed(float x, float y) const
{
    return mEnabled && mRect.contains(x, y);
}

// --- the popup --------------------------------------------------------------

void Combo::open(const Rect &screen)
{
    if (!mEnabled || mItems.empty())
        return;

    const float h = geo::popupH(static_cast<int>(mItems.size()));
    const float w = std::max(mRect.w, geo::kPopupTickW + 40.0f);

    // Below by preference: both combos sit in the upper half of the window, so down is where
    // there is room. Flip above if it would run off the bottom, and clamp if neither fits --
    // a list drawn off the edge of the screen is a choice the user cannot make.
    float x = snap(mRect.left());
    float y = snap(mRect.bottom() + geo::kPopupGap);

    if (y + h > screen.bottom())
        y = snap(mRect.top() - geo::kPopupGap - h);
    if (y < screen.top())
        y = snap(screen.top());
    if (y + h > screen.bottom())
        y = snap(std::max(screen.top(), screen.bottom() - h));

    if (x + w > screen.right())
        x = snap(screen.right() - w);
    if (x < screen.left())
        x = snap(screen.left());

    mPopup = Rect(x, y, w, h);
    mOpen = true;
    mHover = -1;
    mHighlight = mIndex >= 0 ? mIndex : 0;
}

void Combo::close()
{
    mOpen = false;
    mHover = -1;
}

Rect Combo::rowRect(int row) const
{
    return Rect(mPopup.x, mPopup.y + geo::kPopupPadY + static_cast<float>(row) * geo::kPopupRowH,
                mPopup.w, geo::kPopupRowH);
}

int Combo::rowAt(float x, float y) const
{
    if (!mOpen || !mPopup.contains(x, y))
        return -1;
    for (size_t i = 0; i < mItems.size(); ++i) {
        if (rowRect(static_cast<int>(i)).contains(x, y))
            return static_cast<int>(i);
    }
    return -1;
}

void Combo::motion(float x, float y)
{
    if (!mOpen)
        return;
    mHover = rowAt(x, y);
    if (mHover >= 0)
        mHighlight = mHover;
}

void Combo::activateRow(int row)
{
    if (row < 0 || row >= static_cast<int>(mItems.size()))
        return;
    mIndex = row;
    close();
    // Closed BEFORE the callback, so a handler is free to rebuild this combo's items -- which
    // the device combo's handler does, on every selection.
    if (choose)
        choose(row);
}

bool Combo::click(float x, float y)
{
    if (!mOpen)
        return false;

    if (!mPopup.contains(x, y)) {
        close();
        return true; // swallowed: the dismissal does not also press what was underneath
    }

    const int row = rowAt(x, y);
    if (row >= 0)
        activateRow(row);
    return true;
}

bool Combo::key(Key k)
{
    if (!mOpen)
        return false;

    const int n = static_cast<int>(mItems.size());
    if (n == 0) {
        close();
        return true;
    }

    switch (k) {
        case Key::Up:
            mHighlight = (mHighlight - 1 + n) % n;
            mHover = -1;
            return true;
        case Key::Down:
            mHighlight = (mHighlight + 1) % n;
            mHover = -1;
            return true;
        case Key::Home:
            mHighlight = 0;
            mHover = -1;
            return true;
        case Key::End:
            mHighlight = n - 1;
            mHover = -1;
            return true;
        case Key::Enter:
            activateRow(mHighlight);
            return true;
        case Key::Escape:
            close();
            return true;
        default:
            // Everything else is swallowed while the popup is open, so a keystroke meant for the
            // list cannot reach the controls behind it.
            return true;
    }
}

void Combo::drawPopup(Canvas &c, const Palette &p) const
{
    if (!mOpen)
        return;

    c.setColor(p.surface);
    c.fillRoundRect(mPopup, geo::kPopupRadius);
    c.setColor(p.border);
    c.setPenSize(1.0f);
    c.strokeRoundRect(mPopup, geo::kPopupRadius);

    c.setFont(Font::Body);
    c.setFontSize(geo::kPopupTextSize);

    for (size_t i = 0; i < mItems.size(); ++i) {
        const int row = static_cast<int>(i);
        const Rect r = rowRect(row);
        const ComboItem &it = mItems[i];

        // A row lights for the pointer, or for the keyboard when the pointer is not in the list
        // at all. Two cursors, one highlight -- the pointer wins while it is present.
        const bool lit = (mHover == row) || (mHighlight == row && mHover < 0);
        if (lit) {
            c.setColor(p.accent, kOnFillAlpha);
            c.fillRect(Rect(r.x + 1.0f, r.y, r.w - 2.0f, r.h));
        }

        const uint32_t fg = lit ? p.onAccent : p.text;
        const float baseline = r.centerY() + geo::kPopupTextSize * geo::kLabelBaselineBias;

        // The tick gutter: what makes this a combo box rather than a menu.
        if (row == mIndex) {
            c.setColor(fg);
            c.setPenSize(2.0f);
            const float tx = r.x + geo::kPopupTickW * 0.30f;
            const float tm = r.x + geo::kPopupTickW * 0.48f;
            const float te = r.x + geo::kPopupTickW * 0.74f;
            c.strokeLine(tx, r.centerY(), tm, r.centerY() + r.h * 0.18f);
            c.strokeLine(tm, r.centerY() + r.h * 0.18f, te, r.centerY() - r.h * 0.20f);
        }

        float x = r.x + geo::kPopupTickW;
        float maxW = r.right() - x - geo::kComboPadX;

        if (it.swatched) {
            const Rect sw(x, r.centerY() - geo::kSwatch / 2.0f, geo::kSwatch, geo::kSwatch);
            c.setColor(it.swatch);
            c.fillRoundRect(sw, geo::kSwatchRadius);
            x += geo::kSwatch + 6.0f;
            maxW -= geo::kSwatch + 6.0f;
        }

        c.setColor(fg);
        const std::string s = c.clipToWidth(it.label, std::max(0.0f, maxW));
        c.drawString(s.c_str(), x, baseline);
    }
}

} // namespace audiogui
