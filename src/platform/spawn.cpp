// See spawn.h.

#include "spawn.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

extern char **environ;

namespace audiogui
{
namespace
{

// Build the child's environment as a NUL-terminated array BEFORE forking. Everything between
// fork() and execve() must be async-signal-safe, and allocating a string is not.
std::vector<std::string> buildEnv(const EnvPairs &extra)
{
    std::vector<std::string> env;
    for (char **e = environ; e && *e; ++e) {
        const char *eq = strchr(*e, '=');
        if (!eq) {
            env.emplace_back(*e);
            continue;
        }
        const std::string name(*e, static_cast<size_t>(eq - *e));
        // An entry we are about to set is dropped here rather than appended twice: putting a
        // duplicate in envp is not an error, but which one execve's child sees is left to the
        // libc, and the bridge reading the stale one would point it at the previous device.
        bool overridden = false;
        for (const auto &kv : extra) {
            if (kv.first == name) {
                overridden = true;
                break;
            }
        }
        if (!overridden)
            env.emplace_back(*e);
    }
    for (const auto &kv : extra)
        env.push_back(kv.first + "=" + kv.second);
    return env;
}

} // namespace

long spawnDetached(const std::string &program, const std::string &cwd, const EnvPairs &extraEnv)
{
    if (program.empty())
        return -1;

    const std::vector<std::string> envStrings = buildEnv(extraEnv);
    std::vector<char *> envp;
    envp.reserve(envStrings.size() + 1);
    for (const std::string &s : envStrings)
        envp.push_back(const_cast<char *>(s.c_str()));
    envp.push_back(nullptr);

    // The bridges take no arguments; everything they need arrives through the environment.
    char *argv[] = {const_cast<char *>(program.c_str()), nullptr};

    // O_CLOEXEC so the write end closes automatically on a successful exec. That is also how a
    // failed exec is distinguishable in principle -- the read would return 0 -- though here the
    // pid is written before the exec, so the pipe carries the pid rather than a status.
    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC) != 0)
        return -1;

    const pid_t mid = fork();
    if (mid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }

    if (mid == 0) {
        // --- intermediate child ---
        close(pfd[0]);

        // Its own session, so a signal to the GUI's process group does not reach the bridge.
        setsid();

        const pid_t gc = fork();
        if (gc == 0) {
            // --- grandchild: becomes the bridge ---
            close(pfd[1]);
            if (!cwd.empty() && chdir(cwd.c_str()) != 0) {
                // Not fatal: the bridges are started by absolute path and do not read anything
                // relative to the working directory. Carry on rather than lose the audio.
            }
            // The GUI may have blocked or ignored signals the bridge cares about; reset the ones
            // it uses for control so it starts from a known disposition.
            signal(SIGUSR1, SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            signal(SIGPIPE, SIG_DFL);

            execve(program.c_str(), argv, envp.data());
            _exit(127); // exec failed; the pid the parent got will simply not stay alive
        }

        // Report the grandchild's pid (or -1) and get out of the way, orphaning it onto init.
        const long out = (gc < 0) ? -1 : static_cast<long>(gc);
        ssize_t ignored = write(pfd[1], &out, sizeof(out));
        (void)ignored;
        close(pfd[1]);
        _exit(0);
    }

    // --- parent ---
    close(pfd[1]);

    long pid = -1;
    ssize_t n;
    do {
        n = read(pfd[0], &pid, sizeof(pid));
    } while (n < 0 && errno == EINTR);
    close(pfd[0]);

    // Reap the intermediate immediately; it has already exited or is about to. This is the only
    // wait this program ever does, and it is what keeps the process table clean.
    int status = 0;
    while (waitpid(mid, &status, 0) < 0 && errno == EINTR) {
    }

    if (n != static_cast<ssize_t>(sizeof(pid)))
        return -1;
    return pid;
}

} // namespace audiogui
