// See levelmeter.h.

#include "levelmeter.h"

#include "../geometry.h"
#include "ink.h"

#include <algorithm>

namespace audiogui
{
namespace
{

// One channel's state, so the L and R cases cannot drift apart. Everything the ballistics do is
// here; advance() is just this twice.
struct Chan {
    float &disp;
    float &peak;
    int &hold;
};

bool step(Chan ch, bool have, float sample)
{
    ch.disp *= kMeterRelease;
    if (ch.hold > 0)
        --ch.hold;
    else
        ch.peak *= kPeakRelease;

    if (have) {
        const float v = std::clamp(sample, 0.0f, 1.0f);
        if (v > ch.disp)
            ch.disp = v; // instant attack
    }

    if (ch.disp > ch.peak) {
        ch.peak = ch.disp;
        ch.hold = kPeakHoldTicks;
    }

    if (ch.disp < kMeterIdle)
        ch.disp = 0.0f;
    if (ch.peak < kMeterIdle)
        ch.peak = 0.0f;

    return ch.disp > 0.0f || ch.peak > 0.0f;
}

} // namespace

bool LevelMeter::advance(bool have, float l, float r)
{
    const bool a = step({dispL, peakL, holdL}, have, l);
    const bool b = step({dispR, peakR, holdR}, have, r);
    return a || b;
}

void LevelMeter::reset()
{
    dispL = dispR = peakL = peakR = 0.0f;
    holdL = holdR = 0;
}

void LevelMeter::draw(Canvas &c, const Palette &p) const
{
    const float barH = geo::meterBarH();

    auto bar = [&](float y, float level, float peak) {
        const Rect well(rect.x, y, rect.w, barH);

        c.setColor(p.input);
        c.fillRoundRect(well, geo::kMeterRadius);
        // Gold piping round the well, which is exactly what rations-amp's drawMeter does on its
        // own code-drawn fallback well -- the same colour at a similar alpha.
        c.setColor(p.gold, kMeterWellAlpha);
        c.setPenSize(1.0f);
        c.strokeRoundRect(well, geo::kMeterRadius);

        // The fill lives inside the well's border, so a full-scale bar does not paint over the
        // rounded corners it is supposed to sit within.
        const Rect track = well.inset(geo::kMeterWellInset);
        if (track.w <= 0.0f || track.h <= 0.0f)
            return;

        const float lv = std::clamp(level, 0.0f, 1.0f);
        const float fillW = track.w * lv;

        if (fillW > 0.0f) {
            c.setColor(p.accent);
            c.fillRect(Rect(track.x, track.y, fillW, track.h));

            // The rungs. Their positions are measured from the track's own left edge, never from
            // the fill's width, which is what keeps them still while the level moves; they are
            // then drawn only across the lit part, so the empty track stays plain.
            c.setColor(p.window);
            for (float x = track.x + geo::kRungPitch; x < track.x + fillW; x += geo::kRungPitch)
                c.fillRect(Rect(x, track.y, geo::kRungW, track.h));

            // The lit tip.
            if (fillW >= geo::kMeterCapW) {
                c.setColor(p.accentBright);
                c.fillRect(
                    Rect(track.x + fillW - geo::kMeterCapW, track.y, geo::kMeterCapW, track.h));
            }
        }

        if (peak > 0.0f) {
            const float pv = std::clamp(peak, 0.0f, 1.0f);
            // Clamped to the track's right edge so a full-scale peak stays inside the well
            // instead of half-hanging off it.
            const float x = std::min(track.x + track.w * pv, track.right() - geo::kMeterPeakW);
            c.setColor(p.peak);
            c.fillRect(Rect(x, track.y, geo::kMeterPeakW, track.h));
        }
    };

    bar(rect.y, dispL, peakL);
    bar(rect.y + barH + geo::kMeterBarGap, dispR, peakR);
}

} // namespace audiogui
