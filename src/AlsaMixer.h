// AlsaMixer.h
// Thin wrapper over the libasound simple-mixer (snd_mixer_*) API.
//
// Exposes the handful of controls the GUI cares about (Master, Headphone,
// Speaker, Mic, Mic Boost, Capture, IEC958) as named "elements" with
// volume/mute/switch accessors. Controls that a given sound card does not have
// are simply reported absent, so the UI can omit them.
//
// EXTERNAL CHANGES ARE DELIVERED LIVE, and this is the one place the GUI's event
// loop is structurally involved. The Qt build wrapped each of the mixer's poll
// descriptors in a QSocketNotifier and emitted changed(). There is no Qt event
// loop any more, so the descriptors are handed out instead: main.cpp adds them to
// the X11Window's select() set, and the window calls handleEvents() when one of
// them fires. The class does no waiting of its own.
//
// THE DESCRIPTORS DO NOT SURVIVE A REOPEN. snd_mixer_close() invalidates them, and
// every Element::elem pointer with them, so a caller that reopens must re-register
// what pollDescriptors() then returns. onDescriptorsChanged is fired to say so.
#pragma once

#include <alsa/asoundlib.h>

#include <functional>
#include <string>
#include <vector>

class AlsaMixer
{
public:
    // What kind of control an element is, which decides which libasound calls apply.
    enum class Kind {
        PlaybackVolume, // has playback volume (+ usually a mute switch)
        CaptureVolume,  // has capture volume (Mic) (+ usually a capture switch)
        PlaybackSwitch, // switch-only, e.g. IEC958 on some cards
        CaptureSwitch   // switch-only capture enable (Capture on some cards)
    };

    struct Element {
        std::string name;  // ALSA simple-element name, e.g. "Master"
        std::string label; // human label for the UI
        Kind kind = Kind::PlaybackVolume;
        bool hasVolume = false;
        bool hasSwitch = false;           // mute (playback) or enable (capture) switch
        snd_mixer_elem_t *elem = nullptr; // owned by the mixer handle, not by us
    };

    AlsaMixer() = default;
    ~AlsaMixer();

    AlsaMixer(const AlsaMixer &) = delete;
    AlsaMixer &operator=(const AlsaMixer &) = delete;

    // Called after the hardware/mixer state changed underneath us (external edit).
    // Replaces the Qt `changed()` signal.
    std::function<void()> onChanged;

    // Called when the set of poll descriptors has been torn down and rebuilt, so the
    // caller can re-register them with its event loop. Fired by open() and reopen().
    std::function<void()> onDescriptorsChanged;

    // Open the given ALSA card ("default" by default). Returns false on failure.
    bool open(const std::string &card = "default");

    // Close any current card and open another one, rebuilding the element list and
    // poll descriptors. Used when the user switches output device. Returns false on
    // failure (leaving the mixer closed).
    bool reopen(const std::string &card);

    bool isOpen() const
    {
        return m_handle != nullptr;
    }

    const std::vector<Element> &elements() const
    {
        return m_elements;
    }

    // The descriptors to wait on. Valid only until the next open/reopen/close.
    const std::vector<int> &pollDescriptors() const
    {
        return m_fds;
    }

    // Drain ALSA's event queue and fire onChanged. Call when a poll descriptor is
    // readable.
    void handleEvents();

    // Volume is 0..100 (percent of the control's range). Clamped internally.
    int volume(const Element &e) const;
    void setVolume(const Element &e, int percent);

    // For playback elements the switch is "not muted"; for capture it is "enabled".
    bool switchOn(const Element &e) const;
    void setSwitchOn(const Element &e, bool on);

private:
    void buildElements();
    void collectDescriptors();
    void close();

    snd_mixer_t *m_handle = nullptr;
    std::string m_card;
    std::vector<Element> m_elements;
    std::vector<int> m_fds;
};
