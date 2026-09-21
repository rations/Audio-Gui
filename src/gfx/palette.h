// The palette, in one place.
//
// The sibling projects (CPU-Power, simple-login-gui) declare their colours as a constexpr `pal::`
// namespace, because each of them has exactly one look. This one CANNOT: Audio-Gui has a live
// dark/light toggle and four user-selectable accents, and those are features of the program
// rather than artifacts of the Qt GUI being removed. So the tokens are a value type resolved at
// run time, and every draw call takes the live one.
//
// THE COLOURS ARE THE ONES THE Qt BUILD SHIPPED, carried across unchanged from Theme.cpp's
// paletteFor() and accentPair() so that removing Qt does not silently restyle the program. What
// changed is how they are APPLIED -- the ~150-line Qt stylesheet is gone, and the drawing idiom
// in ink.h took its place.
//
// 0xRRGGBB throughout, matching Canvas::setColor. Alpha is a separate argument to setColor and is
// never baked into a constant here.

#pragma once

#include <cstdint>

namespace audiogui
{

enum class Mode { Dark = 0, Light = 1 };
enum class Accent { Green = 0, Orange = 1, Blue = 2, Yellow = 3 };

inline constexpr int kModeCount = 2;
inline constexpr int kAccentCount = 4;

struct Palette {
    // --- per-mode tokens (Theme.cpp paletteFor) ---
    uint32_t window;  // app background, and the level meter's ground
    uint32_t surface; // group-box cards
    uint32_t border;  // 1px borders and hairlines
    uint32_t text;    // body text
    uint32_t subtext; // group-box titles, secondary text
    uint32_t input;   // slider groove, unchecked indicators, and the meter's empty track
    uint32_t tipBg;
    uint32_t tipText;

    // --- the live accent (Theme.cpp accentPair) ---
    uint32_t accent;
    uint32_t accentBright; // the meter's cap line; a lighter shade of accent
    uint32_t onAccent;     // text drawn ON the accent -- dark for yellow, white otherwise

    // A control that cannot be used right now is drawn in this and left visible, never hidden:
    // it has to read as "not available", not as "absent". ink.h routes every control's ink
    // through inkFor() so this can never be one colour on one control and another on the next.
    uint32_t disabled;

    // The meter's peak-hold marker. Deliberately NOT the accent: it has to stay legible sitting
    // directly on top of the accent-coloured fill it marks the high-water point of.
    uint32_t peak;

    // Piping and hairlines, sampled from the rations-amp plug-in art and carried unchanged
    // through CPU-Power and simple-login-gui -- which is the whole point of it being here: it is
    // the one colour every project in the family shares, so a rule drawn in it reads as the same
    // rule wherever you see it. Always drawn at an alpha, never at full strength.
    uint32_t gold;
};

// Theme.cpp built its filled-slider gradient from QColor::lighter(155), which scales the HSV
// VALUE and leaves hue and saturation alone. There is no gradient any more -- the house idiom
// is a flat accent wash -- but the same shade is still wanted for the meter's cap line, so the
// one operation Qt was doing is reproduced here rather than the result being hardcoded per
// accent, which would silently stop tracking if an accent were ever retuned.
uint32_t lighter(uint32_t rgb, int percent);

Palette paletteFor(Mode m, Accent a);

// The swatch colour and the name shown in the accent picker.
uint32_t accentColor(Accent a);
const char *accentName(Accent a);
const char *modeName(Mode m);

} // namespace audiogui
