// AlsaMixer.h
// Thin Qt wrapper over the libasound simple-mixer (snd_mixer_*) API.
//
// Exposes the handful of controls the GUI cares about (Master, Headphone,
// Speaker, Mic, Mic Boost, Capture, IEC958) as named "elements" with
// volume/mute/switch accessors. Controls that a given sound card does not have
// are simply reported absent, so the UI can omit them.
//
// External changes (another app or `amixer` moving a control) are delivered live:
// the mixer's poll descriptors are registered with QSocketNotifier and
// `changed()` is emitted after snd_mixer_handle_events().
#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <alsa/asoundlib.h>

class QSocketNotifier;

class AlsaMixer : public QObject
{
  Q_OBJECT

public:
  // What kind of control an element is, which decides which libasound calls apply.
  enum class Kind
  {
    PlaybackVolume, // has playback volume (+ usually a mute switch)
    CaptureVolume, // has capture volume (Mic) (+ usually a capture switch)
    PlaybackSwitch, // switch-only, e.g. IEC958 on some cards
    CaptureSwitch // switch-only capture enable (Capture on some cards)
  };

  struct Element
  {
    QString name; // ALSA simple-element name, e.g. "Master"
    QString label; // human label for the UI
    Kind kind = Kind::PlaybackVolume;
    bool hasVolume = false;
    bool hasSwitch = false; // mute (playback) or enable (capture) switch
    snd_mixer_elem_t* elem = nullptr; // owned by the mixer handle, not by us
  };

  explicit AlsaMixer(QObject* parent = nullptr);
  ~AlsaMixer() override;

  // Open the given ALSA card ("default" by default). Returns false on failure.
  bool open(const QString& card = QStringLiteral("default"));

  const QVector<Element>& elements() const { return m_elements; }

  // Volume is 0..100 (percent of the control's range). Clamped internally.
  int volume(const Element& e) const;
  void setVolume(const Element& e, int percent);

  // For playback elements the switch is "not muted"; for capture it is "enabled".
  bool switchOn(const Element& e) const;
  void setSwitchOn(const Element& e, bool on);

signals:
  // Emitted when the hardware/mixer state changed underneath us (external edit).
  void changed();

private slots:
  void onPollActivity();

private:
  void buildElements();
  void registerNotifiers();

  snd_mixer_t* m_handle = nullptr;
  QString m_card;
  QVector<Element> m_elements;
  QVector<QSocketNotifier*> m_notifiers;
};
