#!/usr/bin/env bash
# Source this to get an ESP-IDF 6.0.1 environment for esp32p4:
#   . tools/env.sh
# Override the IDF location with IDF_INSTALL_DIR if it is not ~/esp/esp-idf.

_espkvm_idf_dir="${IDF_INSTALL_DIR:-$HOME/esp/esp-idf}"

if [ ! -f "$_espkvm_idf_dir/export.sh" ]; then
    echo "ESP-IDF not found at $_espkvm_idf_dir" >&2
    echo "Install it with: tools/install-idf.sh" >&2
    return 1 2>/dev/null || exit 1
fi

# shellcheck disable=SC1091
. "$_espkvm_idf_dir/export.sh"

unset _espkvm_idf_dir
