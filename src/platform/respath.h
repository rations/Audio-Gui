// Finding this program's own resources (currently: the two fonts) at run time.
//
// Resolution order:
//   1. $AUDIOGUI_RESOURCE_DIR, if set -- the development and packaging override;
//   2. the compile-time install prefix, AUDIOGUI_RESOURCE_DIR_DEFAULT;
//   3. "resources" beside the executable, which is what a build tree looks like.
//
// Returns an empty string if none of those contains a fonts directory. Callers treat that as
// "no bundled fonts", which FontStack already degrades to a system toy face for.
//
// THE ENVIRONMENT OVERRIDE EXISTS ONLY IN THIS BINARY. A substituted font is a FreeType attack
// surface, so a resource path taken from the environment is only acceptable in a process that has
// no privilege to lose. audio-gui runs entirely unprivileged, and the two bridge processes it
// spawns link no font code at all, so this override reaches nothing that could be hurt by it.

#pragma once

#include <string>

namespace audiogui
{

// Cached after the first call.
const std::string &resourceDir();

} // namespace audiogui
