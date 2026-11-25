#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tinyusb=${TINYUSB_DIR:-$here/../../../managed_components/espressif__tinyusb/src}
if [ ! -f "$tinyusb/tusb.c" ]; then
    echo "USB transport tests need TinyUSB sources. Build the firmware or set TINYUSB_DIR to its TinyUSB src directory." >&2
    exit 1
fi
out=$(mktemp -d)
trap 'rm -f "$out/sdkconfig.h" "$out/tusb_config.h" "$out/test_transport" "$out/config_descriptor.inc" "$out/reconnect.inc" "$out/test_descriptors"; rmdir "$out"' EXIT
echo '#define CONFIG_KVM_ENABLE_USB_VIDEO 1' > "$out/sdkconfig.h"
cat > "$out/tusb_config.h" <<'EOF'
#define CFG_TUSB_MCU OPT_MCU_NONE
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_HIGH_SPEED)
#define TUP_DCD_ENDPOINT_MAX 8
#define CFG_TUD_HID 4
#define CFG_TUD_AUDIO 1
EOF
cc -std=c11 -Wall -Wextra -Werror -I "$out" -I "$tinyusb" -I "$here/.." -I "$here/../include" \
    "$here/test_usb_video_transport.c" "$here/../usb_video_transport.c" \
    "$tinyusb/tusb.c" "$tinyusb/common/tusb_fifo.c" -o "$out/test_transport"
"$out/test_transport"

awk '
    /^\/\* ---- HID interfaces/ { reports = 1 }
    /^typedef struct __attribute__\(\(packed\)\)/ { reports = 0 }
    /^#if CFG_TUD_AUDIO/ && !audio_seen { descriptors = audio_seen = 1 }
    /^\/\* ---- report queue/ { descriptors = 0 }
    reports || descriptors { print }
' "$here/../../kvm_hid/usb_hid.c" > "$out/config_descriptor.inc"
awk '
    /^static esp_timer_handle_t s_reconnect_timer;/ { reconnect = 1 }
    /^\/\*/ { reconnect = 0 }
    /^static esp_timer_handle_t s_watchdog_timer;/ { print }
    /^void usb_hid_prepare_restart\(/ { restart = 1 }
    /^static void usb_video_visibility\(/ { visibility = 1 }
    reconnect || restart || visibility { print }
    /^}/ { restart = visibility = 0 }
' "$here/../../kvm_hid/usb_hid.c" > "$out/reconnect.inc"
cc -std=c11 -Wall -Wextra -Werror -I "$out" -I "$tinyusb" -I "$here/../include" \
    -I "$here/../../kvm_hid" "$here/test_usb_video_descriptors.c" -o "$out/test_descriptors"
"$out/test_descriptors"
