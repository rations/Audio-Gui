// MixerStripWidget.h
// One mixer row: label + horizontal volume slider + mute/enable toggle, bound to
// a single AlsaMixer element. Two-way: user edits push to ALSA; refresh() pulls
// current hardware values back (used when an external change fires).
#pragma once

#include <QWidget>

#include "AlsaMixer.h"

class QLabel;
class QSlider;
class QToolButton;

class MixerStripWidget : public QWidget
{
  Q_OBJECT

public:
  MixerStripWidget(AlsaMixer* mixer, AlsaMixer::Element element, QWidget* parent = nullptr);

  // Re-read current volume/switch state from ALSA into the widgets.
  void refresh();

private slots:
  void onSliderMoved(int value);
  void onMuteToggled(bool checked);

private:
  void updateMuteIcon(bool on);

  AlsaMixer* m_mixer;
  AlsaMixer::Element m_element;

  QLabel* m_label = nullptr;
  QSlider* m_slider = nullptr;
  QToolButton* m_mute = nullptr;
  bool m_updating = false; // guard against feedback while we set widgets programmatically
};
