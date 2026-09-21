// AlsaDevices.cpp
#include "AlsaDevices.h"

#include <alsa/asoundlib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace
{

// Case-insensitive substring test used to classify cards/PCMs by name. Replaces
// QString::contains(..., Qt::CaseInsensitive); ASCII-only folding is all that is
// wanted here, because every keyword matched against it is ASCII.
bool contains(const std::string &haystack, const char *needle)
{
    const size_t n = std::char_traits<char>::length(needle);
    if (n == 0)
        return true;
    if (haystack.size() < n)
        return false;
    const auto eq = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    };
    return std::search(haystack.begin(), haystack.end(), needle, needle + n, eq) != haystack.end();
}

// snd_*_get_* can return NULL; QString::fromUtf8(nullptr) yielded an empty string
// and std::string's constructor would be undefined behaviour, so it is checked here.
std::string str(const char *s)
{
    return s ? std::string(s) : std::string();
}

// Decide the category of one playback PCM from the card identity and PCM strings.
// The PCM *id* ("HDMI 0") is checked as well as the *name*, because the name is
// often the EDID monitor label (e.g. "Smart TV") with no "HDMI" keyword.
AlsaDevices::Category classify(const std::string &cardName, const std::string &cardLongName,
                               const std::string &components, const std::string &pcmName,
                               const std::string &pcmId)
{
    if (contains(pcmId, "HDMI") || contains(pcmName, "HDMI") || contains(pcmId, "DisplayPort") ||
        contains(pcmName, "DisplayPort"))
        return AlsaDevices::Category::Hdmi;
    if (contains(components, "USB") || contains(cardName, "USB") || contains(cardLongName, "USB"))
        return AlsaDevices::Category::Usb;
    return AlsaDevices::Category::Internal;
}

} // namespace

namespace AlsaDevices
{

std::vector<OutputDevice> enumerateOutputs()
{
    std::vector<OutputDevice> out;

    snd_ctl_card_info_t *cardInfo = nullptr;
    snd_pcm_info_t *pcmInfo = nullptr;
    snd_ctl_card_info_alloca(&cardInfo);
    snd_pcm_info_alloca(&pcmInfo);

    int card = -1;
    while (snd_card_next(&card) == 0 && card >= 0) {
        char hwName[32];
        snprintf(hwName, sizeof(hwName), "hw:%d", card);

        snd_ctl_t *ctl = nullptr;
        if (snd_ctl_open(&ctl, hwName, 0) < 0)
            continue;

        if (snd_ctl_card_info(ctl, cardInfo) < 0) {
            snd_ctl_close(ctl);
            continue;
        }

        const std::string cardId = str(snd_ctl_card_info_get_id(cardInfo));
        const std::string cardName = str(snd_ctl_card_info_get_name(cardInfo));
        const std::string cardLong = str(snd_ctl_card_info_get_longname(cardInfo));
        const std::string components = str(snd_ctl_card_info_get_components(cardInfo));

        int dev = -1;
        while (snd_ctl_pcm_next_device(ctl, &dev) == 0 && dev >= 0) {
            snd_pcm_info_set_device(pcmInfo, static_cast<unsigned>(dev));
            snd_pcm_info_set_subdevice(pcmInfo, 0);
            snd_pcm_info_set_stream(pcmInfo, SND_PCM_STREAM_PLAYBACK);
            if (snd_ctl_pcm_info(ctl, pcmInfo) < 0)
                continue; // no playback on this PCM

            const std::string pcmName = str(snd_pcm_info_get_name(pcmInfo));
            const std::string pcmId = str(snd_pcm_info_get_id(pcmInfo));

            OutputDevice d;
            d.cardId = cardId;
            d.cardIndex = card;
            d.pcmIndex = dev;
            d.category = classify(cardName, cardLong, components, pcmName, pcmId);
            // Card name is the primary label; append the PCM name when it adds detail
            // (e.g. multiple HDMI outputs on one card).
            // pcmName is empty for sof-hda-dsp (aplay shows "[]"); fall back to pcmId
            // which carries meaningful strings like "HDA Analog", "HDMI1". Strip the
            // trailing " (*)" suffix that ALSA appends to pcmId strings.
            std::string label = pcmName;
            if (label.empty()) {
                label = pcmId;
                const size_t paren = label.rfind(" (");
                if (paren != std::string::npos && paren > 0)
                    label.resize(paren);
            }
            d.displayName =
                (label.empty() || label == cardName) ? cardName : cardName + " — " + label;
            out.push_back(d);
        }

        snd_ctl_close(ctl);
    }

    return out;
}

std::string deviceStringFor(const OutputDevice &dev)
{
    // Internal goes through "default" so the user's dmix/.asoundrc (software mixing
    // + the baseline we may have written) stays in effect. USB/HDMI get plughw so
    // the plug plugin converts the bridge's S16/stereo to whatever the card wants.
    if (dev.category == Category::Internal || dev.cardId.empty())
        return "default";
    return "plughw:CARD=" + dev.cardId + ",DEV=" + std::to_string(dev.pcmIndex);
}

std::string tokenFor(const OutputDevice &dev)
{
    if (dev.category == Category::Internal || dev.cardId.empty())
        return std::string();
    return dev.cardId + ":" + std::to_string(dev.pcmIndex);
}

std::string cardIdFromToken(const std::string &token)
{
    const size_t colon = token.find(':');
    return colon == std::string::npos ? std::string() : token.substr(0, colon);
}

const OutputDevice *findByToken(const std::vector<OutputDevice> &devices, const std::string &token)
{
    for (const OutputDevice &d : devices)
        if (tokenFor(d) == token)
            return &d;
    return nullptr;
}

std::string firstInternalCardId()
{
    const std::vector<OutputDevice> devices = enumerateOutputs();
    for (const OutputDevice &d : devices)
        if (d.category == Category::Internal)
            return d.cardId;
    return std::string();
}

} // namespace AlsaDevices
