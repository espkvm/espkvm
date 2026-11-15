#!/bin/sh
# The VeNCrypt negotiation on this machine, no device needed.
set -e
here=$(dirname "$0")
out=$(mktemp -d)
cc -std=c11 -Wall -Wextra -O2 -I "$here/.." \
   "$here/test_vencrypt.c" "$here/../vencrypt_protocol.c" -o "$out/test_vencrypt"
"$out/test_vencrypt"
rm -rf "$out"
