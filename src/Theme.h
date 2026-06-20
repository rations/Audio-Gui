// Theme.h
// Self-contained look for the app: a Fusion base plus a generated stylesheet, so
// the UI looks the same on every system instead of inheriting whatever Qt/GTK
// theme the user runs. Dark/light mode and the accent color are user-selectable
// and persisted via QSettings.
#pragma once

#include <QColor>
#include <QString>

namespace Theme
{

enum class Mode
{
  Dark = 0,
  Light = 1
};

enum class Accent
{
  Green = 0,
  Orange = 1,
  Blue = 2,
  Yellow = 3
};

// Accent swatch (for the picker) and display name.
QColor accentColor(Accent a);
QString accentName(Accent a);

// Background / track colors for the custom-painted level meter in this mode.
QColor meterBackground(Mode m);
QColor meterTrack(Mode m);

// Apply Fusion + palette + stylesheet to the whole application.
void apply(Mode mode, Accent accent);

// Persistence (QSettings keys under "appearance/").
Mode savedMode();
Accent savedAccent();
void save(Mode mode, Accent accent);

} // namespace Theme
