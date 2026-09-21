// See widgets.h.

#include "widgets.h"

#include "../geometry.h"
#include "ink.h"

#include <algorithm>
#include <cmath>

namespace audiogui
{
namespace
{

// The well every control is drawn on: the Qt stylesheet used @INPUT for a slider groove, an
// unchecked indicator and a tool button alike, and keeping that one token here is what makes
// them read as the same material.
void fillWell(Canvas &c, const Palette &p, const Rect &r, float radius, bool enabled)
{
    c.setColor(enabled ? p.input : p.window);
    c.fillRoundRect(r, radius);
}

// The 1px outline whose ALPHA is the entire hover treatment. There is no hover fill and no
// pressed state anywhere in this file -- see ink.h.
void strokeInk(Canvas &c, const Palette &p, const Rect &r, float radius, bool enabled, bool on,
               bool hovered)
{
    c.setColor(inkFor(p, enabled, on), hovered ? kOutlineAlphaHover : kOutlineAlphaIdle);
    c.setPenSize(1.0f);
    c.strokeRoundRect(r, radius);
}

} // namespace

void drawIcon(Canvas &c, Icon icon, const Rect &r, uint32_t rgb)
{
    const float cx = r.centerX();
    const float cy = r.centerY();
    const float u = std::min(r.w, r.h) / 2.0f; // the icon's half-extent

    c.setColor(rgb);

    switch (icon) {
        case Icon::SpeakerOn:
        case Icon::SpeakerMuted: {
            // A speaker: a small square body, then a cone flaring out to the right. The cone is one
            // filled triangle -- stacking fillRects to fake it stair-steps visibly even at 1x.
            const float bodyH = u * 0.46f;
            const float bodyW = u * 0.30f;
            const float bodyX = cx - u * 0.90f;
            const float coneX = bodyX + bodyW;
            const float coneTip = cx - u * 0.05f;
            const float coneH = u * 0.92f;

            c.fillRect(Rect(bodyX, cy - bodyH / 2.0f, bodyW, bodyH));
            const float tri[] = {coneX,   cy - bodyH / 2.0f, coneTip, cy - coneH,
                                 coneTip, cy + coneH,        coneX,   cy + bodyH / 2.0f};
            c.fillPolygon(tri, 4);

            if (icon == Icon::SpeakerOn) {
                // Canvas::strokeArc uses the knob convention: degrees, 0 is UP, positive clockwise.
                // 30..150 is therefore the RIGHT-hand side. (-60..60, the obvious-looking choice,
                // is the TOP -- which is where these first landed.) It also sets the colour itself,
                // so it is given one rather than left to inherit.
                c.strokeArc(coneTip, cy, u * 0.42f, 30.0, 150.0, 1.3f, rgb);
                c.strokeArc(coneTip, cy, u * 0.72f, 30.0, 150.0, 1.3f, rgb);
                c.setColor(rgb); // strokeArc left its own colour set
            } else {
                c.setPenSize(1.6f);
                const float k = u * 0.30f;
                const float mx = cx + u * 0.52f;
                c.strokeLine(mx - k, cy - k, mx + k, cy + k);
                c.strokeLine(mx + k, cy - k, mx - k, cy + k);
            }
            break;
        }
        case Icon::CaptureOn:
            c.fillEllipse(cx, cy, u * 0.55f, u * 0.55f);
            break;
        case Icon::CaptureOff:
            c.setPenSize(1.6f);
            c.strokeEllipse(cx, cy, u * 0.55f, u * 0.55f);
            break;
        case Icon::Sun: {
            c.fillEllipse(cx, cy, u * 0.40f, u * 0.40f);
            c.setPenSize(1.3f);
            for (int i = 0; i < 8; ++i) {
                const double a = i * 3.14159265358979 / 4.0;
                const float dx = static_cast<float>(std::cos(a));
                const float dy = static_cast<float>(std::sin(a));
                c.strokeLine(cx + dx * u * 0.62f, cy + dy * u * 0.62f, cx + dx * u * 0.92f,
                             cy + dy * u * 0.92f);
            }
            break;
        }
        case Icon::Moon:
            // A full disc. The crescent is made by the CALLER punching a second disc out of it in
            // whatever colour is behind the icon, which is the only thing that knows what that is.
            c.fillEllipse(cx, cy, u * 0.72f, u * 0.72f);
            break;
    }
}

void drawGroupBox(Canvas &c, const Palette &p, const Rect &frame, const char *title)
{
    c.setColor(p.surface);
    c.fillRoundRect(frame, geo::kGroupRadius);

    // THE FRAME IS THE ACCENT, not the neutral border token. Drawn at an alpha rather than full
    // strength: at 255 three stacked boxes outlined in a saturated colour fight the controls
    // inside them for attention, which is the opposite of what a frame is for.
    c.setColor(p.accent, kFrameAlpha);
    c.setPenSize(1.0f);
    c.strokeRoundRect(frame, geo::kGroupRadius);

    if (title && *title) {
        c.setFont(Font::Body);
        c.setFontSize(geo::kGroupTitleSize);
        c.setColor(p.subtext);
        const float maxW = frame.w - 2.0f * geo::kGroupTitleX;
        const std::string s = c.clipToWidth(title, maxW);
        // The title sits in the band ABOVE the frame, baselined so its descenders clear the
        // frame's top edge rather than touching it.
        c.drawString(s.c_str(), frame.x + geo::kGroupTitleX, frame.y - 5.0f);

        // A gold hairline from the end of the title to the frame's right edge: the family's
        // piping, in the one place this window has room for it. CPU-Power rules off its title
        // the same way, at the same sort of alpha.
        const float titleEnd = frame.x + geo::kGroupTitleX + c.stringWidth(s.c_str()) + 8.0f;
        const float ruleY = frame.y - 9.0f;
        if (titleEnd < frame.right() - 4.0f) {
            c.setColor(p.gold, kHairlineAlpha);
            c.setPenSize(1.0f);
            c.strokeLine(titleEnd, ruleY, frame.right(), ruleY);
        }
    }
}

void drawRowText(Canvas &c, const Palette &p, const Rect &row, const char *text, uint32_t rgb,
                 float size)
{
    (void)p;
    if (!text || !*text)
        return;
    c.setFont(Font::Body);
    c.setFontSize(size);
    c.setColor(rgb);
    const std::string s = c.clipToWidth(text, row.w);
    c.drawString(s.c_str(), row.x, row.centerY() + size * geo::kLabelBaselineBias);
}

// --- Slider -----------------------------------------------------------------

namespace
{
// The thumb travels between these two x positions; everything about the slider derives from it,
// so draw() and valueAt() cannot disagree about where a value sits.
float thumbTravelX0(const Rect &r)
{
    return r.x + geo::kThumbR;
}
float thumbTravelW(const Rect &r)
{
    return std::max(1.0f, r.w - 2.0f * geo::kThumbR);
}
} // namespace

void Slider::draw(Canvas &c, const Palette &p) const
{
    const float grooveY = rect.centerY() - geo::kGrooveH / 2.0f;
    const Rect groove(rect.x, grooveY, rect.w, geo::kGrooveH);

    c.setColor(enabled ? p.input : p.border);
    c.fillRoundRect(groove, geo::kGrooveRadius);

    const float t = std::clamp(value, 0, 100) / 100.0f;
    const float cx = thumbTravelX0(rect) + thumbTravelW(rect) * t;

    // The filled portion. The Qt stylesheet ran a light-to-dark accent gradient across it; the
    // house idiom is a flat accent, and Canvas has no gradient API in any project in this family.
    if (cx > groove.x) {
        c.setColor(enabled ? p.accent : p.border);
        c.fillRoundRect(Rect(groove.x, groove.y, cx - groove.x, groove.h), geo::kGrooveRadius);
    }

    // The thumb: @TEXT filled, accent-ringed when the pointer is on it. 16x16 at radius 8.
    c.setColor(enabled ? p.text : p.disabled);
    c.fillEllipse(cx, rect.centerY(), geo::kThumbR, geo::kThumbR);
    if (enabled && (hovered || dragging)) {
        c.setColor(p.accent, kOutlineAlphaHover);
        c.strokeEllipse(cx, rect.centerY(), geo::kThumbR, geo::kThumbR);
    }
}

bool Slider::hit(float x, float y) const
{
    if (!enabled)
        return false;
    // Grown to the thumb's size in both axes: the groove itself is 6 units tall and would be a
    // target nobody can hit.
    const Rect target(rect.x - geo::kThumbR, rect.centerY() - geo::kThumbR,
                      rect.w + 2.0f * geo::kThumbR, 2.0f * geo::kThumbR);
    return target.contains(x, y);
}

int Slider::valueAt(float x) const
{
    const float t = (x - thumbTravelX0(rect)) / thumbTravelW(rect);
    return std::clamp(static_cast<int>(std::lround(t * 100.0f)), 0, 100);
}

// --- IconButton -------------------------------------------------------------

void IconButton::draw(Canvas &c, const Palette &p) const
{
    fillWell(c, p, rect, geo::kMuteRadius, enabled);
    if (on && enabled) {
        c.setColor(p.accent, kOnFillAlpha);
        c.fillRoundRect(rect, geo::kMuteRadius);
    }
    strokeInk(c, p, rect, geo::kMuteRadius, enabled, on, hovered);

    drawIcon(c, on ? iconOn : iconOff, rect.inset(5.0f),
             !enabled ? p.disabled : (on ? p.onAccent : p.text));
}

bool IconButton::hit(float x, float y) const
{
    return enabled && rect.contains(x, y);
}

// --- Toggle (checkbox / radio) ----------------------------------------------

namespace
{
Rect indicatorRect(const Rect &row)
{
    return Rect(row.x, row.centerY() - geo::kIndicatorSize / 2.0f, geo::kIndicatorSize,
                geo::kIndicatorSize);
}
} // namespace

float Toggle::textX() const
{
    return rect.x + geo::kIndicatorSize + geo::kIndicatorGap;
}

float Toggle::textMaxW() const
{
    return std::max(0.0f, rect.right() - textX());
}

void Toggle::draw(Canvas &c, const Palette &p) const
{
    const Rect box = indicatorRect(rect);
    const bool circle = shape == Shape::Radio;
    const float radius = circle ? geo::kRadioRadius : geo::kCheckRadius;

    if (circle) {
        c.setColor(enabled ? p.input : p.window);
        c.fillEllipse(box.centerX(), box.centerY(), geo::kRadioRadius, geo::kRadioRadius);
    } else {
        fillWell(c, p, box, radius, enabled);
    }

    if (on && enabled) {
        c.setColor(p.accent, kOnFillAlpha);
        if (circle)
            c.fillEllipse(box.centerX(), box.centerY(), geo::kRadioRadius, geo::kRadioRadius);
        else
            c.fillRoundRect(box, radius);
    }

    c.setColor(inkFor(p, enabled, on), hovered ? kOutlineAlphaHover : kOutlineAlphaIdle);
    c.setPenSize(1.0f);
    if (circle)
        c.strokeEllipse(box.centerX(), box.centerY(), geo::kRadioRadius, geo::kRadioRadius);
    else
        c.strokeRoundRect(box, radius);

    // The mark. A radio gets a dot; a checkbox gets two strokeLines, never a glyph -- a
    // checkmark taken from the body font depends on that font having one.
    if (on) {
        c.setColor(enabled ? p.onAccent : p.disabled);
        if (circle) {
            c.fillEllipse(box.centerX(), box.centerY(), geo::kRadioRadius * 0.4f,
                          geo::kRadioRadius * 0.4f);
        } else {
            c.setPenSize(2.0f);
            const float x0 = box.x + box.w * 0.24f;
            const float x1 = box.x + box.w * 0.44f;
            const float x2 = box.x + box.w * 0.78f;
            c.strokeLine(x0, box.centerY(), x1, box.y + box.h * 0.72f);
            c.strokeLine(x1, box.y + box.h * 0.72f, x2, box.y + box.h * 0.28f);
        }
    }

    if (!label.empty()) {
        c.setFont(Font::Body);
        c.setFontSize(geo::kBodySize);
        c.setColor(enabled ? p.text : p.disabled);
        const std::string s = c.clipToWidth(label, textMaxW());
        c.drawString(s.c_str(), textX(), rect.centerY() + geo::kBodySize * geo::kLabelBaselineBias);
    }
}

bool Toggle::hit(float x, float y) const
{
    return enabled && rect.contains(x, y);
}

} // namespace audiogui
