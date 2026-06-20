// Theme.cpp
#include "Theme.h"

#include <QApplication>
#include <QPalette>
#include <QSettings>
#include <QStyleFactory>

namespace Theme
{

namespace
{

// Per-mode color tokens used to build the stylesheet.
struct Palette
{
  QString window; // app background
  QString surface; // cards / group boxes
  QString border;
  QString text;
  QString subtext;
  QString input; // sliders groove / unchecked indicators
  QString hover; // subtle hover fill
  QString tipBg; // tooltip background
  QString tipText;
};

Palette paletteFor(Mode m)
{
  if (m == Mode::Dark)
    return {"#1e2228", "#262b33", "#333a44", "#e6e9ee", "#9aa3ad", "#3a4250", "#2f3640", "#11141a", "#e6e9ee"};
  return {"#f4f6f8", "#ffffff", "#d8dde3", "#1f2329", "#5b6470", "#d3d8de", "#e9edf1", "#2b2f36", "#f4f6f8"};
}

struct AccentPair
{
  QString base;
  QString hover;
  bool darkText; // true when the accent is light enough to need dark text on it
};

AccentPair accentPair(Accent a)
{
  switch (a)
  {
    case Accent::Green: return {"#2ecc71", "#27ae60", false};
    case Accent::Orange: return {"#e67e22", "#d35400", false};
    case Accent::Blue: return {"#3498db", "#2980b9", false};
    case Accent::Yellow: return {"#f1c40f", "#d4ac0d", true};
  }
  return {"#2ecc71", "#27ae60", false};
}

QString buildStyleSheet(Mode mode, Accent accent)
{
  const Palette p = paletteFor(mode);
  const AccentPair ac = accentPair(accent);
  const QString onAccent = ac.darkText ? QStringLiteral("#1f2329") : QStringLiteral("#ffffff");

  // Filled-slider gradient: a lighter shade of the accent on the left (low value)
  // deepening to a darker shade on the right (turned up).
  const QColor base(ac.base);
  const QString accentLight = base.lighter(155).name();
  const QString accentDark = base.darker(145).name();

  QString qss = QStringLiteral(R"(
* { font-size: 13px; }

QWidget { background: @WINDOW; color: @TEXT; }

QGroupBox {
  background: @SURFACE;
  border: 1px solid @BORDER;
  border-radius: 10px;
  margin-top: 16px;
  padding: 14px 14px 10px 14px;
  font-weight: 600;
}
QGroupBox::title {
  subcontrol-origin: margin;
  subcontrol-position: top left;
  left: 12px;
  padding: 0 6px;
  color: @SUBTEXT;
}

QLabel { background: transparent; }

QSlider::groove:horizontal {
  height: 6px;
  background: @INPUT;
  border-radius: 3px;
}
QSlider::sub-page:horizontal {
  background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 @ACCENT_LIGHT, stop:1 @ACCENT_DARK);
  border-radius: 3px;
}
QSlider::handle:horizontal {
  width: 16px;
  height: 16px;
  margin: -6px 0;
  border-radius: 8px;
  background: @TEXT;
}
QSlider::handle:horizontal:hover { background: @ACCENT; }
QSlider::sub-page:horizontal:disabled { background: @BORDER; }

QToolButton {
  background: @INPUT;
  border: 1px solid @BORDER;
  border-radius: 8px;
  padding: 3px 8px;
}
QToolButton:hover { border-color: @ACCENT; }
QToolButton:checked { background: @ACCENT; color: @ONACCENT; border-color: @ACCENT; }

QCheckBox, QRadioButton { spacing: 8px; background: transparent; padding: 2px 0; }
QCheckBox::indicator, QRadioButton::indicator {
  width: 16px; height: 16px;
  border: 1px solid @BORDER;
  background: @INPUT;
}
QCheckBox::indicator { border-radius: 4px; }
QRadioButton::indicator { border-radius: 9px; }
QCheckBox::indicator:checked, QRadioButton::indicator:checked {
  background: @ACCENT;
  border-color: @ACCENT;
}
QRadioButton:disabled, QCheckBox:disabled { color: @SUBTEXT; }
QCheckBox::indicator:disabled, QRadioButton::indicator:disabled { border-color: @BORDER; background: @WINDOW; }

QComboBox {
  background: @INPUT;
  border: 1px solid @BORDER;
  border-radius: 8px;
  padding: 4px 10px;
  min-height: 22px;
}
QComboBox:hover { border-color: @ACCENT; }
QComboBox::drop-down { border: none; width: 22px; }
QComboBox QAbstractItemView {
  background: @SURFACE;
  border: 1px solid @BORDER;
  selection-background-color: @ACCENT;
  selection-color: @ONACCENT;
  outline: none;
}

QToolTip {
  background: @TIPBG;
  color: @TIPTEXT;
  border: 1px solid @BORDER;
  border-radius: 6px;
  padding: 4px 8px;
}
)");

  // Replace longer accent tokens first: "@ACCENT" is a prefix of "@ACCENT_LIGHT".
  qss.replace("@ACCENT_LIGHT", accentLight)
    .replace("@ACCENT_DARK", accentDark)
    .replace("@ONACCENT", onAccent)
    .replace("@ACCENT", ac.base)
    .replace("@WINDOW", p.window)
    .replace("@SURFACE", p.surface)
    .replace("@BORDER", p.border)
    .replace("@SUBTEXT", p.subtext)
    .replace("@TEXT", p.text)
    .replace("@INPUT", p.input)
    .replace("@HOVER", p.hover)
    .replace("@TIPBG", p.tipBg)
    .replace("@TIPTEXT", p.tipText);
  return qss;
}

bool isValid(int v, int maxExclusive)
{
  return v >= 0 && v < maxExclusive;
}

} // namespace

QColor accentColor(Accent a)
{
  return QColor(accentPair(a).base);
}

QString accentName(Accent a)
{
  switch (a)
  {
    case Accent::Green: return QStringLiteral("Green");
    case Accent::Orange: return QStringLiteral("Orange");
    case Accent::Blue: return QStringLiteral("Blue");
    case Accent::Yellow: return QStringLiteral("Yellow");
  }
  return QStringLiteral("Green");
}

QColor meterBackground(Mode m)
{
  return QColor(paletteFor(m).window);
}

QColor meterTrack(Mode m)
{
  return QColor(paletteFor(m).input);
}

void apply(Mode mode, Accent accent)
{
  if (auto* fusion = QStyleFactory::create(QStringLiteral("Fusion")))
    QApplication::setStyle(fusion);

  // A matching base palette keeps any not-fully-styled bits (menus, native
  // dialogs) coherent with the stylesheet.
  const Palette p = paletteFor(mode);
  QPalette pal;
  pal.setColor(QPalette::Window, QColor(p.window));
  pal.setColor(QPalette::Base, QColor(p.surface));
  pal.setColor(QPalette::AlternateBase, QColor(p.input));
  pal.setColor(QPalette::Text, QColor(p.text));
  pal.setColor(QPalette::WindowText, QColor(p.text));
  pal.setColor(QPalette::ButtonText, QColor(p.text));
  pal.setColor(QPalette::Button, QColor(p.input));
  pal.setColor(QPalette::Highlight, accentColor(accent));
  pal.setColor(QPalette::HighlightedText, QColor(accentPair(accent).darkText ? "#1f2329" : "#ffffff"));
  pal.setColor(QPalette::Disabled, QPalette::Text, QColor(p.subtext));
  pal.setColor(QPalette::Disabled, QPalette::WindowText, QColor(p.subtext));
  QApplication::setPalette(pal);

  qApp->setStyleSheet(buildStyleSheet(mode, accent));
}

Mode savedMode()
{
  QSettings s;
  int v = s.value(QStringLiteral("appearance/mode"), static_cast<int>(Mode::Dark)).toInt();
  return isValid(v, 2) ? static_cast<Mode>(v) : Mode::Dark;
}

Accent savedAccent()
{
  QSettings s;
  int v = s.value(QStringLiteral("appearance/accent"), static_cast<int>(Accent::Green)).toInt();
  return isValid(v, 4) ? static_cast<Accent>(v) : Accent::Green;
}

void save(Mode mode, Accent accent)
{
  QSettings s;
  s.setValue(QStringLiteral("appearance/mode"), static_cast<int>(mode));
  s.setValue(QStringLiteral("appearance/accent"), static_cast<int>(accent));
}

} // namespace Theme
