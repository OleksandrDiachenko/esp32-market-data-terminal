#!/usr/bin/env bash
# Builds the ESP32-C6 co-processor ("slave") firmware from this project's
# pinned esp_hosted version, in packet mode to match the host's SDIO config.
#
# Must run against the vendored source under managed_components/ (fetched by
# `idf.py reconfigure` from the espressif/esp_hosted version pinned in
# components/wifi_manager/idf_component.yml), so the slave image always
# matches whatever the host side actually resolved.
#
# Requires a *separate* ESP-IDF install from the one used for the P4 host -
# see docs/decisions/0005-esp-hosted-slave-ota.md for why (the vendored
# slave/ project's SDIO driver source isn't updated for ESP-IDF 6.0's HAL
# component split).
#
# Usage:
#   IDF_PATH=~/esp/esp-idf-v5.5.4 tools/esp_hosted_slave/build_slave_fw.sh [build-dir]
#
# Output: <build-dir>/network_adapter.bin - flash into the host's own
# "slave_fw" data partition (see partitions.csv), e.g.:
#   esptool.py --chip esp32p4 -p <port> write-flash --force 0xD40000 \
#     <build-dir>/network_adapter.bin

set -euo pipefail

if [[ -z "${IDF_PATH:-}" ]]; then
    echo "error: source a separate ESP-IDF's export.sh first (see docs/decisions/0005-esp-hosted-slave-ota.md)" >&2
    exit 1
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../.." && pwd)"
slave_dir="${repo_root}/managed_components/espressif__esp_hosted/slave"
build_dir="${1:-${repo_root}/build-slave-fw}"

if [[ ! -d "${slave_dir}" ]]; then
    echo "error: ${slave_dir} not found - run 'idf.py reconfigure' in ${repo_root} first" >&2
    exit 1
fi

rm -f "${slave_dir}/sdkconfig"

(
    cd "${slave_dir}"
    idf.py -B "${build_dir}" \
        -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32c6;${script_dir}/sdkconfig.packet_mode" \
        set-target esp32c6
    idf.py -B "${build_dir}" build
)

echo
echo "Built: ${build_dir}/network_adapter.bin"
