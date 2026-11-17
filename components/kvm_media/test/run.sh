#!/bin/sh
# Build and run the media read-ahead cache on this machine - no device needed.
set -e
here=$(dirname "$0")
out=$(mktemp -d)
cc -std=c11 -Wall -Wextra -O2 -I "$here/.." \
   "$here/test_cache.c" "$here/../media_cache.c" -o "$out/test_cache"
"$out/test_cache"
rm -rf "$out"
