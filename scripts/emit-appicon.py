#!/usr/bin/env python3
"""Emit src/platform/appicon.h from raw RGBA dumps of the icon ladder.

Called only by scripts/make-icons.sh, which supplies MASTER_SHA, EMBED and TMP in the
environment and the output path as argv[1]. Split out of that script rather than inlined so that
the shell stays readable and this stays greppable.

The output is the _NET_WM_ICON payload, already in the property's own byte layout: for each
image, width, height, then width*height packed ARGB cardinals, rows left to right and top to
bottom (EWMH 1.5 section 5.12). Images are simply concatenated.

Input is straight, NON-premultiplied RGBA -- which is what the property wants and what
ImageMagick's RGBA: output gives. Cairo's ARGB32 is premultiplied and would be wrong here.
"""

import os
import sys

out = sys.argv[1]
sizes = [int(x) for x in os.environ["EMBED"].split()]
tmp = os.environ["TMP"]

words = []
for n in sizes:
    with open(f"{tmp}/{n}.rgba", "rb") as f:
        raw = f.read()
    if len(raw) != n * n * 4:
        raise SystemExit(f"{n}x{n}: expected {n * n * 4} bytes of RGBA, got {len(raw)}")
    words.append(n)  # width
    words.append(n)  # height
    for i in range(0, len(raw), 4):
        r, g, b, a = raw[i], raw[i + 1], raw[i + 2], raw[i + 3]
        words.append((a << 24) | (r << 16) | (g << 8) | b)

rows = [
    "    " + " ".join(f"0x{w:08x}," for w in words[i:i + 8])
    for i in range(0, len(words), 8)
]
ladder = ", ".join(f"{n}x{n}" for n in sizes)
body = "\n".join(rows)

with open(out, "w", encoding="utf-8") as f:
    f.write(f"""// GENERATED FILE -- DO NOT EDIT BY HAND.
//
// Regenerate with:  scripts/make-icons.sh
// Master artwork:   icon.png
//   sha256 {os.environ["MASTER_SHA"]}
// Sizes embedded:   {ladder}
//
// The application icon, in the exact byte layout the _NET_WM_ICON property wants, so that
// publishing it is one XChangeProperty with no conversion step left to get wrong.
//
// EWMH 1.5 section 5.12: an array of 32-bit packed CARDINAL ARGB, high byte alpha and low byte
// blue; each image is preceded by its width and height; rows run left to right and top to
// bottom. Several images are concatenated and the window manager picks one, and MAY scale it.
//
// THE PIXELS ARE NOT PREMULTIPLIED -- unlike every other surface in this program, which is Cairo
// ARGB32 and is. The specification is silent on the point, so it was measured against a running
// client rather than assumed: a 128x128 icon published by VLC held 252 pixels whose colour
// exceeded their own alpha, which premultiplied data cannot contain, and its semi-transparent
// pixels matched the source PNG's values exactly. Anything that ever routes this artwork through
// a Cairo surface must un-premultiply on the way back out.

#pragma once

#include <cstddef>
#include <cstdint>

namespace audiogui
{{
namespace appicon
{{

inline constexpr std::uint32_t kData[] = {{
{body}
}};

inline constexpr std::size_t kWords = sizeof(kData) / sizeof(kData[0]);

}} // namespace appicon
}} // namespace audiogui
""")

print(f"  {ladder} -> src/platform/appicon.h ({len(words)} cardinals)")
