// The one genuinely reusable piece of the sibling CPU-Power window's widget file, plus the
// drawing idiom its controls share, written down so this project's controls look like that
// project's without copying controls that cannot be copied.
//
// Taken from simple-login-gui, which had already extracted it for the same reason. The one
// change here is that inkFor() takes a Palette: see the note on it below.
//
// WHY THERE IS NO PORTED widgets.cpp HERE. CPU-Power's Slider, Toggle, Checkbox and Readout are
// not parameterised widgets: none of them carries a rect, and every draw() and hit() reads
// absolute constants out of that project's geometry.h -- Toggle::draw() starts with
// `Rect r(geo::kToggleX, geo::kToggleY, geo::kToggleW, geo::kToggleH)`. They are that panel's
// specific controls, and this panel has none of the same ones. Bringing them across would mean
// bringing across a geometry header describing a 420x285 window with a five-stop governor
// slider, which is not this window. So the controls are written for this panel's geometry, and
// what crosses over is this file: the state-to-colour rule, and the conventions below that make
// a control drawn here read as a sibling of one drawn there.
//
// THE IDIOM, as measured from CPU-Power's widgets.cpp rather than remembered:
//
//   * A control is a kWellColor rounded fill, then (when it is "on") the accent at alpha 190
//     over it, then a 1px outline in inkFor() at ALPHA 255 WHEN HOVERED AND 170 OTHERWISE. That
//     alpha step is the entire hover treatment -- there is no separate hover fill, and there is
//     no pressed state at all.
//   * Corner radius is 3.0 for a box, and exactly half the height for anything meant to read as
//     a capsule.
//   * Pen size is 1.0 for outlines and hairlines, 2.0 for a checkmark or a thumb ring.
//   * A tick is drawn as two strokeLine() calls, never as a glyph: a checkmark taken from the
//     body font depends on that font having one.
//   * A label's baseline sits at r.centerY() + fontSize * 0.36f.
//   * Hit rects are deliberately larger than the ink, because a 15-unit box is not a target.
//
// Canvas already pins the half-pixel offset that makes a 1px stroke land ON the boundary rather
// than straddling two pixel rows, so none of the above has to think about it.

#pragma once

#include "palette.h"

#include <cstdint>

namespace audiogui
{

// The state-to-colour rule, ported from CPU-Power's widgets.cpp.
//
// Disabled outranks on: a control that cannot be used does not also advertise what it would
// have been set to. Everything that draws a control's ink goes through here, so "disabled" can
// never be drawn in one colour by one control and another colour by the next.
//
// It takes the palette rather than reading a constexpr `pal::` namespace like the sibling
// projects do, because this program's colours change under the user's dark/light and accent
// controls. See palette.h.
inline uint32_t inkFor(const Palette &p, bool enabled, bool on)
{
    if (!enabled)
        return p.disabled;
    return on ? p.accent : p.subtext;
}

// The two alphas that are the whole hover treatment. Named so a control cannot quietly invent a
// third.
constexpr int kOutlineAlphaHover = 255;
constexpr int kOutlineAlphaIdle = 170;

// The accent wash laid over a control that is on, under its outline.
constexpr int kOnFillAlpha = 190;

// A group box's frame, in the ACCENT rather than the neutral border. Lower than a control's
// outline on purpose: a frame that competes with the controls inside it has stopped being a
// frame. Measured against three stacked boxes, which is the worst case this window has.
constexpr int kFrameAlpha = 120;

// The gold piping. Faint: it is a seam, not a line anybody should read.
constexpr int kHairlineAlpha = 70;

// The meter well's gold rim. Stronger than a hairline, because this one IS meant to be read:
// it is the edge that tells you where full scale is when the bar is nowhere near it.
constexpr int kMeterWellAlpha = 120;

} // namespace audiogui
