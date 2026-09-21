// See fs.h.

#include "fs.h"

#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace audiogui
{
namespace fs
{
namespace
{

std::string dirNameOf(const std::string &path)
{
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos)
        return ".";
    if (slash == 0)
        return "/";
    return path.substr(0, slash);
}

std::string envOr(const char *name, const std::string &fallback)
{
    const char *v = getenv(name);
    return (v && *v) ? std::string(v) : fallback;
}

} // namespace

bool exists(const std::string &path)
{
    struct stat st;
    return !path.empty() && stat(path.c_str(), &st) == 0;
}

bool readFile(const std::string &path, std::string &out)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    out.clear();
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        out.append(buf, n);
    const bool ok = ferror(f) == 0;
    fclose(f);
    return ok;
}

bool makeDirs(const std::string &path)
{
    if (path.empty())
        return false;
    struct stat st;
    if (stat(path.c_str(), &st) == 0)
        return S_ISDIR(st.st_mode);

    const std::string parent = dirNameOf(path);
    if (parent != path && parent != "/" && parent != "." && !makeDirs(parent))
        return false;

    // 0700: everything this program writes under the config and runtime directories is the
    // user's own, and the runtime files name a pid this user is about to signal.
    if (mkdir(path.c_str(), 0700) == 0)
        return true;
    return errno == EEXIST && stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool writeFileAtomic(const std::string &path, const std::string &body)
{
    const std::string dir = dirNameOf(path);
    if (!makeDirs(dir))
        return false;

    // The temporary must be in the SAME directory as the target: rename() is only atomic within
    // one filesystem, and /tmp is very often a different one from $HOME.
    std::string tmp = path + ".tmp-XXXXXX";
    std::vector<char> tpl(tmp.begin(), tmp.end());
    tpl.push_back('\0');

    const int fd = mkstemp(tpl.data());
    if (fd < 0)
        return false;
    tmp.assign(tpl.data());

    bool ok = true;
    size_t written = 0;
    while (ok && written < body.size()) {
        const ssize_t n = write(fd, body.data() + written, body.size() - written);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            ok = false;
            break;
        }
        written += static_cast<size_t>(n);
    }

    // fsync before the rename, not after: the rename can otherwise be durable while the contents
    // it points at are not, which is the crash that leaves a zero-length ~/.asoundrc behind.
    if (ok && fsync(fd) != 0)
        ok = false;
    if (close(fd) != 0)
        ok = false;

    // mkstemp makes the file 0600; these are ordinary config files and should be readable the
    // way the user's umask says the rest of their config is.
    if (ok && chmod(tmp.c_str(), 0644) != 0)
        ok = false;

    if (ok && rename(tmp.c_str(), path.c_str()) != 0)
        ok = false;

    if (!ok)
        unlink(tmp.c_str());
    return ok;
}

bool removeFile(const std::string &path)
{
    return unlink(path.c_str()) == 0 || errno == ENOENT;
}

const std::string &homeDir()
{
    static const std::string dir = [] {
        const char *h = getenv("HOME");
        if (h && *h)
            return std::string(h);
        if (const struct passwd *pw = getpwuid(getuid()))
            if (pw->pw_dir && *pw->pw_dir)
                return std::string(pw->pw_dir);
        return std::string("/");
    }();
    return dir;
}

const std::string &configHome()
{
    static const std::string dir = envOr("XDG_CONFIG_HOME", homeDir() + "/.config");
    return dir;
}

const std::string &runtimeDir()
{
    static const std::string dir = envOr("XDG_RUNTIME_DIR", "/tmp");
    return dir;
}

const std::string &exePath()
{
    static const std::string p = [] {
        char buf[4096];
        const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n <= 0)
            return std::string();
        buf[n] = '\0';
        return std::string(buf);
    }();
    return p;
}

const std::string &exeDir()
{
    static const std::string d = dirNameOf(exePath());
    return d;
}

} // namespace fs
} // namespace audiogui
