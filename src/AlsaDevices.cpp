// AlsaDevices.cpp
#include "AlsaDevices.h"

#include <alsa/asoundlib.h>

namespace
{

// Case-insensitive substring test used to classify cards/PCMs by name.
bool contains(const QString& haystack, const char* needle)
{
  return haystack.contains(QString::fromLatin1(needle), Qt::CaseInsensitive);
}

// Decide the category of one playback PCM from the card identity and PCM strings.
// The PCM *id* ("HDMI 0") is checked as well as the *name*, because the name is
// often the EDID monitor label (e.g. "Smart TV") with no "HDMI" keyword.
AlsaDevices::Category classify(const QString& cardName, const QString& cardLongName, const QString& components,
                               const QString& pcmName, const QString& pcmId)
{
  if (contains(pcmId, "HDMI") || contains(pcmName, "HDMI") || contains(pcmId, "DisplayPort")
      || contains(pcmName, "DisplayPort"))
    return AlsaDevices::Category::Hdmi;
  if (contains(components, "USB") || contains(cardName, "USB") || contains(cardLongName, "USB"))
    return AlsaDevices::Category::Usb;
  return AlsaDevices::Category::Internal;
}

} // namespace

namespace AlsaDevices
{

QVector<OutputDevice> enumerateOutputs()
{
  QVector<OutputDevice> out;

  snd_ctl_card_info_t* cardInfo = nullptr;
  snd_pcm_info_t* pcmInfo = nullptr;
  snd_ctl_card_info_alloca(&cardInfo);
  snd_pcm_info_alloca(&pcmInfo);

  int card = -1;
  while (snd_card_next(&card) == 0 && card >= 0)
  {
    char hwName[32];
    snprintf(hwName, sizeof(hwName), "hw:%d", card);

    snd_ctl_t* ctl = nullptr;
    if (snd_ctl_open(&ctl, hwName, 0) < 0)
      continue;

    if (snd_ctl_card_info(ctl, cardInfo) < 0)
    {
      snd_ctl_close(ctl);
      continue;
    }

    const QString cardId = QString::fromUtf8(snd_ctl_card_info_get_id(cardInfo));
    const QString cardName = QString::fromUtf8(snd_ctl_card_info_get_name(cardInfo));
    const QString cardLong = QString::fromUtf8(snd_ctl_card_info_get_longname(cardInfo));
    const QString components = QString::fromUtf8(snd_ctl_card_info_get_components(cardInfo));

    int dev = -1;
    while (snd_ctl_pcm_next_device(ctl, &dev) == 0 && dev >= 0)
    {
      snd_pcm_info_set_device(pcmInfo, static_cast<unsigned>(dev));
      snd_pcm_info_set_subdevice(pcmInfo, 0);
      snd_pcm_info_set_stream(pcmInfo, SND_PCM_STREAM_PLAYBACK);
      if (snd_ctl_pcm_info(ctl, pcmInfo) < 0)
        continue; // no playback on this PCM

      const QString pcmName = QString::fromUtf8(snd_pcm_info_get_name(pcmInfo));
      const QString pcmId = QString::fromUtf8(snd_pcm_info_get_id(pcmInfo));

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
      QString label = pcmName;
      if (label.isEmpty()) {
        label = pcmId;
        const int paren = label.lastIndexOf(QLatin1String(" ("));
        if (paren > 0)
          label.truncate(paren);
      }
      d.displayName = label.isEmpty() || label == cardName
                      ? cardName
                      : QStringLiteral("%1 — %2").arg(cardName, label);
      out.push_back(d);
    }

    snd_ctl_close(ctl);
  }

  return out;
}

QString deviceStringFor(const OutputDevice& dev)
{
  // Internal goes through "default" so the user's dmix/.asoundrc (software mixing
  // + the baseline we may have written) stays in effect. USB/HDMI get plughw so
  // the plug plugin converts the bridge's S16/stereo to whatever the card wants.
  if (dev.category == Category::Internal || dev.cardId.isEmpty())
    return QStringLiteral("default");
  return QStringLiteral("plughw:CARD=%1,DEV=%2").arg(dev.cardId).arg(dev.pcmIndex);
}

QString tokenFor(const OutputDevice& dev)
{
  if (dev.category == Category::Internal || dev.cardId.isEmpty())
    return QString();
  return QStringLiteral("%1:%2").arg(dev.cardId).arg(dev.pcmIndex);
}

QString cardIdFromToken(const QString& token)
{
  const int colon = token.indexOf(QLatin1Char(':'));
  return colon < 0 ? QString() : token.left(colon);
}

const OutputDevice* findByToken(const QVector<OutputDevice>& devices, const QString& token)
{
  for (const OutputDevice& d : devices)
    if (tokenFor(d) == token)
      return &d;
  return nullptr;
}

QString firstInternalCardId()
{
  const QVector<OutputDevice> devices = enumerateOutputs();
  for (const OutputDevice& d : devices)
    if (d.category == Category::Internal)
      return d.cardId;
  return QString();
}

} // namespace AlsaDevices
