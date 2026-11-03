#!/bin/sh
#
# Everything that can be checked without a device, in one command.
#
# Three kinds of thing live here: the C that is plain enough to run on a host -
# reading a screen, deciding a screen has gone flat, and refusing two settings
# on one pin; the console's own logic, run by node's test runner; the paste
# tables, which are checked against the real keyboard layouts; and the Worker
# behind check.espkvm.io, against node's own SQLite.
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
    echo "== a recording as a transport stream, and its keystroke subtitles =="
    sh "$here/components/kvm_record/test/run.sh"
    echo
    echo "== two things on one pin, and device profiles =="
    sh "$here/components/kvm_config/test/run.sh"
    echo
    echo "== screen areas =="
    sh "$here/components/kvm_regions/test/run.sh"
    echo
    echo "== reading a runbook =="
    sh "$here/components/kvm_runbook/test/run.sh"
    echo
    echo "== a cron schedule =="
    sh "$here/components/kvm_sched/test/run.sh"
    echo
    echo "== a Telegram reply =="
    sh "$here/components/kvm_notify/test/run.sh"
    echo
    echo "== the optical drive's MMC replies =="
    sh "$here/components/kvm_hid/test/run.sh"
    echo
    echo "== one-time passwords =="
    sh "$here/components/kvm_web/test/run.sh"
    echo
    echo "== WebRTC: SRTP, STUN, H.264 over RTP, SDP =="
    sh "$here/components/kvm_webrtc/test/run.sh"
    echo
    echo "== VeNCrypt: version and TLS subtype negotiation =="
    sh "$here/components/kvm_vnc/test/run.sh"
    echo
fi

if [ "$what" = all ] || [ "$what" = web ]; then
    echo "== the console =="
    (cd "$here/web" && npm run --silent typecheck && npm run --silent test)
    echo
    echo "== the paste tables =="
    node "$here/tools/check_layouts.mjs"
    echo
    echo "== the firmware's copy of the layouts, for subtitles =="
    node "$here/tools/gen_keymap.mjs" --check
fi

echo "all good"
