// MixerStripWidget.cpp
#include "MixerStripWidget.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QStyle>
#include <QToolButton>

MixerStripWidget::MixerStripWidget(AlsaMixer* mixer, AlsaMixer::Element element, QWidget* parent)
: QWidget(parent)
, m_mixer(mixer)
, m_element(std::move(element))
{
  auto* row = new QHBoxLayout(this);
  row->setContentsMargins(0, 0, 0, 0);

  m_label = new QLabel(m_element.label, this);
  m_label->setMinimumWidth(110);
  row->addWidget(m_label);

  if (m_element.hasVolume)
  {
    m_slider = new QSlider(Qt::Horizontal, this);
    m_slider->setRange(0, 100);
    m_slider->setSingleStep(2);
    m_slider->setPageStep(10);
    connect(m_slider, &QSlider::valueChanged, this, &MixerStripWidget::onSliderMoved);
    row->addWidget(m_slider, 1);
  }
  else
  {
    row->addStretch(1);
  }

  if (m_element.hasSwitch)
  {
    m_mute = new QToolButton(this);
    m_mute->setCheckable(true);
    m_mute->setAutoRaise(true);
    connect(m_mute, &QToolButton::toggled, this, &MixerStripWidget::onMuteToggled);
    row->addWidget(m_mute);
  }

  refresh();
}

void MixerStripWidget::refresh()
{
  m_updating = true;
  if (m_slider)
    m_slider->setValue(m_mixer->volume(m_element));
  if (m_mute)
  {
    bool on = m_mixer->switchOn(m_element);
    // For a playback element the switch being on means "not muted"; the button is
    // "checked" when the control is active (unmuted / capture-enabled).
    m_mute->setChecked(on);
    updateMuteIcon(on);
  }
  m_updating = false;
}

void MixerStripWidget::onSliderMoved(int value)
{
  if (m_updating)
    return;
  m_mixer->setVolume(m_element, value);
}

void MixerStripWidget::onMuteToggled(bool checked)
{
  if (m_updating)
    return;
  m_mixer->setSwitchOn(m_element, checked);
  updateMuteIcon(checked);
}

void MixerStripWidget::updateMuteIcon(bool on)
{
  if (!m_mute)
    return;
  const bool capture =
    m_element.kind == AlsaMixer::Kind::CaptureVolume || m_element.kind == AlsaMixer::Kind::CaptureSwitch;
  m_mute->setText(on ? (capture ? QStringLiteral("●") : QStringLiteral("🔊"))
                     : (capture ? QStringLiteral("○") : QStringLiteral("🔇")));
  m_mute->setToolTip(on ? (capture ? tr("Enabled — click to disable") : tr("On — click to mute"))
                        : (capture ? tr("Disabled — click to enable") : tr("Muted — click to unmute")));
}
