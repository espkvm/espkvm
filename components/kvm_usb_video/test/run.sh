#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cjson="${CJSON_DIR:-$here/../../../managed_components/espressif__cjson/cJSON}"
out=$(mktemp -d)
trap 'rm -f "$out/test_info" "$out/test_h264" "$out/test_ws" "$out/test_snapshots" "$out/usb_ws_handler.inc" "$out/usb_snapshot_handlers.inc"; rmdir "$out"' EXIT
if [ -f "$cjson/cJSON.c" ]; then
    set -- "-I$cjson" "$cjson/cJSON.c"
elif [ -z "${CJSON_DIR:-}" ] && command -v pkg-config >/dev/null && pkg-config --exists libcjson; then
    # pkg-config returns a list of compiler arguments.
    set -- $(pkg-config --cflags --libs libcjson)
else
    echo "USB video tests need cJSON. Build the firmware, install libcjson with pkg-config metadata, or set CJSON_DIR to the cJSON sources." >&2
    exit 1
fi
cc -std=c11 -Wall -Wextra -Werror -I "$here/.." \
    "$here/test_usb_video_info.c" "$here/../usb_video_info.c" "$@" -o "$out/test_info"
"$out/test_info"

cc -std=c11 -Wall -Wextra -Werror -I "$here/.." \
    "$here/test_usb_video_h264.c" "$here/../usb_video_h264.c" -o "$out/test_h264"
"$out/test_h264"

awk '
    /^esp_err_t ws_input_handler\(/ { found = 1 }
    found && /^[[:space:]]+const bool is_input =/ {
        print "    return ESP_FAIL;\n}"
        closed = 1
        exit
    }
    found { print }
    END { if (!closed) exit 1 }
' "$here/../../kvm_web/ws_server.c" > "$out/usb_ws_handler.inc"
cc -std=c11 -Wall -Wextra -Werror -I "$out" -I "$here/../include" \
    -I "$here/../../kvm_proto/include" "$here/test_usb_video_ws.c" -o "$out/test_ws"
"$out/test_ws"
echo "USB video WebSocket authorization tests passed"

awk '
    /^void capture_snapshot_usb_started\(void\)/ { stream = 1 }
    /^void capture_vnc_snapshot_end\(void\)/ { stream = 0 }
    /^esp_err_t capture_snapshot_jpeg\(/ { snapshot = 1 }
    stream || snapshot { print }
    END { if (!snapshot) exit 1 }
' "$here/../../video_pipeline/capture_snapshot.c" > "$out/usb_snapshot_handlers.inc"
cc -std=c11 -Wall -Wextra -Werror -I "$out" "$here/test_usb_snapshots.c" -o "$out/test_snapshots"
"$out/test_snapshots"
echo "USB video snapshot transition tests passed"
