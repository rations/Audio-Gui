// The output level meter: two horizontal bars, L over R.
//
// THIS FILE HOLDS NO IPC. The peaks come from a POSIX shared-memory page the running bridge
// publishes, and reading that page is src/peaksource.h's job. Keeping the two apart is what lets
// tools/uirender draw a meter at a chosen level with no bridge running and no X server -- and a
// meter is exactly the thing you cannot check by looking at a still window, because what is wrong
// with it is usually its motion.
//
// WHERE THE DESIGN COMES FROM. The Qt build drew two rounded bars filled with a screen-space
// green-yellow-red QLinearGradient. That gradient is gone with Qt: Canvas has no gradient API in
// any project in this family, and rations-amp's RationsEditorView::drawMeter
// (products/rations/src/rationsview.cpp) already solves the same problem without one --
//
//   * a flat accent fill for the level;
//   * 1px rungs punched through it AT FIXED POSITIONS, so a continuous fill reads as segments
//     without per-segment logic and without the segments sliding as the level moves;
//   * a 1px bright cap line at the fill's leading edge, which is what gives the "lit tip" read;
//   * a peak-hold marker that holds about a second and then falls.
//
// Rotated from vertical to horizontal, and the rungs are drawn in the WINDOW colour rather than
// rations' black, so they read as gaps punched through to the ground in light mode as well.
//
// THE LEVEL SCALE IS LINEAR, deliberately. rations-amp maps -70..0 dB before it ever reaches the
// view; the bridges here publish a linear block peak and the Qt meter drew it linearly, so
// introducing dB would make every signal read louder than it used to. That is a change to make
// on purpose, not one to smuggle in with a toolkit swap.

#pragma once

#include "canvas.h"
#include "palette.h"

namespace audiogui
{

// Ballistics, at the 30 fps tick. From rations-amp's rationsview.cpp, which tuned them against a
// real signal: instant attack, exponential release, and a peak marker that holds before falling.
inline constexpr float kMeterRelease = 0.944f;
inline constexpr float kPeakRelease = 0.985f;
inline constexpr int kPeakHoldTicks = 30;

// Below this, a bar is treated as silent and stops asking for repaints. Without it a 33 ms tick
// repaints the whole window thirty times a second forever, including in silence.
inline constexpr float kMeterIdle = 0.002f;

struct LevelMeter {
    Rect rect;

    float dispL = 0.0f, dispR = 0.0f;
    float peakL = 0.0f, peakR = 0.0f;
    int holdL = 0, holdR = 0;

    // Advance exactly one frame. `have` is false when the bridge published nothing new since the
    // last frame, which is also what a stopped bridge looks like.
    //
    // The release runs BEFORE the new sample is taken, not after: a steady signal then sits at
    // its own value instead of sawtoothing one release-step below it every frame.
    //
    // Returns true while anything is still moving, so the caller can stop invalidating the
    // window once the meter has settled to silence.
    bool advance(bool have, float l, float r);

    // Drop to silence immediately -- the bridge went away, or the meter was hidden.
    void reset();

    void draw(Canvas &c, const Palette &p) const;
};

} // namespace audiogui
