#!/bin/sh
# Build and run the pin-conflict rule's tests on this machine - no device
# needed. It is a set comparison over small structs, which is why it is
# separable from the settings store around it.
set -e
here=$(dirname "$0")
out=$(mktemp -d)
cc -std=c11 -Wall -Wextra -O2 -I "$here/.." \
   "$here/test_pin_conflict.c" "$here/../pin_conflict.c" -o "$out/test_pin_conflict"
"$out/test_pin_conflict"
rm -rf "$out"
