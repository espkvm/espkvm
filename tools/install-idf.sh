#!/usr/bin/env bash
# One-shot ESP-IDF 6.0.1 install for esp32p4. Idempotent: safe to re-run.
set -euo pipefail

IDF_VERSION="${IDF_VERSION:-v6.0.1}"
IDF_DIR="${IDF_INSTALL_DIR:-$HOME/esp/esp-idf}"

if [ ! -d "$IDF_DIR/.git" ]; then
    mkdir -p "$(dirname "$IDF_DIR")"
    git clone -b "$IDF_VERSION" --depth 1 --recursive --shallow-submodules \
        https://github.com/espressif/esp-idf.git "$IDF_DIR"
fi

"$IDF_DIR/install.sh" esp32p4

# On Linux, install.sh expects cmake/ninja from the distro. Install them into the
# IDF python env instead, so no root and no system packages are required.
# shellcheck disable=SC1091
. "$IDF_DIR/export.sh" >/dev/null
command -v cmake >/dev/null && command -v ninja >/dev/null || python -m pip install --quiet cmake ninja

echo
echo "Done. Activate with:  . tools/env.sh"
