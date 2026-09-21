// AlsaMixer.cpp
#include "AlsaMixer.h"

#include <algorithm>

namespace
{

struct Wanted {
    const char *name;
    const char *label;
    AlsaMixer::Kind kind;
};

// The controls the GUI offers, in the order it shows them. A card that does not
// have one simply does not get it: buildElements keeps only what it finds.
//
// THIS TABLE IS ALSO THE WINDOW'S SIZE BOUND. geometry.h asserts its layout against
// kMaxStrips = 5 and kMaxSwitches = 2, which are the counts of the volume and the
// switch-only entries below. Adding a row here means changing those.
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

AlsaMixer::~AlsaMixer()
{
    close();
}

void AlsaMixer::close()
{
    m_fds.clear();
    m_elements.clear();
    if (m_handle) {
        // This invalidates every descriptor in m_fds and every Element::elem pointer;
        // both are cleared above so nothing can reach them afterwards.
        snd_mixer_close(m_handle);
        m_handle = nullptr;
    }
}

bool AlsaMixer::reopen(const std::string &card)
{
    close();
    return open(card);
}

bool AlsaMixer::open(const std::string &card)
{
    m_card = card;

    if (snd_mixer_open(&m_handle, 0) < 0) {
        m_handle = nullptr;
        return false;
    }
    if (snd_mixer_attach(m_handle, card.c_str()) < 0 ||
        snd_mixer_selem_register(m_handle, nullptr, nullptr) < 0 || snd_mixer_load(m_handle) < 0) {
        snd_mixer_close(m_handle);
        m_handle = nullptr;
        return false;
    }

    buildElements();
    collectDescriptors();
    if (onDescriptorsChanged)
        onDescriptorsChanged();
    return true;
}

void AlsaMixer::buildElements()
{
    m_elements.clear();

    snd_mixer_selem_id_t *sid = nullptr;
    snd_mixer_selem_id_alloca(&sid);

    for (const Wanted &w : kWanted) {
        snd_mixer_selem_id_set_index(sid, 0);
        snd_mixer_selem_id_set_name(sid, w.name);
        snd_mixer_elem_t *elem = snd_mixer_find_selem(m_handle, sid);
        if (!elem)
            continue;

        Element e;
        e.name = w.name;
        e.label = w.label;
        e.kind = w.kind;
        e.elem = elem;

        switch (w.kind) {
            case Kind::PlaybackVolume:
                e.hasVolume = snd_mixer_selem_has_playback_volume(elem) != 0;
                e.hasSwitch = snd_mixer_selem_has_playback_switch(elem) != 0;
                break;
            case Kind::CaptureVolume:
                e.hasVolume = snd_mixer_selem_has_capture_volume(elem) != 0;
                e.hasSwitch = snd_mixer_selem_has_capture_switch(elem) != 0;
                break;
            case Kind::PlaybackSwitch:
                e.hasSwitch = snd_mixer_selem_has_playback_switch(elem) != 0;
                break;
            case Kind::CaptureSwitch:
                e.hasSwitch = snd_mixer_selem_has_capture_switch(elem) != 0;
                break;
        }

        // Keep only elements that actually expose something we can drive.
        if (e.hasVolume || e.hasSwitch)
            m_elements.push_back(e);
    }
}

void AlsaMixer::collectDescriptors()
{
    m_fds.clear();

    int count = snd_mixer_poll_descriptors_count(m_handle);
    if (count <= 0)
        return;

    std::vector<struct pollfd> pfds(static_cast<size_t>(count));
    count = snd_mixer_poll_descriptors(m_handle, pfds.data(), static_cast<unsigned>(count));
    if (count <= 0)
        return;

    m_fds.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        m_fds.push_back(pfds[static_cast<size_t>(i)].fd);
}

void AlsaMixer::handleEvents()
{
    if (!m_handle)
        return;
    // Drain ALSA's event queue, then let the UI re-read current values.
    snd_mixer_handle_events(m_handle);
    if (onChanged)
        onChanged();
}

int AlsaMixer::volume(const Element &e) const
{
    if (!e.elem || !e.hasVolume)
        return 0;

    long lo = 0, hi = 0, raw = 0;
    if (e.kind == Kind::CaptureVolume) {
        snd_mixer_selem_get_capture_volume_range(e.elem, &lo, &hi);
        snd_mixer_selem_get_capture_volume(e.elem, SND_MIXER_SCHN_FRONT_LEFT, &raw);
    } else {
        snd_mixer_selem_get_playback_volume_range(e.elem, &lo, &hi);
        snd_mixer_selem_get_playback_volume(e.elem, SND_MIXER_SCHN_FRONT_LEFT, &raw);
    }
    if (hi <= lo)
        return 0;
    return static_cast<int>((raw - lo) * 100 / (hi - lo));
}

void AlsaMixer::setVolume(const Element &e, int percent)
{
    if (!e.elem || !e.hasVolume)
        return;
    percent = std::clamp(percent, 0, 100);

    long lo = 0, hi = 0;
    if (e.kind == Kind::CaptureVolume) {
        snd_mixer_selem_get_capture_volume_range(e.elem, &lo, &hi);
        long raw = lo + (hi - lo) * percent / 100;
        snd_mixer_selem_set_capture_volume_all(e.elem, raw);
    } else {
        snd_mixer_selem_get_playback_volume_range(e.elem, &lo, &hi);
        long raw = lo + (hi - lo) * percent / 100;
        snd_mixer_selem_set_playback_volume_all(e.elem, raw);
    }
}

bool AlsaMixer::switchOn(const Element &e) const
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

void AlsaMixer::setSwitchOn(const Element &e, bool on)
{
    if (!e.elem || !e.hasSwitch)
        return;
    if (e.kind == Kind::CaptureVolume || e.kind == Kind::CaptureSwitch)
        snd_mixer_selem_set_capture_switch_all(e.elem, on ? 1 : 0);
    else
        snd_mixer_selem_set_playback_switch_all(e.elem, on ? 1 : 0);
}
