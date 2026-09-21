#!/bin/sh
# Regenerate the installable icon set from the single master artwork.
#
# Everything under packaging/icons/ is DERIVED. The master is icon.png at the top of the tree,
# and this script is the only thing that should ever write the derived files, so that a question
# about any one of them ("why is the 16px one sharpened?") has an answer in one place rather than
# in whatever command happened to be typed that day.
#
# It produces TWO things, for the two entirely separate ways a Linux desktop finds an icon:
#
#   packaging/icons/hicolor/<size>x<size>/apps/audio-gui.png
#       The icon theme. Found by NAME, without an extension, from the desktop entry's Icon= key,
#       following the freedesktop.org Icon Theme Specification. This is what a menu, a dock or an
#       application grid reads.
#
#   src/platform/appicon.h
#       The same artwork as a C++ array, published on the window itself as _NET_WM_ICON. This is
#       what a PLAIN WINDOW MANAGER reads -- it never looks at desktop entries at all, so without
#       this the titlebar and task list of the tool's actual target audience stay blank.
#
# The generated header is committed, not built, so that compiling this project needs a compiler
# and not an image toolchain.
#
# Usage:  scripts/make-icons.sh [master.png]
#
# Needs ImageMagick and python3. Writes only inside packaging/icons/ and src/platform/appicon.h.
# Touches nothing installed.

set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
master=${1:-$root/icon.png}
outdir=$root/packaging/icons/hicolor

# ImageMagick 7 calls it `magick`; 6 calls it `convert`. Both resize with alpha-weighted
# averaging, which is what keeps a dark halo off the edges of transparent artwork -- verified on
# this master by comparing a plain resize against an explicitly premultiplied one: zero differing
# pixels (`compare -metric AE`).
if command -v magick >/dev/null 2>&1; then
    im=magick
elif command -v convert >/dev/null 2>&1; then
    im=convert
else
    echo "$0: needs ImageMagick (magick or convert) on PATH" >&2
    exit 1
fi

[ -f "$master" ] || { echo "$0: no master artwork at $master" >&2; exit 1; }

# The standard application icon sizes. 512 is included because the master is big enough to supply
# it honestly; nothing here is ever upscaled, and adding a size larger than the master would be
# inventing detail rather than reproducing it.
sizes="16 22 24 32 48 64 128 256 512"

src_w=$($im identify -format '%w' "$master")
src_h=$($im identify -format '%h' "$master")
echo "master: $master (${src_w}x${src_h})"

for n in $sizes; do
    if [ "$n" -gt "$src_w" ] || [ "$n" -gt "$src_h" ]; then
        echo "  skip ${n}x${n}: larger than the master" >&2
        continue
    fi

    # Downscaling art this detailed to a menu icon loses the edges that make it legible, so the
    # small sizes get a light unsharp pass. It is not decoration: at 16px the headphone band and
    # the waveform bars either side of it merge into a grey ring without it. Above 64 the detail
    # survives on its own and sharpening only adds crunch, so it tapers to nothing.
    if [ "$n" -le 48 ]; then
        sharpen="-unsharp 0x0.6+0.8+0.02"
    elif [ "$n" -le 128 ]; then
        sharpen="-unsharp 0x0.5+0.4+0.02"
    else
        sharpen=""
    fi

    dst=$outdir/${n}x${n}/apps
    mkdir -p "$dst"

    # -extent squares the result even if a future master is not square, so the theme directory
    # never disagrees with the size in its own name. -strip drops timestamps and colour profiles
    # so that regenerating from an unchanged master produces byte-identical files.
    # shellcheck disable=SC2086
    $im "$master" \
        -filter Lanczos -resize "${n}x${n}" \
        $sharpen \
        -background none -gravity center -extent "${n}x${n}" \
        -strip -define png:compression-level=9 \
        "$dst/audio-gui.png"

    echo "  ${n}x${n} -> packaging/icons/hicolor/${n}x${n}/apps/audio-gui.png"
done

# ---------------------------------------------------------------------------------------------
# The same artwork again, as _NET_WM_ICON data compiled into the GUI.
#
# EWMH 1.5 section 5.12: "This is an array of 32bit packed CARDINAL ARGB with high byte being A,
# low byte being B. The first two cardinals are width, height. Data is in rows, left to right and
# top to bottom." Several images are concatenated, each preceded by its own width and height.
#
# THE PIXELS ARE NOT PREMULTIPLIED. The specification does not say so either way, which is the
# trap, because Cairo's ARGB32 -- what the rest of this program draws in -- is premultiplied. It
# was measured rather than assumed, by comparing a running client's published property against
# the artwork it was built from. ImageMagick's RGBA output is straight, non-premultiplied, so
# this pipeline needs no conversion -- but it needs to stay that way, hence the comment.
#
# Only the small end of the ladder is embedded. _NET_WM_ICON is read for titlebars, task lists,
# pagers and window switchers, which want 16 to 64 pixels; the big sizes are what the icon theme
# is for, and a window manager MAY scale. Embedding 128 alone would nearly triple this file for
# the case least likely to read it.
# ---------------------------------------------------------------------------------------------
embed="16 24 32 48 64"
header=$root/src/platform/appicon.h

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM

for n in $embed; do
    src=$outdir/${n}x${n}/apps/audio-gui.png
    [ -f "$src" ] || { echo "$0: $src missing -- nothing to embed" >&2; exit 1; }
    # RGBA: straight 8-bit samples, no premultiplication, no header, no stride.
    $im "$src" -depth 8 RGBA:"$tmp/$n.rgba"
done

MASTER_SHA=$(sha256sum "$master" | cut -d' ' -f1) EMBED="$embed" TMP="$tmp" \
    python3 "$root/scripts/emit-appicon.py" "$header"
