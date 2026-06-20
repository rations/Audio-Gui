// MainWindow.h
// Top-level window: a Mixer section (dynamic volume strips + level meter) and a
// Routing & Switches section (3-way PA-compat routing radio + Capture / IEC958
// toggles).
#pragma once

#include <QMainWindow>
#include <QVector>

#include "AlsaMixer.h"
#include "BridgeManager.h"

class QCheckBox;
class QComboBox;
class QRadioButton;
class QToolButton;
class LevelMeter;
class MixerStripWidget;

class MainWindow : public QMainWindow
{
  Q_OBJECT

public:
  explicit MainWindow(QWidget* parent = nullptr);

private slots:
  void onMixerChanged();
  void onJackAvailabilityChanged(bool available);
  void onAppearanceChanged();

private:
  QWidget* buildMixerSection();
  QWidget* buildRoutingSection();
  QWidget* buildAppearanceBar();
  void applyMeterThemeColors();

  AlsaMixer m_mixer;
  BridgeManager m_bridges;

  QVector<MixerStripWidget*> m_strips;

  // Switch-only controls (Capture, IEC958) shown as checkboxes in routing section.
  struct SwitchBox
  {
    AlsaMixer::Element element;
    QCheckBox* box;
  };
  QVector<SwitchBox> m_switches;

  QRadioButton* m_jackRadio = nullptr;
  LevelMeter* m_meter = nullptr;

  QComboBox* m_accentCombo = nullptr;
  QToolButton* m_modeToggle = nullptr;
};
