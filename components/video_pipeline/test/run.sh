#!/bin/sh
# Build and run the flat-screen decision's tests on this machine - no device
# needed. It is plain C over a pixel buffer for exactly that reason.
set -e
here=$(dirname "$0")
out=$(mktemp -d)
cc -std=c11 -Wall -Wextra -O2 -I "$here/.." \
   "$here/test_capture_flat.c" "$here/../capture_flat_scan.c" -o "$out/test_capture_flat"
"$out/test_capture_flat"
rm -rf "$out"
