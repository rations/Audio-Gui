// Audio-Gui window geometry.
//
// EVERYTHING HERE IS IN LOGICAL UNITS. The window applies one cairo_scale(s, s) at compose time
// and divides mouse coordinates by s before hit-testing, so a scale factor must never be baked
// into a constant here. Change the scale and every number below stays exactly as it is.
//
// WHERE THIS DIFFERS FROM THE SIBLING PROJECTS. CPU-Power's geometry.h can give every control an
// absolute rectangle, because its panel is one fixed arrangement -- which is also why its widgets
// do not carry rects and could not be reused here (simple-login-gui's ink.h records that at
// length). Audio-Gui's content is DATA-DRIVEN: the mixer strips and the switch checkboxes are
// rebuilt from whatever ALSA elements the selected device actually exposes. So this file holds
// the fixed frame -- margins, row heights, radii, text sizes -- plus constexpr functions that
// compute a section's height from a count, and the panel runs one layout pass to stack them.
//
// The counts are BOUNDED, which is what keeps the compile-time checks meaningful: AlsaMixer's
// kWanted[] is a fixed 7-entry table, so there can never be more than kMaxStrips volume strips or
// kMaxSwitches switch checkboxes. The window is therefore never taller than kWinHMax, and the
// static_asserts at the bottom hold against the worst case rather than against a typical one.
//
// Text is a different problem -- a string's width depends on the font, which is not a
// compile-time quantity -- so strings are measured against their real slots at run time by
// tools/uirender, which fails if any of them overflows.

#pragma once

namespace audiogui
{
namespace geo
{

// --- The window -------------------------------------------------------------
// 440 is the width the Qt build shipped (main.cpp: window.resize(440, 460)) and it is kept: it
// is the width that makes "HDA Intel PCH — HDA Analog" fit the device combo without eliding,
// which is the longest string the window has no control over.
constexpr float kWinW = 440.0f;
constexpr float kMargin = 14.0f;
constexpr float kContentW = kWinW - 2.0f * kMargin;

// The scales the layout is audited at. 0.75 is a 1024x768 laptop, 2.0 a HiDPI panel.
constexpr float kScaleMin = 0.75f;
constexpr float kScaleMax = 2.0f;

// --- Text -------------------------------------------------------------------
// 13 is what the Qt stylesheet set globally (`* { font-size: 13px; }`) and is kept so the window
// reads at the same density it always has.
constexpr float kBodySize = 13.0f;
constexpr float kGroupTitleSize = 13.0f;
constexpr float kMeterLabelSize = 12.0f;
constexpr float kBarTextSize = 12.0f;

// A label's baseline sits this far below the centre of its row. From simple-login-gui's ink.h,
// which measured it: a cap height is about 0.72 em, so half of it below the centre is 0.36.
constexpr float kLabelBaselineBias = 0.36f;

// --- Group boxes ------------------------------------------------------------
// The Qt stylesheet drew these as a 1px @BORDER frame at radius 10 over an @SURFACE fill, with
// the title sitting in a 16px margin above the frame, inset 12 from the left. Same shape here,
// except the title is drawn above the frame rather than punched into it -- a gap in a stroked
// rounded rect costs a path split, and nothing about the look needs one.
constexpr float kGroupRadius = 10.0f;
constexpr float kGroupTitleH = 16.0f;
constexpr float kGroupTitleX = 12.0f;
constexpr float kGroupPadX = 14.0f;
constexpr float kGroupPadTop = 12.0f;
constexpr float kGroupPadBottom = 10.0f;
constexpr float kGroupInnerW = kContentW - 2.0f * kGroupPadX;

// Vertical space between one section and the next.
constexpr float kSectionGap = 10.0f;

// Total height of a group box whose contents are innerH tall, including its title band.
constexpr float groupH(float innerH)
{
    return kGroupTitleH + kGroupPadTop + innerH + kGroupPadBottom;
}

// --- Mixer strips -----------------------------------------------------------
constexpr int kMaxStrips = 5;   // Master, Headphone, Speaker, Microphone, Mic Boost
constexpr int kMaxSwitches = 2; // Capture, IEC958

constexpr float kStripH = 30.0f;
constexpr float kStripLabelW = 110.0f;
constexpr float kStripGap = 8.0f;

// The mute/capture button at the right end of a strip. It was a QToolButton carrying an emoji
// glyph; it is now drawn, because a glyph taken from the body font depends on that font having
// one and neither bundled face has a speaker.
constexpr float kMuteW = 30.0f;
constexpr float kMuteH = 22.0f;
constexpr float kMuteRadius = 6.0f;

// Slider, from the Qt stylesheet: a 6px groove at radius 3 with a 16x16 handle at radius 8.
constexpr float kGrooveH = 6.0f;
constexpr float kGrooveRadius = 3.0f;
constexpr float kThumbR = 8.0f;

constexpr float kSliderX = kStripLabelW + kStripGap;
constexpr float kSliderW = kGroupInnerW - kSliderX - kStripGap - kMuteW;

// The placeholder shown instead of strips for a device that exposes none (USB and HDMI, which
// share a card with the internal codec on sof-hda-dsp and would otherwise show its controls).
constexpr float kPlaceholderH = 28.0f;

// --- Level meter ------------------------------------------------------------
// Two horizontal bars, L over R. The Qt build sized the widget 240x36 with a 28 minimum; the
// bars here span the group's inner width, which is wider and reads better at this aspect.
constexpr float kMeterLabelH = 18.0f;
constexpr float kMeterH = 34.0f;
constexpr float kMeterRadius = 4.0f;
constexpr float kMeterWellInset = 3.0f; // the track, inside the well's border
constexpr float kMeterBarGap = 3.0f;
constexpr float kMeterTopGap = 6.0f; // between the last strip and the "Output level" label

// The ladder rungs that make a continuous fill read as segments, from rations-amp's drawMeter.
//
// rations uses a 3-unit pitch, and that number did not survive the move: its meter is 27 units
// wide, so 3 gives it about nine segments, while this bar is kGroupInnerW wide and the same pitch
// gave 128 -- which at 1x stops reading as segments at all and turns into a hatched texture.
// Compared 3 / 4 / 6 / 8 / 10 side by side at 1x; 8 is the first that reads as discrete lit
// segments, and gives 48 of them across the bar.
constexpr float kRungPitch = 8.0f;
constexpr float kRungW = 1.0f;
// The lit tip at the fill's leading edge, and the peak-hold marker.
constexpr float kMeterCapW = 1.0f;
constexpr float kMeterPeakW = 2.0f;

constexpr float meterBarH()
{
    return (kMeterH - kMeterBarGap) / 2.0f;
}

// --- Combo boxes ------------------------------------------------------------
// Qt: 1px border at radius 8, padding 4px 10px, min-height 22, drop-down 22 wide.
constexpr float kComboH = 28.0f;
constexpr float kComboRadius = 8.0f;
constexpr float kComboPadX = 10.0f;
constexpr float kComboArrowW = 22.0f;

// The popup list. Row geometry follows simple-login-gui's Menu, which is where the placement
// logic comes from.
constexpr float kPopupRowH = 26.0f;
constexpr float kPopupPadY = 5.0f;
constexpr float kPopupRadius = 6.0f;
constexpr float kPopupGap = 4.0f;
constexpr float kPopupTextSize = 12.0f;
constexpr float kPopupTickW = 16.0f; // the gutter holding the current-item tick

constexpr float popupH(int rows)
{
    return 2.0f * kPopupPadY + static_cast<float>(rows) * kPopupRowH;
}

// --- Radio buttons and checkboxes -------------------------------------------
// Qt: 16x16 indicators, checkbox at radius 4 and radio at radius 9 (a circle), 8px between the
// indicator and its text.
constexpr int kRoutingModeCount = 3;
constexpr float kRowH = 24.0f;
constexpr float kIndicatorSize = 16.0f;
constexpr float kIndicatorGap = 8.0f;
constexpr float kCheckRadius = 4.0f;
constexpr float kRadioRadius = kIndicatorSize / 2.0f;
// Hit rects are deliberately larger than the ink: a 16-unit box is not a target.
constexpr float kRowHitH = kRowH;
constexpr float kSwitchTopGap = 6.0f; // between the last radio and the first switch checkbox

// --- The appearance bar -----------------------------------------------------
// Not in a group box: it is window chrome rather than a setting about audio.
constexpr float kBarH = 28.0f;
constexpr float kBarGap = 8.0f;
constexpr float kAccentComboW = 132.0f;
constexpr float kModeToggleW = 96.0f;
constexpr float kSwatch = 12.0f; // the accent swatch drawn in the combo, was a 12x12 QPixmap
constexpr float kSwatchRadius = 3.0f;

// --- Section heights --------------------------------------------------------
constexpr float mixerInnerH(int strips, bool showMeter)
{
    const float rows = strips > 0 ? static_cast<float>(strips) * kStripH : kPlaceholderH;
    const float meter = showMeter ? kMeterTopGap + kMeterLabelH + kMeterH : 0.0f;
    return rows + meter;
}

constexpr float routingInnerH(int switches)
{
    const float radios = static_cast<float>(kRoutingModeCount) * kRowH;
    const float sw = switches > 0 ? kSwitchTopGap + static_cast<float>(switches) * kRowH : 0.0f;
    return radios + sw;
}

constexpr float deviceInnerH()
{
    return kComboH;
}

constexpr float windowH(int strips, int switches, bool showMeter)
{
    return kMargin + groupH(mixerInnerH(strips, showMeter)) + kSectionGap + groupH(deviceInnerH()) +
           kSectionGap + groupH(routingInnerH(switches)) + kSectionGap + kBarH + kMargin;
}

// The extremes the window can actually take, both reachable: the maximum is a machine whose
// codec exposes every element kWanted[] asks for, the minimum a USB device in pure-ALSA mode.
constexpr float kWinHMax = windowH(kMaxStrips, kMaxSwitches, true);
constexpr float kWinHMin = windowH(0, 0, false);

// --- Clearances -------------------------------------------------------------
// A comment claiming two things do not overlap stops being true silently the moment somebody
// nudges a constant; a static_assert fails the build instead.

static_assert(kSliderW > 120.0f, "the volume slider has been squeezed below a usable length by "
                                 "the strip label or the mute button");
static_assert(kSliderX + kSliderW + kStripGap + kMuteW <= kGroupInnerW,
              "a mixer strip's contents are wider than the group box holding them");
static_assert(kStripH >= 2.0f * kThumbR + 4.0f,
              "the slider thumb is taller than the row it sits in, so adjacent strips will touch");
static_assert(kMuteH <= kStripH, "the mute button is taller than its strip row");

static_assert(meterBarH() > kMeterPeakW + 2.0f * kMeterWellInset,
              "a meter bar is too short to show its own peak marker");
static_assert(kRungPitch > kRungW, "the meter's ladder rungs touch, so the fill reads as solid "
                                   "black rather than as segments");

static_assert(kIndicatorSize < kRowH, "a checkbox indicator is taller than its row");
static_assert(kRadioRadius * 2.0f == kIndicatorSize,
              "the radio is no longer a circle inscribed in the indicator box");

static_assert(kBarGap + kAccentComboW + kBarGap + kModeToggleW < kContentW,
              "the appearance bar's controls no longer fit across the window");
static_assert(kSwatch < kBarH, "the accent swatch is taller than the bar it is drawn in");

static_assert(kComboArrowW + 2.0f * kComboPadX < kGroupInnerW,
              "the device combo has no room left for its text");
static_assert(kPopupTickW < kAccentComboW, "the popup's tick gutter is wider than the popup");

// The window has to stay usable on the smallest panel the scale range is audited for: 768 rows
// at kScaleMin leaves 1024 logical units, and the tallest the window can get must fit inside it.
static_assert(kWinHMax < 1024.0f, "the fully-populated window is too tall for a 768-row display "
                                  "at the minimum audited scale");
static_assert(kWinHMin < kWinHMax, "the window's height bounds are inverted");

} // namespace geo
} // namespace audiogui
