#!/usr/bin/env bash
# Fetch a small bootable image and write it into the on-flash "rescue" partition,
# so the target can boot it over USB with no microSD card. The default is
# netboot.xyz - a tiny iPXE image that boots a menu of rescue systems and
# installers over the network.
#
#   tools/fetch-rescue.sh                 # download netboot.xyz and write it
#   tools/fetch-rescue.sh path/to.img     # write a local image instead
#   tools/fetch-rescue.sh https://.../x   # download a specific URL
#
# The image must be a raw bootable disk image (an isohybrid .iso, an iPXE .usb,
# a DOS floppy) and fit the partition - see RESCUE_MAX below. It is written over
# the serial/JTAG cable with parttool.py, which looks up the partition by name,
# so source the ESP-IDF environment first:  . tools/env.sh
#
# Prefer no cable? Upload the same image from the console instead:
#   Settings -> Virtual media -> Built-in rescue image -> Upload.
#
# iPXE is GPL, so it is fetched at run time rather than committed to this repo.
set -euo pipefail

# Keep this in step with the "rescue" size in partitions.csv (4 MB).
RESCUE_MAX=$((4 * 1024 * 1024))
PORT="${PORT:-/dev/ttyACM0}"
DEFAULT_URL="${RESCUE_URL:-https://github.com/netbootxyz/netboot.xyz/releases/latest/download/netboot.xyz.iso}"

src="${1:-$DEFAULT_URL}"
tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT

if [ -f "$src" ]; then
    cp "$src" "$tmp"
    echo "Using local image: $src"
else
    echo "Downloading $src"
    curl -fL --progress-bar "$src" -o "$tmp"
fi

size=$(wc -c < "$tmp")
printf 'Image is %s bytes (%s KB).\n' "$size" "$((size / 1024))"
if [ "$size" -gt "$RESCUE_MAX" ]; then
    echo "error: image is larger than the ${RESCUE_MAX} byte rescue partition." >&2
    echo "       Use a smaller image (plain iPXE, memtest, a DOS floppy)." >&2
    exit 1
fi

if ! command -v parttool.py >/dev/null 2>&1; then
    echo "error: parttool.py not found - source the ESP-IDF environment first:" >&2
    echo "       . tools/env.sh" >&2
    exit 1
fi

echo "Writing to the 'rescue' partition on $PORT ..."
parttool.py --port "$PORT" write_partition --partition-name=rescue --input "$tmp"

cat <<'EOF'

Done. To boot the target from it:
  1. Console -> Settings -> Virtual media -> pick "Built-in rescue image".
  2. Turn on "Expose virtual media" (this restarts the USB device).
  3. Boot the target and choose the USB drive in its boot menu.
EOF
