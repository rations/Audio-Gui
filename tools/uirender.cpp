// uirender -- draw the panel offline and AUDIT EVERY STRING AGAINST ITS REAL SLOT.
//
// A static_assert can hold a clearance between two rectangles, because both are compile-time
// numbers. It cannot hold a label inside its slot: how wide "IEC958 (S/PDIF)" renders depends on
// the font, the size and the rasteriser, none of which exist at compile time. So the layout has
// two halves -- geometry.h asserts the rectangles, and this measures the text that goes in them,
// with the real fonts, at every scale the window is claimed to work at.
//
// A string wider than its slot is not a crash. Canvas::clipToWidth truncates it with an ellipsis,
// so the window still draws. That is exactly why this tool exists: the failure is silent, it is
// invisible on the developer's machine if their font happens to be narrow enough, and the first
// person to see "IEC958 (S/PD..." is a user on a different fontconfig.
//
// IT ALSO DRAWS THE METER, at levels and with a peak marker, which is the one part of this window
// that cannot be checked by looking at a still screenshot of the running program -- by the time
// you have it, the level has moved. rations-amp's tools/panelrender.cpp does the same for the
// same reason.
//
// Text metrics are scale-invariant in logical units: Canvas sets CAIRO_HINT_METRICS_OFF, so a
// string's width at scale s is exactly s times its width at scale 1. The audit therefore runs
// once, and the PNGs are written at each scale because rasterisation at 0.75x genuinely is not
// rasterisation at 2x.
//
// Exit status is 0 only if every string fits.
//
// Usage: uirender [--out <dir>]

#include "gfx/fontstack.h"
#include "geometry.h"
#include "panel.h"
#include "platform/respath.h"

#include <cairo/cairo.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <sys/stat.h>
#include <sys/types.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace audiogui;

namespace
{

int gFailures = 0;

// The worst case each slot has to hold: the longest string this window can ever put there.
const char *kLongestStripLabel = "Mic Boost";
const char *kLongestSwitchLabel = "IEC958 (S/PDIF)";
const char *kLongestDeviceLabel = "HDA Intel PCH \xE2\x80\x94 HDMI/DP,pcm=3 Digital Out";

// EVERY CHARACTER THE PANEL DRAWS MUST EXIST IN THE FACE THAT DRAWS IT.
//
// This is a separate question from whether the text fits, and checking only the latter is how a
// hole in a label reaches a user. cairo's toy text API -- cairo_show_text on one cairo_ft face --
// has NO FONT FALLBACK: a character the face lacks is rendered as glyph 0, which in Roboto is a
// zero-width nothing. It therefore measures as fitting, draws as a gap, and looks on the
// developer's machine exactly like it looks in the screenshot they are about to ship.
//
// Qt hid this completely, because Qt falls back per character across the whole installed font
// set. That is precisely why the Qt build could put a U+2192 arrow in a radio label and why this
// port could not.
void checkGlyphs(const FontStack &fonts, const char *what, const std::string &text, Font font)
{
    FT_Face face =
        static_cast<FT_Face>(font == Font::Title ? fonts.titleFtFace() : fonts.bodyFtFace());
    if (!face)
        return; // a toy fallback; load() already refused to proceed on that

    // Minimal UTF-8 decode: the panel's strings are literals in this repository, so this only has
    // to handle well-formed input.
    const unsigned char *p = reinterpret_cast<const unsigned char *>(text.c_str());
    while (*p) {
        unsigned cp = *p;
        int len = 1;
        if ((*p & 0xE0) == 0xC0) {
            cp = *p & 0x1Fu;
            len = 2;
        } else if ((*p & 0xF0) == 0xE0) {
            cp = *p & 0x0Fu;
            len = 3;
        } else if ((*p & 0xF8) == 0xF0) {
            cp = *p & 0x07u;
            len = 4;
        }
        for (int i = 1; i < len; ++i) {
            if ((p[i] & 0xC0) != 0x80) {
                len = i;
                break;
            }
            cp = (cp << 6) | (p[i] & 0x3Fu);
        }
        if (cp >= 0x20 && FT_Get_Char_Index(face, cp) == 0) {
            fprintf(stderr,
                    "uirender: %s uses U+%04X, which %s has no glyph for -- it will draw as a "
                    "gap: \"%s\"\n",
                    what, cp, face->family_name ? face->family_name : "the bundled face",
                    text.c_str());
            ++gFailures;
        }
        p += len;
    }
}

void checkFits(Canvas &c, const char *what, const std::string &text, float slot, Font font,
               float size)
{
    c.setFont(font);
    c.setFontSize(size);
    const float w = c.stringWidth(text.c_str());
    if (w > slot) {
        fprintf(stderr, "uirender: %s does not fit: \"%s\" needs %.1f, slot is %.1f\n", what,
                text.c_str(), static_cast<double>(w), static_cast<double>(slot));
        ++gFailures;
    }
}

Panel::Strip makeStrip(const char *label, int value, bool muted, bool capture)
{
    Panel::Strip s;
    s.label = label;
    s.hasVolume = true;
    s.hasSwitch = true;
    s.slider.value = value;
    s.button.on = !muted;
    s.button.iconOn = capture ? Icon::CaptureOn : Icon::SpeakerOn;
    s.button.iconOff = capture ? Icon::CaptureOff : Icon::SpeakerMuted;
    return s;
}

std::vector<ComboItem> devices()
{
    std::vector<ComboItem> v;
    v.push_back({"HDA Intel PCH \xE2\x80\x94 HDA Analog", "", 0, false});
    v.push_back({kLongestDeviceLabel, "PCH:3", 0, false});
    v.push_back({"UMC204HD 192k \xE2\x80\x94 USB Audio", "U192k:0", 0, false});
    return v;
}

// Fill a panel with the worst case rather than with what this machine happens to report.
void scenePopulated(Panel &p, bool meter)
{
    std::vector<Panel::Strip> strips;
    strips.push_back(makeStrip("Master", 72, false, false));
    strips.push_back(makeStrip("Headphone", 55, false, false));
    strips.push_back(makeStrip("Speaker", 90, true, false));
    strips.push_back(makeStrip("Microphone", 40, false, true));
    strips.push_back(makeStrip(kLongestStripLabel, 18, true, false));
    p.setStrips(std::move(strips));
    p.setSwitches({"Capture", kLongestSwitchLabel}, {true, false});
    p.setDevices(devices(), 1);
    p.setJackAvailable(true);
    p.setMeterVisible(meter);
}

void sceneEmpty(Panel &p)
{
    p.setStrips({});
    p.setSwitches({}, {});
    p.setPlaceholder("No mixer controls for this output.");
    p.setDevices(devices(), 2);
    p.setJackAvailable(false);
    p.setMeterVisible(false);
}

struct Scene {
    const char *name;
    Mode mode;
    Accent accent;
};

const Scene kScenes[] = {
    {"full-dark", Mode::Dark, Accent::Green},       {"full-light", Mode::Light, Accent::Green},
    {"accent-orange", Mode::Dark, Accent::Orange},  {"accent-blue", Mode::Dark, Accent::Blue},
    {"accent-yellow", Mode::Light, Accent::Yellow},
};

bool render(Panel &p, float scale, const std::string &outPath)
{
    p.layout();
    const int pw = static_cast<int>(geo::kWinW * scale + 0.5f);
    const int ph = static_cast<int>(p.height() * scale + 0.5f);

    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s);
        return false;
    }
    cairo_t *cr = cairo_create(s);
    // The scale goes exactly where X11Window::paint puts it, and nowhere else.
    cairo_scale(cr, scale, scale);
    {
        FontStack fonts;
        fonts.load(resourceDir());
        Canvas c(cr, &fonts, geo::kWinW, p.height());
        p.draw(c);
    }
    cairo_destroy(cr);
    const bool ok = cairo_surface_write_to_png(s, outPath.c_str()) == CAIRO_STATUS_SUCCESS;
    cairo_surface_destroy(s);
    return ok;
}

} // namespace

int main(int argc, char **argv)
{
    std::string out = ".";
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--out") == 0 && i + 1 < argc)
            out = argv[++i];
    }

    // Create the output directory rather than failing on every write: this is a developer tool
    // and "mkdir first" is not a useful thing to have to remember.
    mkdir(out.c_str(), 0755);

    FontStack fonts;
    if (!fonts.load(resourceDir())) {
        // Measuring text against a substituted system face proves nothing about what a user with
        // the bundled fonts will see, so this is fatal here even though the window survives it.
        fprintf(stderr,
                "uirender: the bundled fonts are missing; the audit would be meaningless\n");
        return 1;
    }

    // --- the audit, once ---
    {
        cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
        cairo_t *cr = cairo_create(s);
        Canvas c(cr, &fonts, geo::kWinW, geo::kWinHMax);

        // Glyph coverage first: a string with a hole in it is wrong whatever its width.
        for (const char *t : {"Master",
                              "Headphone",
                              "Speaker",
                              "Microphone",
                              "Mic Boost",
                              "Capture",
                              kLongestSwitchLabel,
                              "Mixer",
                              "Output device",
                              "Audio options & switches",
                              "Output level",
                              "No mixer controls for this output.",
                              "Dark",
                              "Light",
                              "PA Bridge \xC2\xBB ALSA (default)",
                              "ALSA (no bridge)",
                              "PA Bridge \xC2\xBB JACK",
                              kLongestDeviceLabel,
                              "Could not open the ALSA mixer (\"default\").",
                              "Check that a sound card is present and alsa-utils is installed."})
            checkGlyphs(fonts, "a panel string", t, Font::Body);
        for (int i = 0; i < kAccentCount; ++i)
            checkGlyphs(fonts, "an accent name", accentName(static_cast<Accent>(i)), Font::Body);
        // clipToWidth appends U+2026 whenever anything is truncated, so the ellipsis itself has
        // to exist or an elided string ends in a gap.
        checkGlyphs(fonts, "the truncation ellipsis", "\xE2\x80\xA6", Font::Body);

        checkFits(c, "a mixer strip label", kLongestStripLabel, geo::kStripLabelW, Font::Body,
                  geo::kBodySize);
        for (const char *l : {"Master", "Headphone", "Speaker", "Microphone", "Mic Boost"})
            checkFits(c, "a mixer strip label", l, geo::kStripLabelW, Font::Body, geo::kBodySize);

        // A switch checkbox's text starts after the indicator and its gap.
        const float switchSlot = geo::kGroupInnerW - geo::kIndicatorSize - geo::kIndicatorGap;
        for (const char *l : {"Capture", kLongestSwitchLabel})
            checkFits(c, "a switch label", l, switchSlot, Font::Body, geo::kBodySize);

        for (const char *l : {"PA Bridge \xE2\x86\x92 ALSA (default)", "ALSA (no bridge)",
                              "PA Bridge \xE2\x86\x92 JACK"})
            checkFits(c, "a routing label", l, switchSlot, Font::Body, geo::kBodySize);

        for (const char *t : {"Mixer", "Output device", "Audio options & switches"})
            checkFits(c, "a group title", t, geo::kContentW - 2.0f * geo::kGroupTitleX, Font::Body,
                      geo::kGroupTitleSize);

        checkFits(c, "the meter label", "Output level", geo::kGroupInnerW, Font::Body,
                  geo::kMeterLabelSize);
        checkFits(c, "the placeholder", "No mixer controls for this output.", geo::kGroupInnerW,
                  Font::Body, geo::kBodySize);

        // The theme toggle: icon box, gap, then the word.
        const float themeSlot = geo::kModeToggleW - 7.0f - 14.0f - 6.0f - 8.0f;
        for (const char *l : {"Dark", "Light"})
            checkFits(c, "the theme toggle", l, themeSlot, Font::Body, geo::kBarTextSize);

        // The accent combo, closed: swatch, gap, then the name.
        const float accentSlot =
            geo::kAccentComboW - geo::kComboArrowW - geo::kComboPadX - geo::kSwatch - 6.0f;
        for (int i = 0; i < kAccentCount; ++i)
            checkFits(c, "an accent name", accentName(static_cast<Accent>(i)), accentSlot,
                      Font::Body, geo::kBodySize);

        cairo_destroy(cr);
        cairo_surface_destroy(s);
    }

    // --- the pictures ---
    const float kScales[] = {geo::kScaleMin, 1.0f, geo::kScaleMax};

    for (const Scene &sc : kScenes) {
        Panel p;
        p.setMode(sc.mode);
        p.setAccent(sc.accent);
        scenePopulated(p, true);
        p.setRouting(Routing::PulseToAlsa);
        p.layout();
        // A meter frozen mid-signal, with the peak marker held behind the level.
        p.meter().dispL = 0.62f;
        p.meter().dispR = 0.74f;
        p.meter().peakL = 0.80f;
        p.meter().peakR = 0.93f;

        for (float scale : kScales) {
            char buf[512];
            snprintf(buf, sizeof(buf), "%s/panel-%s@%.2fx.png", out.c_str(), sc.name,
                     static_cast<double>(scale));
            if (!render(p, scale, buf)) {
                fprintf(stderr, "uirender: could not write %s\n", buf);
                ++gFailures;
            }
        }
    }

    // The empty case: a USB or HDMI output on a shared card, in pure-ALSA mode.
    {
        Panel p;
        p.setMode(Mode::Dark);
        sceneEmpty(p);
        p.setRouting(Routing::PureAlsa);
        p.setDeviceComboEnabled(true, "");
        render(p, 1.0f, out + "/panel-empty.png");
    }

    // The mixer-open failure: the whole window is the message.
    {
        Panel p;
        p.setMode(Mode::Dark);
        p.setFatalError("Could not open the ALSA mixer (\"default\").\nCheck that a sound card is "
                        "present and alsa-utils is installed.");
        render(p, 1.0f, out + "/panel-error.png");
    }

    // Meter levels, so the ladder, the lit tip and the peak marker are all in a diffable artifact.
    {
        Panel p;
        p.setMode(Mode::Dark);
        scenePopulated(p, true);
        p.layout();
        const float levels[][4] = {
            {0.0f, 0.0f, 0.0f, 0.0f}, {0.25f, 0.18f, 0.40f, 0.33f}, {1.0f, 0.97f, 1.0f, 1.0f}};
        const char *names[] = {"silent", "quiet", "hot"};
        for (int i = 0; i < 3; ++i) {
            p.meter().dispL = levels[i][0];
            p.meter().dispR = levels[i][1];
            p.meter().peakL = levels[i][2];
            p.meter().peakR = levels[i][3];
            render(p, 1.0f, out + "/meter-" + names[i] + ".png");
        }
    }

    if (gFailures > 0) {
        fprintf(stderr, "uirender: %d layout problem(s)\n", gFailures);
        return 1;
    }
    printf("uirender: layout audit passed; images written to %s\n", out.c_str());
    return 0;
}
