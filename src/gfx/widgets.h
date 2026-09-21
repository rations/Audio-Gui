// This window's controls.
//
// NOT the sibling CPU-Power project's widgets, and simple-login-gui's ink.h already records why
// they could not be ported: none of CPU-Power's controls carries a rect, and every draw() and
// hit() reads absolute constants out of that project's geometry.h. That works for a panel with
// one fixed arrangement. It cannot work here, because Audio-Gui builds its mixer strips and
// switch checkboxes from whatever ALSA elements the selected device exposes, and destroys and
// rebuilds them every time the device changes.
//
// SO EVERY CONTROL HERE CARRIES ITS RECT, and hit-tests against the same rect it draws, which is
// what stops the painter and the hit test drifting apart when the layout is computed at run time.
// The panel positions them; they do not know where they are until it does.
//
// What DOES carry over is the drawing idiom in ink.h -- a well fill, an optional accent wash, and
// a 1px inkFor() outline whose alpha is the entire hover treatment -- so a control drawn here
// reads as a sibling of one drawn there.
//
// ICONS ARE DRAWN, NEVER TYPED. The Qt build put emoji in button text (a speaker, a crossed-out
// speaker, a filled and hollow dot, a sun and a moon). That needs a colour-emoji face through
// FreeType, and neither bundled font has any of those glyphs, so each one is a few strokes here
// instead -- the same rule ink.h already states for a checkmark.

#pragma once

#include "canvas.h"
#include "palette.h"

#include <string>

namespace audiogui
{

enum class Icon {
    SpeakerOn,    // playback element, not muted
    SpeakerMuted, // playback element, muted
    CaptureOn,    // capture element, enabled
    CaptureOff,   // capture element, disabled
    Sun,          // light mode
    Moon,         // dark mode
};

// Draws one icon centred in `r`, in `rgb`. Exposed because the appearance bar's mode toggle
// draws a sun or a moon beside its label rather than inside a button of its own.
//
// The colour is a PARAMETER rather than whatever was last set on the canvas, because
// Canvas::strokeArc takes its own colour argument and sets it -- an icon that mixed arcs with
// fills and trusted the canvas state would silently draw half of itself in the wrong colour.
void drawIcon(Canvas &c, Icon icon, const Rect &r, uint32_t rgb);

// A group box: the @SURFACE card, its 1px @BORDER frame at radius 10, and the @SUBTEXT title
// sitting in the band above it. `frame` is the card, NOT including the title band.
void drawGroupBox(Canvas &c, const Palette &p, const Rect &frame, const char *title);

// One line of text, baseline-positioned within `row` by ink.h's kLabelBaselineBias, clipped to
// the row's width with an ellipsis if it does not fit.
void drawRowText(Canvas &c, const Palette &p, const Rect &row, const char *text, uint32_t rgb,
                 float size);

// --- The volume slider ------------------------------------------------------
// 0..100, matching the range AlsaMixer already converts the element's raw range into.
struct Slider {
    Rect rect; // the groove's full travel, thumb centres at [rect.left(), rect.right()]
    int value = 0;
    bool enabled = true;
    bool hovered = false;
    bool dragging = false;

    void draw(Canvas &c, const Palette &p) const;

    // The hit rect is the groove grown vertically to the thumb's height, because a 6-unit groove
    // is not a target. Grown horizontally too, by the thumb radius, so the ends are reachable.
    bool hit(float x, float y) const;

    // The value the thumb CENTRE lands on for a pointer at x, clamped. Mapping the pointer to
    // the centre rather than the left edge is what stops the thumb jumping half its width on
    // the first press of a drag.
    int valueAt(float x) const;
};

// --- The mute / capture button at the end of a strip ------------------------
struct IconButton {
    Rect rect;
    Icon iconOn = Icon::SpeakerOn;
    Icon iconOff = Icon::SpeakerMuted;
    bool on = false;
    bool enabled = true;
    bool hovered = false;

    void draw(Canvas &c, const Palette &p) const;
    bool hit(float x, float y) const;
};

// --- Checkbox and radio -----------------------------------------------------
// One type, two indicator shapes: a 16x16 box at radius 4, or a circle of the same size. They
// differ in nothing else, and splitting them into two structs would duplicate the label
// handling and the hit rect for the sake of one branch.
struct Toggle {
    enum class Shape { Check, Radio };

    Rect rect; // the whole clickable row, indicator and label together
    std::string label;
    Shape shape = Shape::Check;
    bool on = false;
    bool enabled = true;
    bool hovered = false;

    void draw(Canvas &c, const Palette &p) const;
    bool hit(float x, float y) const;

    // Where the text starts, so the layout audit can measure the label against the room it has.
    float textX() const;
    float textMaxW() const;
};

} // namespace audiogui
