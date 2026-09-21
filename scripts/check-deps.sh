#!/bin/sh
# Fail if the GUI links anything that is not on the allowlist.
#
# An allowlist, for the same reason a manifest is one: a new NEEDED entry is a new thing the
# recipient has to already have, and it should have to be DECIDED rather than discovered. The
# whole point of removing Qt was to shorten this list; nothing should lengthen it by accident.
#
# Borrowed from CPU-Power's scripts/make-release.sh, which gates its release the same way.
set -eu

gui=${1:-build/audio-gui}
[ -f "$gui" ] || { echo "$0: no binary at $gui" >&2; exit 1; }

# libfontconfig is deliberately absent: FontStack loads the two bundled faces by path through
# FreeType and never asks fontconfig anything, so nothing drags it in.
allowed='libcairo.so.2 libfreetype.so.6 libX11.so.6 libasound.so.2
         libstdc++.so.6 libm.so.6 libgcc_s.so.1 libc.so.6
         ld-linux-x86-64.so.2 ld-linux-aarch64.so.1 ld-linux-armhf.so.3'

status=0
for lib in $(objdump -p "$gui" | awk '/NEEDED/{print $2}'); do
    case " $(echo $allowed) " in
        *" $lib "*) ;;
        *)
            echo "$0: $gui links $lib, which is not on the allowlist." >&2
            echo "  Either it belongs in the dependency list -- add it here, to" >&2
            echo "  packaging/build-deb.sh and to the README -- or it was linked by accident." >&2
            status=1
            ;;
    esac
done

[ "$status" -eq 0 ] && echo "$0: $gui links only allowlisted libraries"
exit $status
