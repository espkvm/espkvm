#!/bin/sh
# Build and run the scanner's tests on this machine - no device needed. The
# scanner is plain C over a pixel buffer for exactly this reason.
set -e
here=$(dirname "$0")
out=$(mktemp -d)
cc -std=c11 -Wall -Wextra -O2 \
   -I "$here/../include" -I "$here/.." \
   "$here/test_screentext.c" "$here/../screentext.c" -o "$out/test_screentext"
"$out/test_screentext" "$here/../fonts/ibm_vga_8x16.bin" \
                       "$here/../fonts/pcdos_cp437_8x16.bin" \
                       "$here/../fonts/uefi_hii_8x19.txt"
rm -rf "$out"
