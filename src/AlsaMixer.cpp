// AlsaMixer.cpp
#include "AlsaMixer.h"

#include <QSocketNotifier>
#include <poll.h>

#include <algorithm>

namespace
{

// The controls we surface, in display order, with the libasound name and the
// kind that decides which accessors apply. Names follow the conventional ALSA
// simple-control names; cards that lack one are skipped at build time.
struct Wanted
{
  const char* name;
  const char* label;
  AlsaMixer::Kind kind;
};

const Wanted kWanted[] = {
  {"Master", "Master", AlsaMixer::Kind::PlaybackVolume},
  {"Headphone", "Headphone", AlsaMixer::Kind::PlaybackVolume},
  {"Speaker", "Speaker", AlsaMixer::Kind::PlaybackVolume},
  {"Mic", "Microphone", AlsaMixer::Kind::CaptureVolume},
  {"Mic Boost", "Mic Boost", AlsaMixer::Kind::PlaybackVolume},
  {"Capture", "Capture", AlsaMixer::Kind::CaptureSwitch},
  {"IEC958", "IEC958 (S/PDIF)", AlsaMixer::Kind::PlaybackSwitch},
};

} // namespace

AlsaMixer::AlsaMixer(QObject* parent)
: QObject(parent)
{
}

AlsaMixer::~AlsaMixer()
{
  qDeleteAll(m_notifiers);
  m_notifiers.clear();
  if (m_handle)
  {
    snd_mixer_close(m_handle);
    m_handle = nullptr;
  }
}

bool AlsaMixer::open(const QString& card)
{
  m_card = card;

  if (snd_mixer_open(&m_handle, 0) < 0)
  {
    m_handle = nullptr;
    return false;
  }
  if (snd_mixer_attach(m_handle, card.toUtf8().constData()) < 0
      || snd_mixer_selem_register(m_handle, nullptr, nullptr) < 0 || snd_mixer_load(m_handle) < 0)
  {
    snd_mixer_close(m_handle);
    m_handle = nullptr;
    return false;
  }

  buildElements();
  registerNotifiers();
  return true;
}

void AlsaMixer::buildElements()
{
  m_elements.clear();

  snd_mixer_selem_id_t* sid = nullptr;
  snd_mixer_selem_id_alloca(&sid);

  for (const Wanted& w : kWanted)
  {
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, w.name);
    snd_mixer_elem_t* elem = snd_mixer_find_selem(m_handle, sid);
    if (!elem)
      continue;

    Element e;
    e.name = QString::fromUtf8(w.name);
    e.label = QString::fromUtf8(w.label);
    e.kind = w.kind;
    e.elem = elem;

    switch (w.kind)
    {
      case Kind::PlaybackVolume:
        e.hasVolume = snd_mixer_selem_has_playback_volume(elem) != 0;
        e.hasSwitch = snd_mixer_selem_has_playback_switch(elem) != 0;
        break;
      case Kind::CaptureVolume:
        e.hasVolume = snd_mixer_selem_has_capture_volume(elem) != 0;
        e.hasSwitch = snd_mixer_selem_has_capture_switch(elem) != 0;
        break;
      case Kind::PlaybackSwitch: e.hasSwitch = snd_mixer_selem_has_playback_switch(elem) != 0; break;
      case Kind::CaptureSwitch: e.hasSwitch = snd_mixer_selem_has_capture_switch(elem) != 0; break;
    }

    // Keep only elements that actually expose something we can drive.
    if (e.hasVolume || e.hasSwitch)
      m_elements.push_back(e);
  }
}

void AlsaMixer::registerNotifiers()
{
  int count = snd_mixer_poll_descriptors_count(m_handle);
  if (count <= 0)
    return;

  QVector<struct pollfd> pfds(count);
  count = snd_mixer_poll_descriptors(m_handle, pfds.data(), static_cast<unsigned>(count));

  for (int i = 0; i < count; ++i)
  {
    auto* n = new QSocketNotifier(pfds[i].fd, QSocketNotifier::Read, this);
    connect(n, &QSocketNotifier::activated, this, &AlsaMixer::onPollActivity);
    m_notifiers.push_back(n);
  }
}

void AlsaMixer::onPollActivity()
{
  if (!m_handle)
    return;
  // Drain ALSA's event queue, then let the UI re-read current values.
  snd_mixer_handle_events(m_handle);
  emit changed();
}

int AlsaMixer::volume(const Element& e) const
{
  if (!e.elem || !e.hasVolume)
    return 0;

  long lo = 0, hi = 0, raw = 0;
  if (e.kind == Kind::CaptureVolume)
  {
    snd_mixer_selem_get_capture_volume_range(e.elem, &lo, &hi);
    snd_mixer_selem_get_capture_volume(e.elem, SND_MIXER_SCHN_FRONT_LEFT, &raw);
  }
  else
  {
    snd_mixer_selem_get_playback_volume_range(e.elem, &lo, &hi);
    snd_mixer_selem_get_playback_volume(e.elem, SND_MIXER_SCHN_FRONT_LEFT, &raw);
  }
  if (hi <= lo)
    return 0;
  return static_cast<int>((raw - lo) * 100 / (hi - lo));
}

void AlsaMixer::setVolume(const Element& e, int percent)
{
  if (!e.elem || !e.hasVolume)
    return;
  percent = std::clamp(percent, 0, 100);

  long lo = 0, hi = 0;
  if (e.kind == Kind::CaptureVolume)
  {
    snd_mixer_selem_get_capture_volume_range(e.elem, &lo, &hi);
    long raw = lo + (hi - lo) * percent / 100;
    snd_mixer_selem_set_capture_volume_all(e.elem, raw);
  }
  else
  {
    snd_mixer_selem_get_playback_volume_range(e.elem, &lo, &hi);
    long raw = lo + (hi - lo) * percent / 100;
    snd_mixer_selem_set_playback_volume_all(e.elem, raw);
  }
}

bool AlsaMixer::switchOn(const Element& e) const
{
  if (!e.elem || !e.hasSwitch)
    return true; // no switch == always "on"

  int val = 1;
  if (e.kind == Kind::CaptureVolume || e.kind == Kind::CaptureSwitch)
    snd_mixer_selem_get_capture_switch(e.elem, SND_MIXER_SCHN_FRONT_LEFT, &val);
  else
    snd_mixer_selem_get_playback_switch(e.elem, SND_MIXER_SCHN_FRONT_LEFT, &val);
  return val != 0;
}

void AlsaMixer::setSwitchOn(const Element& e, bool on)
{
  if (!e.elem || !e.hasSwitch)
    return;
  if (e.kind == Kind::CaptureVolume || e.kind == Kind::CaptureSwitch)
    snd_mixer_selem_set_capture_switch_all(e.elem, on ? 1 : 0);
  else
    snd_mixer_selem_set_playback_switch_all(e.elem, on ? 1 : 0);
}
