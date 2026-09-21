// See palette.h.

#include "palette.h"

#include <algorithm>

namespace audiogui
{
namespace
{

struct AccentSpec {
    uint32_t base;
    bool darkText; // true when the accent is light enough to need dark text on it
    const char *name;
};

// Theme.cpp accentPair(), verbatim. Its `hover` field is dropped: it was declared and then never
// substituted into any stylesheet rule, so carrying it over would carry over a value that has
// never been seen on screen.
constexpr AccentSpec kAccents[kAccentCount] = {
    {0x2ECC71, false, "Green"},
    {0xE67E22, false, "Orange"},
    {0x3498DB, false, "Blue"},
    {0xF1C40F, true, "Yellow"},
};

const AccentSpec &specFor(Accent a)
{
    const int i = static_cast<int>(a);
    return kAccents[(i >= 0 && i < kAccentCount) ? i : 0];
}

} // namespace

// Qt's QColor::lighter(f) converts to HSV, multiplies the VALUE by f/100, clamps, and converts
// back. Hue and saturation are untouched, which is why it lightens without washing out to grey
// the way a straight blend toward white does.
uint32_t lighter(uint32_t rgb, int percent)
{
    const int r = static_cast<int>((rgb >> 16) & 0xFF);
    const int g = static_cast<int>((rgb >> 8) & 0xFF);
    const int b = static_cast<int>(rgb & 0xFF);

    const int vMax = std::max({r, g, b});
    const int vMin = std::min({r, g, b});
    if (vMax == 0)
        return rgb; // black has no value to scale

    const int scaled = std::min(255, vMax * percent / 100);
    if (scaled == vMax)
        return rgb;

    // Saturation is (vMax - vMin) / vMax and must be preserved, so the distance of each channel
    // below the maximum scales by exactly the same factor the maximum did. Working in integers
    // with the rounding term keeps the hue from drifting a unit on the mid channel.
    const int span = vMax - vMin;
    const int newMin = span == 0 ? scaled : scaled - (span * scaled + vMax / 2) / vMax;

    auto remap = [&](int c) -> int {
        if (span == 0)
            return scaled;
        const int t = ((c - vMin) * (scaled - newMin) + span / 2) / span;
        return std::clamp(newMin + t, 0, 255);
    };

    return (static_cast<uint32_t>(remap(r)) << 16) | (static_cast<uint32_t>(remap(g)) << 8) |
           static_cast<uint32_t>(remap(b));
}

Palette paletteFor(Mode m, Accent a)
{
    const AccentSpec &ac = specFor(a);

    Palette p{};
    if (m == Mode::Dark) {
        // Theme.cpp paletteFor(Mode::Dark)
        p.window = 0x1E2228;
        p.surface = 0x262B33;
        p.border = 0x333A44;
        p.text = 0xE6E9EE;
        p.subtext = 0x9AA3AD;
        p.input = 0x3A4250;
        p.tipBg = 0x11141A;
        p.tipText = 0xE6E9EE;
        // Dimmer than subtext, so "disabled" and "secondary" cannot be confused for each other.
        p.disabled = 0x5A626C;
    } else {
        // Theme.cpp paletteFor(Mode::Light)
        p.window = 0xF4F6F8;
        p.surface = 0xFFFFFF;
        p.border = 0xD8DDE3;
        p.text = 0x1F2329;
        p.subtext = 0x5B6470;
        p.input = 0xD3D8DE;
        p.tipBg = 0x2B2F36;
        p.tipText = 0xF4F6F8;
        p.disabled = 0xA8B0B9;
    }

    p.accent = ac.base;
    p.accentBright = lighter(ac.base, 155);
    p.onAccent = ac.darkText ? 0x1F2329 : 0xFFFFFF;

    // One colour in both modes: the peak marker sits on the accent fill, not on the page, so it
    // is the fill it has to stay legible against rather than the background.
    p.peak = 0xFF5A4D;

    // The family's gold. Identical in both modes -- it is a pigment, not a role, and the alpha
    // it is drawn at is what adapts it to the ground underneath.
    p.gold = 0xB88B4C;

    return p;
}

uint32_t accentColor(Accent a)
{
    return specFor(a).base;
}

const char *accentName(Accent a)
{
    return specFor(a).name;
}

const char *modeName(Mode m)
{
    return m == Mode::Dark ? "Dark" : "Light";
}

} // namespace audiogui
