// AlsaDevices.h
// Enumerate ALSA playback devices straight from libasound (snd_ctl_*), with no
// shelling out to `aplay`. Each detected output is classified Internal / USB /
// HDMI so the GUI can present a "switch output device" dropdown.
//
// Switching is done by the bridge (see BridgeManager): the chosen device's
// `PULSE_BRIDGE_ALSA_DEV` string is handed to pa-alsa-bridge. Internal maps to
// the ALSA "default" PCM (preserving the user's dmix/.asoundrc); USB and HDMI map
// to "plughw:CARD=<id>,DEV=<n>" so the plug plugin handles rate/format/channels.
#pragma once

#include <QString>
#include <QVector>

namespace AlsaDevices
{

enum class Category
{
  Internal, // analog / on-board card — routed through ALSA "default"
  Usb, // USB audio interface
  Hdmi // HDMI / DisplayPort output
};

struct OutputDevice
{
  QString cardId; // stable ALSA card id (e.g. "PCH"), from snd_ctl_card_info_get_id
  QString displayName; // human label for the dropdown
  int cardIndex = -1; // current card number (unstable across reboots/hotplug)
  int pcmIndex = 0; // playback PCM device number on the card
  Category category = Category::Internal;
};

// All playback-capable devices, in card/device order. Empty on a system with no
// sound cards.
QVector<OutputDevice> enumerateOutputs();

// The ALSA PCM string to hand the bridge for a device: "default" for Internal,
// "plughw:CARD=<id>,DEV=<n>" otherwise.
QString deviceStringFor(const OutputDevice& dev);

// Stable, persistable identity of a device: "" for Internal (== ALSA "default"),
// else "<cardId>:<pcmIndex>" (card id is stable; pcm index disambiguates the
// several HDMI outputs that share one card).
QString tokenFor(const OutputDevice& dev);

// The card id embedded in a token (the part before ':'), for opening the mixer.
// Empty token / Internal -> empty (caller uses "default").
QString cardIdFromToken(const QString& token);

// Find a device by its token; returns nullptr if absent (e.g. unplugged).
const OutputDevice* findByToken(const QVector<OutputDevice>& devices, const QString& token);

// Stable card id of the first Internal output, for the baseline ~/.asoundrc
// slave. Empty if no internal card was found.
QString firstInternalCardId();

} // namespace AlsaDevices
