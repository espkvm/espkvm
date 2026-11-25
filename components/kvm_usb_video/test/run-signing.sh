#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if ! pkg-config --exists libsodium; then
    echo "USB video signing tests need host libsodium with pkg-config metadata." >&2
    exit 1
fi
out=$(mktemp -d)
trap 'rm -f "$out/test_signing" "$out/usb_video_signing.inc"; rmdir "$out"' EXIT
awk '
    /^static bool (store_identity\(void\)|authorize\(session_t \*s\))/ { copying = 1; count++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (count != 2 || copying) exit 1 }
' "$here/../usb_video.c" > "$out/usb_video_signing.inc"
# pkg-config returns a list of compiler arguments.
cc -std=c11 -Wall -Wextra -Werror -I "$out" "$here/test_usb_video_signing.c" \
    $(pkg-config --cflags --libs libsodium) -o "$out/test_signing"
"$out/test_signing"
