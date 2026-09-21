// See config.h.

#include "config.h"

#include "platform/fs.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace audiogui
{
namespace config
{
namespace
{

// Sections and keys are held in the order they were read, and new ones are appended, so saving a
// file round-trips it instead of reordering somebody's hand edit.
struct Section {
    std::string name;
    std::vector<std::pair<std::string, std::string>> entries;
};

struct Store {
    std::vector<Section> sections;
    bool loaded = false;
};

Store &store()
{
    static Store s;
    return s;
}

std::string trim(const std::string &s)
{
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return std::string();
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

void splitKey(const std::string &key, std::string &section, std::string &name)
{
    const size_t slash = key.find('/');
    if (slash == std::string::npos) {
        section = "General"; // what QSettings calls the sectionless group
        name = key;
    } else {
        section = key.substr(0, slash);
        name = key.substr(slash + 1);
    }
}

void load()
{
    Store &st = store();
    if (st.loaded)
        return;
    st.loaded = true;

    std::string body;
    if (!fs::readFile(path(), body))
        return;

    std::string current; // the sectionless preamble, if any, belongs to no section
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t nl = body.find('\n', pos);
        if (nl == std::string::npos)
            nl = body.size();
        const std::string line = trim(body.substr(pos, nl - pos));
        pos = nl + 1;

        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;

        if (line.front() == '[' && line.back() == ']') {
            current = line.substr(1, line.size() - 2);
            if (std::none_of(st.sections.begin(), st.sections.end(),
                             [&](const Section &s) { return s.name == current; }))
                st.sections.push_back({current, {}});
            continue;
        }

        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;

        const std::string k = trim(line.substr(0, eq));
        const std::string v = trim(line.substr(eq + 1));
        if (k.empty())
            continue;

        if (current.empty()) {
            current = "General";
            if (std::none_of(st.sections.begin(), st.sections.end(),
                             [&](const Section &s) { return s.name == current; }))
                st.sections.push_back({current, {}});
        }
        for (Section &s : st.sections) {
            if (s.name != current)
                continue;
            auto it = std::find_if(s.entries.begin(), s.entries.end(),
                                   [&](const auto &e) { return e.first == k; });
            if (it != s.entries.end())
                it->second = v;
            else
                s.entries.emplace_back(k, v);
            break;
        }
    }
}

const std::string *find(const std::string &key)
{
    load();
    std::string sec, name;
    splitKey(key, sec, name);
    for (const Section &s : store().sections) {
        if (s.name != sec)
            continue;
        for (const auto &e : s.entries)
            if (e.first == name)
                return &e.second;
        return nullptr;
    }
    return nullptr;
}

void save()
{
    std::string out;
    bool first = true;
    for (const Section &s : store().sections) {
        if (s.entries.empty())
            continue;
        if (!first)
            out += '\n';
        first = false;
        out += '[' + s.name + "]\n";
        for (const auto &e : s.entries)
            out += e.first + '=' + e.second + '\n';
    }
    fs::writeFileAtomic(path(), out);
}

void put(const std::string &key, const std::string &value)
{
    load();
    std::string sec, name;
    splitKey(key, sec, name);

    Section *target = nullptr;
    for (Section &s : store().sections) {
        if (s.name == sec) {
            target = &s;
            break;
        }
    }
    if (!target) {
        store().sections.push_back({sec, {}});
        target = &store().sections.back();
    }

    auto it = std::find_if(target->entries.begin(), target->entries.end(),
                           [&](const auto &e) { return e.first == name; });
    if (it != target->entries.end())
        it->second = value;
    else
        target->entries.emplace_back(name, value);

    save();
}

} // namespace

const std::string &path()
{
    static const std::string p = fs::configHome() + "/AudioGui/AudioGui.conf";
    return p;
}

int getInt(const std::string &key, int fallback)
{
    const std::string *v = find(key);
    if (!v || v->empty())
        return fallback;
    // strtol rather than stoi: a corrupt or hand-edited value should fall back to the default,
    // not throw out of a settings read.
    char *end = nullptr;
    const long n = std::strtol(v->c_str(), &end, 10);
    if (end == v->c_str() || (end && *end != '\0'))
        return fallback;
    return static_cast<int>(n);
}

std::string getString(const std::string &key, const std::string &fallback)
{
    const std::string *v = find(key);
    return v ? *v : fallback;
}

void setInt(const std::string &key, int value)
{
    put(key, std::to_string(value));
}

void setString(const std::string &key, const std::string &value)
{
    put(key, value);
}

} // namespace config
} // namespace audiogui
