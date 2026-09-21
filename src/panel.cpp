// See panel.h.

#include "panel.h"

#include "geometry.h"
#include "gfx/ink.h"

#include <algorithm>

namespace audiogui
{
namespace
{

// U+00BB RIGHT-POINTING DOUBLE ANGLE QUOTATION MARK, not the U+2192 ARROW the Qt build used.
// NEITHER BUNDLED FACE HAS U+2192 -- Roboto and Michroma both return glyph index 0 for it. Qt
// never showed the problem because it fell back to a system font per character; cairo's toy text
// API has one face and no fallback, so the arrow rendered as a hole in the label. Caught by
// tools/uirender's glyph audit, which exists because a missing glyph measures as ZERO WIDTH and
// therefore passes a does-it-fit check unnoticed.
const char *kRoutingLabels[3] = {
    "PA Bridge \xC2\xBB ALSA (default)",
    "ALSA (no bridge)",
    "PA Bridge \xC2\xBB JACK",
};

} // namespace

// --- content ----------------------------------------------------------------

void Panel::setStrips(std::vector<Strip> strips)
{
    mStrips = std::move(strips);
    // Any drag in progress belonged to a strip that no longer exists.
    mDragStrip = -1;
    mPressTarget = Target::Nothing;
}

void Panel::setSwitches(std::vector<std::string> labels, std::vector<bool> states)
{
    mSwitches.clear();
    mSwitches.reserve(labels.size());
    for (size_t i = 0; i < labels.size(); ++i) {
        Toggle t;
        t.label = labels[i];
        t.shape = Toggle::Shape::Check;
        t.on = i < states.size() ? states[i] : false;
        mSwitches.push_back(std::move(t));
    }
}

void Panel::setPlaceholder(const std::string &text)
{
    mPlaceholder = text;
}

void Panel::setFatalError(const std::string &text)
{
    mFatalError = text;
}

void Panel::setDevices(std::vector<ComboItem> items, int currentIndex)
{
    mDeviceCombo.setItems(std::move(items));
    mDeviceCombo.setIndex(currentIndex);
}

void Panel::setRouting(Routing r)
{
    mRouting = r;
    for (int i = 0; i < 3; ++i)
        mRadios[i].on = (static_cast<int>(r) == i);
}

void Panel::setJackAvailable(bool on)
{
    mJackAvailable = on;
    mRadios[2].enabled = on;
}

void Panel::setDeviceComboEnabled(bool on, const std::string &)
{
    mDeviceCombo.setEnabled(on);
}

void Panel::setMeterVisible(bool on)
{
    if (mMeterVisible && !on)
        mMeter.reset(); // a hidden meter must not come back still showing the last level
    mMeterVisible = on;
}

void Panel::setAccent(Accent a)
{
    mAccent = a;
    mPalette = paletteFor(mMode, mAccent);
}

void Panel::setMode(Mode m)
{
    mMode = m;
    mPalette = paletteFor(mMode, mAccent);
}

// --- layout -----------------------------------------------------------------

float Panel::layout()
{
    if (!mFatalError.empty()) {
        mHeight = geo::kWinHMin;
        return mHeight;
    }

    // Set up the fixed properties of the radios and the two combos once per pass; their rects
    // follow below. Doing it here rather than in a constructor keeps the labels in one place.
    for (int i = 0; i < 3; ++i) {
        mRadios[i].label = kRoutingLabels[i];
        mRadios[i].shape = Toggle::Shape::Radio;
        mRadios[i].on = (static_cast<int>(mRouting) == i);
    }
    mRadios[2].enabled = mJackAvailable;

    if (mAccentCombo.items().empty()) {
        std::vector<ComboItem> accents;
        for (int i = 0; i < kAccentCount; ++i) {
            ComboItem it;
            it.label = accentName(static_cast<Accent>(i));
            it.value = it.label;
            it.swatch = accentColor(static_cast<Accent>(i));
            it.swatched = true;
            accents.push_back(std::move(it));
        }
        mAccentCombo.setItems(std::move(accents));
    }
    mAccentCombo.setIndex(static_cast<int>(mAccent));

    const int stripCount = static_cast<int>(mStrips.size());
    const int switchCount = static_cast<int>(mSwitches.size());

    float y = geo::kMargin;

    // --- mixer section ---
    const float mixerInner = geo::mixerInnerH(stripCount, mMeterVisible);
    mMixerFrame = Rect(geo::kMargin, y + geo::kGroupTitleH, geo::kContentW,
                       geo::kGroupPadTop + mixerInner + geo::kGroupPadBottom);

    const float innerX = mMixerFrame.x + geo::kGroupPadX;
    float iy = mMixerFrame.y + geo::kGroupPadTop;

    if (stripCount == 0) {
        mPlaceholderRow = Rect(innerX, iy, geo::kGroupInnerW, geo::kPlaceholderH);
        iy += geo::kPlaceholderH;
    } else {
        mPlaceholderRow = Rect();
        for (Strip &s : mStrips) {
            const Rect row(innerX, iy, geo::kGroupInnerW, geo::kStripH);
            s.slider.rect = Rect(row.x + geo::kSliderX, row.y, geo::kSliderW, row.h);
            s.button.rect = Rect(row.right() - geo::kMuteW, row.centerY() - geo::kMuteH / 2.0f,
                                 geo::kMuteW, geo::kMuteH);
            s.slider.enabled = s.hasVolume;
            s.button.enabled = s.hasSwitch;
            iy += geo::kStripH;
        }
    }

    if (mMeterVisible) {
        iy += geo::kMeterTopGap;
        mMeterLabelRow = Rect(innerX, iy, geo::kGroupInnerW, geo::kMeterLabelH);
        iy += geo::kMeterLabelH;
        mMeter.rect = Rect(innerX, iy, geo::kGroupInnerW, geo::kMeterH);
        iy += geo::kMeterH;
    } else {
        mMeterLabelRow = Rect();
        mMeter.rect = Rect();
    }

    y = mMixerFrame.bottom() + geo::kSectionGap;

    // --- device section ---
    mDeviceFrame = Rect(geo::kMargin, y + geo::kGroupTitleH, geo::kContentW,
                        geo::kGroupPadTop + geo::deviceInnerH() + geo::kGroupPadBottom);
    mDeviceCombo.setRect(Rect(mDeviceFrame.x + geo::kGroupPadX, mDeviceFrame.y + geo::kGroupPadTop,
                              geo::kGroupInnerW, geo::kComboH));
    y = mDeviceFrame.bottom() + geo::kSectionGap;

    // --- routing section ---
    const float routingInner = geo::routingInnerH(switchCount);
    mRoutingFrame = Rect(geo::kMargin, y + geo::kGroupTitleH, geo::kContentW,
                         geo::kGroupPadTop + routingInner + geo::kGroupPadBottom);
    float ry = mRoutingFrame.y + geo::kGroupPadTop;
    const float rx = mRoutingFrame.x + geo::kGroupPadX;
    for (int i = 0; i < 3; ++i) {
        mRadios[i].rect = Rect(rx, ry, geo::kGroupInnerW, geo::kRowH);
        ry += geo::kRowH;
    }
    if (switchCount > 0) {
        ry += geo::kSwitchTopGap;
        for (Toggle &t : mSwitches) {
            t.rect = Rect(rx, ry, geo::kGroupInnerW, geo::kRowH);
            ry += geo::kRowH;
        }
    }
    y = mRoutingFrame.bottom() + geo::kSectionGap;

    // --- appearance bar ---
    mBarRow = Rect(geo::kMargin, y, geo::kContentW, geo::kBarH);
    mAccentCombo.setRect(Rect(mBarRow.x, mBarRow.y, geo::kAccentComboW, geo::kBarH));
    mThemeToggleRect = Rect(mBarRow.x + geo::kAccentComboW + geo::kBarGap, mBarRow.y,
                            geo::kModeToggleW, geo::kBarH);
    y = mBarRow.bottom() + geo::kMargin;

    mHeight = y;
    return mHeight;
}

// --- paint ------------------------------------------------------------------

void Panel::draw(Canvas &c) const
{
    c.setColor(mPalette.window);
    c.fillRect(c.bounds());

    if (!mFatalError.empty()) {
        // The whole window is the message. Two lines, centred, because the Qt build's one
        // centred QLabel carried an embedded newline and the second half is the actionable part.
        c.setFont(Font::Body);
        c.setFontSize(geo::kBodySize);
        c.setColor(mPalette.text);

        const float maxW = geo::kContentW;
        size_t start = 0;
        int line = 0;
        while (start <= mFatalError.size()) {
            const size_t nl = mFatalError.find('\n', start);
            const std::string part =
                mFatalError.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            const std::string s = c.clipToWidth(part, maxW);
            c.drawString(s.c_str(), (c.bounds().w - c.stringWidth(s.c_str())) / 2.0f,
                         c.bounds().h / 2.0f + static_cast<float>(line) * 20.0f);
            if (nl == std::string::npos)
                break;
            start = nl + 1;
            ++line;
        }
        return;
    }

    // --- mixer ---
    drawGroupBox(c, mPalette, mMixerFrame, "Mixer");

    if (mStrips.empty()) {
        if (!mPlaceholder.empty()) {
            c.setFont(Font::Body);
            c.setFontSize(geo::kBodySize);
            c.setColor(mPalette.subtext);
            const std::string s = c.clipToWidth(mPlaceholder, mPlaceholderRow.w);
            c.drawString(s.c_str(), mPlaceholderRow.centerX() - c.stringWidth(s.c_str()) / 2.0f,
                         mPlaceholderRow.centerY() + geo::kBodySize * geo::kLabelBaselineBias);
        }
    } else {
        for (const Strip &s : mStrips) {
            const Rect labelRow(mMixerFrame.x + geo::kGroupPadX, s.slider.rect.y, geo::kStripLabelW,
                                s.slider.rect.h);
            drawRowText(c, mPalette, labelRow, s.label.c_str(), mPalette.text, geo::kBodySize);
            if (s.hasVolume)
                s.slider.draw(c, mPalette);
            if (s.hasSwitch)
                s.button.draw(c, mPalette);
        }
    }

    if (mMeterVisible) {
        drawRowText(c, mPalette, mMeterLabelRow, "Output level", mPalette.subtext,
                    geo::kMeterLabelSize);
        mMeter.draw(c, mPalette);
    }

    // --- device ---
    drawGroupBox(c, mPalette, mDeviceFrame, "Output device");
    mDeviceCombo.drawClosed(c, mPalette);

    // --- routing ---
    drawGroupBox(c, mPalette, mRoutingFrame, "Audio options & switches");
    for (int i = 0; i < 3; ++i)
        mRadios[i].draw(c, mPalette);
    for (const Toggle &t : mSwitches)
        t.draw(c, mPalette);

    // --- appearance bar ---
    mAccentCombo.drawClosed(c, mPalette);
    {
        const Rect &r = mThemeToggleRect;
        c.setColor(mPalette.input);
        c.fillRoundRect(r, geo::kComboRadius);
        c.setColor(inkFor(mPalette, true, false),
                   mThemeHovered ? kOutlineAlphaHover : kOutlineAlphaIdle);
        c.setPenSize(1.0f);
        c.strokeRoundRect(r, geo::kComboRadius);

        const bool light = mMode == Mode::Light;
        const Rect iconBox(r.x + 7.0f, r.centerY() - 7.0f, 14.0f, 14.0f);
        c.setColor(mPalette.text);
        drawIcon(c, light ? Icon::Sun : Icon::Moon, iconBox, mPalette.text);
        if (!light) {
            // The crescent: punch the disc with the well colour it is drawn on. drawIcon draws a
            // full moon, because only the caller knows what is behind it.
            c.setColor(mPalette.input);
            c.fillEllipse(iconBox.centerX() + 3.0f, iconBox.centerY() - 2.0f, 5.5f, 5.5f);
        }

        c.setFont(Font::Body);
        c.setFontSize(geo::kBarTextSize);
        c.setColor(mPalette.text);
        const float tx = iconBox.right() + 6.0f;
        const std::string s = c.clipToWidth(light ? "Light" : "Dark", r.right() - tx - 8.0f);
        c.drawString(s.c_str(), tx, r.centerY() + geo::kBarTextSize * geo::kLabelBaselineBias);
    }

    // Popups last: they are the only thing allowed to paint outside their own section.
    mDeviceCombo.drawPopup(c, mPalette);
    mAccentCombo.drawPopup(c, mPalette);
}

// --- input ------------------------------------------------------------------

Panel::Target Panel::targetAt(float x, float y, int &index) const
{
    index = -1;
    if (!mFatalError.empty())
        return Target::Nothing;

    for (size_t i = 0; i < mStrips.size(); ++i) {
        if (mStrips[i].hasVolume && mStrips[i].slider.hit(x, y)) {
            index = static_cast<int>(i);
            return Target::Slider;
        }
        if (mStrips[i].hasSwitch && mStrips[i].button.hit(x, y)) {
            index = static_cast<int>(i);
            return Target::MuteButton;
        }
    }
    if (mDeviceCombo.hitClosed(x, y))
        return Target::DeviceCombo;
    for (int i = 0; i < 3; ++i) {
        if (mRadios[i].hit(x, y)) {
            index = i;
            return Target::Radio;
        }
    }
    for (size_t i = 0; i < mSwitches.size(); ++i) {
        if (mSwitches[i].hit(x, y)) {
            index = static_cast<int>(i);
            return Target::Switch;
        }
    }
    if (mAccentCombo.hitClosed(x, y))
        return Target::AccentCombo;
    if (mThemeToggleRect.contains(x, y))
        return Target::ThemeToggle;
    return Target::Nothing;
}

Panel::HoverState Panel::hoverState() const
{
    HoverState h;
    for (size_t i = 0; i < mStrips.size(); ++i) {
        if (mStrips[i].slider.hovered)
            h.slider = static_cast<int>(i);
        if (mStrips[i].button.hovered)
            h.mute = static_cast<int>(i);
    }
    for (int i = 0; i < 3; ++i)
        if (mRadios[i].hovered)
            h.radio = i;
    for (size_t i = 0; i < mSwitches.size(); ++i)
        if (mSwitches[i].hovered)
            h.sw = static_cast<int>(i);
    h.device = mDeviceCombo.hovered;
    h.accent = mAccentCombo.hovered;
    h.theme = mThemeHovered;
    return h;
}

void Panel::clearHover()
{
    for (Strip &s : mStrips) {
        s.slider.hovered = false;
        s.button.hovered = false;
    }
    for (int i = 0; i < 3; ++i)
        mRadios[i].hovered = false;
    for (Toggle &t : mSwitches)
        t.hovered = false;
    mDeviceCombo.hovered = false;
    mAccentCombo.hovered = false;
    mThemeHovered = false;
}

void Panel::motion(float x, float y)
{
    // A drag owns the pointer: the value follows it even outside the slider's own rect, which is
    // what makes dragging past the end of the groove hold at 100 instead of snapping back.
    if (mDragStrip >= 0 && mDragStrip < static_cast<int>(mStrips.size())) {
        Strip &s = mStrips[static_cast<size_t>(mDragStrip)];
        const int v = s.slider.valueAt(x);
        if (v != s.slider.value) {
            s.slider.value = v;
            if (cb.volume)
                cb.volume(mDragStrip, v);
            if (onNeedsRepaint)
                onNeedsRepaint();
        }
        return;
    }

    // Snapshot what is lit, so a repaint is asked for only when it actually changes. A pointer
    // travelling across the window otherwise costs one full recompose per motion event.
    const HoverState before = hoverState();

    clearHover();

    // A popup is modal for the pointer as well as the keyboard: nothing behind it lights up.
    if (mDeviceCombo.isOpen()) {
        mDeviceCombo.motion(x, y);
        if (onNeedsRepaint)
            onNeedsRepaint();
        return;
    }
    if (mAccentCombo.isOpen()) {
        mAccentCombo.motion(x, y);
        if (onNeedsRepaint)
            onNeedsRepaint();
        return;
    }

    int index = -1;
    switch (targetAt(x, y, index)) {
        case Target::Slider:
            mStrips[static_cast<size_t>(index)].slider.hovered = true;
            break;
        case Target::MuteButton:
            mStrips[static_cast<size_t>(index)].button.hovered = true;
            break;
        case Target::Radio:
            mRadios[index].hovered = true;
            break;
        case Target::Switch:
            mSwitches[static_cast<size_t>(index)].hovered = true;
            break;
        case Target::DeviceCombo:
            mDeviceCombo.hovered = true;
            break;
        case Target::AccentCombo:
            mAccentCombo.hovered = true;
            break;
        case Target::ThemeToggle:
            mThemeHovered = true;
            break;
        case Target::Nothing:
            break;
    }

    if (!(hoverState() == before) && onNeedsRepaint)
        onNeedsRepaint();
}

void Panel::press(float x, float y)
{
    mPressTarget = Target::Nothing;
    mPressIndex = -1;

    // An open popup gets the click, and swallows it even when it lands outside -- the dismissal
    // must not also press whatever it happened to land on.
    if (mDeviceCombo.isOpen()) {
        mDeviceCombo.click(x, y);
        return;
    }
    if (mAccentCombo.isOpen()) {
        mAccentCombo.click(x, y);
        return;
    }

    int index = -1;
    const Target t = targetAt(x, y, index);
    mPressTarget = t;
    mPressIndex = index;

    // A slider is the one control that acts on PRESS, because it has to: a drag has no meaning if
    // the first movement is ignored until the button comes back up.
    if (t == Target::Slider) {
        mDragStrip = index;
        Strip &s = mStrips[static_cast<size_t>(index)];
        s.slider.dragging = true;
        const int v = s.slider.valueAt(x);
        if (v != s.slider.value) {
            s.slider.value = v;
            if (cb.volume)
                cb.volume(index, v);
        }
    }
}

void Panel::release(float x, float y)
{
    if (mDragStrip >= 0) {
        if (mDragStrip < static_cast<int>(mStrips.size()))
            mStrips[static_cast<size_t>(mDragStrip)].slider.dragging = false;
        mDragStrip = -1;
        mPressTarget = Target::Nothing;
        return;
    }

    int index = -1;
    const Target t = targetAt(x, y, index);

    // Only act if the release is on the same thing the press was.
    if (t != mPressTarget || index != mPressIndex) {
        mPressTarget = Target::Nothing;
        mPressIndex = -1;
        return;
    }
    mPressTarget = Target::Nothing;
    mPressIndex = -1;

    switch (t) {
        case Target::MuteButton: {
            Strip &s = mStrips[static_cast<size_t>(index)];
            s.button.on = !s.button.on;
            if (cb.mute)
                cb.mute(index, s.button.on);
            break;
        }
        case Target::Radio:
            if (mRadios[index].enabled) {
                setRouting(static_cast<Routing>(index));
                if (cb.routing)
                    cb.routing(static_cast<Routing>(index));
            }
            break;
        case Target::Switch: {
            Toggle &t2 = mSwitches[static_cast<size_t>(index)];
            t2.on = !t2.on;
            if (cb.switchToggled)
                cb.switchToggled(index, t2.on);
            break;
        }
        case Target::DeviceCombo:
            mDeviceCombo.choose = [this](int i) {
                if (cb.device)
                    cb.device(i);
            };
            mDeviceCombo.open(Rect(0, 0, geo::kWinW, mHeight));
            break;
        case Target::AccentCombo:
            mAccentCombo.choose = [this](int i) {
                if (cb.accent)
                    cb.accent(i);
            };
            mAccentCombo.open(Rect(0, 0, geo::kWinW, mHeight));
            break;
        case Target::ThemeToggle:
            if (cb.toggleTheme)
                cb.toggleTheme();
            break;
        case Target::Slider:
        case Target::Nothing:
            break;
    }
}

bool Panel::key(Key k)
{
    if (mDeviceCombo.isOpen())
        return mDeviceCombo.key(k);
    if (mAccentCombo.isOpen())
        return mAccentCombo.key(k);
    return false;
}

} // namespace audiogui
