// Starting a bridge process, replacing QProcess::startDetached + QProcessEnvironment.
//
// "DETACHED" HAS THREE SEPARATE REQUIREMENTS, and it is worth naming them because meeting two of
// them and not the third is the bug that shows up a week later:
//
//   1. The bridge must SURVIVE THE GUI. Audio for PulseAudio-compat apps has to keep working when
//      the window is closed, which is the whole reason the bridge is a separate process.
//   2. It must NOT BECOME A ZOMBIE while the GUI is still running. The GUI never waits on it, so
//      a plain fork+exec child that exits would sit in the process table until the GUI quit --
//      and pidAlive(), which is how BridgeManager decides whether a bridge is still up, cannot
//      tell a zombie from a running process (kill(pid, 0) succeeds for both).
//   3. It must be in its OWN SESSION, so a signal sent to the GUI's process group -- a Ctrl-C in
//      the terminal it was started from -- does not take the audio down with it.
//
// The double fork is what satisfies all three: the intermediate child calls setsid() and forks
// again, then exits immediately, which orphans the grandchild onto init (no zombie, survives the
// GUI) while the GUI reaps only the short-lived intermediate. The grandchild's pid comes back
// through a pipe, because the GUI needs it for the pidfile and for the live-switch SIGUSR1.

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace audiogui
{

// Environment entries to add to (or replace in) the inherited environment.
using EnvPairs = std::vector<std::pair<std::string, std::string>>;

// Returns the spawned process's pid, or -1 if it could not be started. `cwd` may be empty.
//
// A failure to exec is reported as -1: the intermediate child reports the grandchild's pid only
// after the fork, so a bad path shows up as a process that exists briefly and exits 127. The
// caller re-checks liveness before trusting the pid.
long spawnDetached(const std::string &program, const std::string &cwd, const EnvPairs &extraEnv);

} // namespace audiogui
