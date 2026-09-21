// Filesystem and path helpers, replacing the QFile / QDir / QSaveFile / QStandardPaths /
// QCoreApplication::applicationDirPath surface the Qt build used.
//
// THE ATOMIC WRITE IS THE LOAD-BEARING ONE. QSaveFile::commit() wrote to a temporary and renamed
// it into place, and two of its users here genuinely need that: ~/.asoundrc, which ALSA may open
// at any moment from any process on the machine, and the bridge control file, which the running
// bridge reads from its SIGUSR1 path. A plain truncate-and-write leaves a window in which either
// reader sees an empty or half-written file, and for the control file that window is exactly when
// it is being read, because the signal is sent immediately afterwards.

#pragma once

#include <string>

namespace audiogui
{
namespace fs
{

bool exists(const std::string &path);

// Whole-file read. Returns false if the file could not be opened.
bool readFile(const std::string &path, std::string &out);

// Write via a temporary in the same directory, fsync, then rename over the target. The rename is
// what makes a reader see either the old contents or the new ones and never a partial file.
// Creates parent directories as needed. Returns false having left the target untouched.
bool writeFileAtomic(const std::string &path, const std::string &body);

bool removeFile(const std::string &path);

// mkdir -p. Returns true if the directory exists afterwards.
bool makeDirs(const std::string &path);

// $HOME, or the passwd entry if it is unset.
const std::string &homeDir();

// $XDG_CONFIG_HOME, else ~/.config. This is the directory QStandardPaths::GenericConfigLocation
// resolved to, and the autostart entry still has to land in the same place.
const std::string &configHome();

// $XDG_RUNTIME_DIR, else /tmp -- the same fallback QStandardPaths::RuntimeLocation had. Holds the
// bridge pidfile and control file, both of which should not survive a reboot.
const std::string &runtimeDir();

// The running executable, from /proc/self/exe. BridgeManager resolves the bridge binaries
// relative to this, which is why CMake puts all three executables in one directory.
const std::string &exePath();
const std::string &exeDir();

} // namespace fs
} // namespace audiogui
