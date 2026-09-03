#!/bin/sh
#
# Everything that can be checked without a device, in one command.
#
# Three kinds of thing live here: the C that is plain enough to run on a host -
# reading a screen, deciding a screen has gone flat, and refusing two settings
# on one pin; the console's own logic, run by node's test runner; and the paste
# tables, which are checked against the real keyboard layouts.
#
# The firmware itself is not built here - that needs ESP-IDF and takes minutes.
# Run this before you push and CI will rarely tell you anything you did not
# already know.
#
#   tools/test.sh            everything
#   tools/test.sh host       just the C
#   tools/test.sh web        just the console
#
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
what=${1:-all}

if [ "$what" = all ] || [ "$what" = host ]; then
    echo "== reading a screen as text =="
    sh "$here/components/kvm_screentext/test/run.sh"
    echo
    echo "== noticing a screen gone flat =="
    sh "$here/components/video_pipeline/test/run.sh"
    echo
    echo "== two things on one pin =="
    sh "$here/components/kvm_config/test/run.sh"
    echo
fi

if [ "$what" = all ] || [ "$what" = web ]; then
    echo "== the console =="
    (cd "$here/web" && npm run --silent typecheck && npm run --silent test)
    echo
    echo "== the paste tables =="
    node "$here/tools/check_layouts.mjs"
fi

echo "all good"
