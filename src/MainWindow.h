// MainWindow.h
// Top-level window: a Mixer section (dynamic volume strips + level meter), a
// Devices section (output-device dropdown), and a Routing & Switches section
// (3-way PA-compat routing radio + Capture / IEC958 toggles). The mixer strips
// and switches follow the selected output device, so they are rebuilt when it
// changes; the device list refreshes live on hotplug.
#pragma once

#include <QMainWindow>
#include <QVector>

#include "AlsaDevices.h"
#include "AlsaMixer.h"
#include "BridgeManager.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QRadioButton;
class QTimer;
class QToolButton;
class QVBoxLayout;
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
  void onDeviceSelected(int index);
  void refreshDevices();

private:
  QWidget* buildMixerSection();
  QWidget* buildDevicesSection();
  QWidget* buildRoutingSection();
  QWidget* buildAppearanceBar();
  void applyMeterThemeColors();

  // Rebuild the card-specific controls (volume strips + switches) for the mixer's
  // currently open card, showing a placeholder when the card has no controls (or
  // when suppressControls forces the placeholder, e.g. USB interfaces whose
  // sliders are nominal — gain is set by hardware knobs).
  void populateMixerControls(bool suppressControls = false);
  // Re-open the mixer on the card backing the given output-device id, then
  // repopulate the controls. Empty id / Internal -> ALSA "default".
  void reopenMixerForDevice(const QString& cardId);
  void updateDeviceComboEnabled();
  // Show the output-level meter only when a pulse bridge is running (PA→ALSA or
  // PA→JACK): in pure-ALSA mode nothing publishes peak data, so the meter would
  // sit dead — hide it (and its label) instead.
  void updateMeterVisibility();

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
  QLabel* m_meterLabel = nullptr; // "Output level" caption, hidden with the meter

  // Device selection + the card-specific control containers it rebuilds.
  QComboBox* m_deviceCombo = nullptr;
  QVBoxLayout* m_stripsLayout = nullptr; // holds the volume strips (+ placeholder)
  QVBoxLayout* m_switchesLayout = nullptr; // holds the Capture / IEC958 checkboxes
  QLabel* m_mixerPlaceholder = nullptr; // shown when the card has no volume controls
  QTimer* m_hotplugTimer = nullptr;
  QVector<AlsaDevices::OutputDevice> m_devices; // last enumerated set (hotplug diff)

  QComboBox* m_accentCombo = nullptr;
  QToolButton* m_modeToggle = nullptr;
};
