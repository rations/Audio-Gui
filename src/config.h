// Persisted settings, replacing QSettings.
//
// SAME FILE, SAME FORMAT, SAME KEYS as the Qt build: ~/.config/AudioGui/AudioGui.conf, in the INI
// dialect QSettings wrote, with the keys "appearance/mode", "appearance/accent", "routing/mode"
// and "routing/device". That is deliberate -- an existing install keeps its dark/light choice,
// its accent and its routing mode across the upgrade, instead of silently reverting to defaults
// because the toolkit changed underneath it.
//
// Keys not written by this program are preserved on save rather than dropped, so a file that has
// been hand-edited, or written by a future version, does not lose anything by being loaded and
// stored once here.

#pragma once

#include <string>

namespace audiogui
{
namespace config
{

// ~/.config/AudioGui/AudioGui.conf, honouring $XDG_CONFIG_HOME.
const std::string &path();

// `key` is "section/name", exactly as it was handed to QSettings.
int getInt(const std::string &key, int fallback);
std::string getString(const std::string &key, const std::string &fallback);

// Both write the file immediately, as QSettings did. A failed write is not reported: a setting
// that could not be persisted is not worth interrupting the user over, and the program carries on
// with the value in memory either way.
void setInt(const std::string &key, int value);
void setString(const std::string &key, const std::string &value);

} // namespace config
} // namespace audiogui
