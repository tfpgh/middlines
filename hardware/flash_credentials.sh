#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${SCRIPT_DIR}"

DEFAULT_PORT_GLOB="/dev/cu.usbserial*"
NVS_OFFSET="0x9000"
NVS_SIZE_HEX="0x4000"
OUT_DIR="${PROJECT_DIR}/build/credentials"

usage() {
  cat <<EOF
Usage: $(basename "$0") <credentials.csv> [port]

Generate an NVS image from a credentials CSV and flash it to the device.

Arguments:
  credentials.csv   Path to the CSV file for one device
  port              Optional serial port. Defaults to ${DEFAULT_PORT_GLOB}

Example:
  $(basename "$0") credentials/device-a.csv
  $(basename "$0") credentials/device-a.csv /dev/cu.usbserial-0001
EOF
}

if [[ $# -lt 1 || $# -gt 2 ]]; then
  usage
  exit 1
fi

if [[ -z "${IDF_PATH:-}" ]]; then
  echo "IDF_PATH is not set. Run: . \"/Users/tobypenner/esp/esp-idf/export.sh\""
  exit 1
fi

CSV_PATH="$1"
PORT_PATTERN="${2:-$DEFAULT_PORT_GLOB}"

if [[ ! -f "$CSV_PATH" ]]; then
  echo "Credentials CSV not found: $CSV_PATH"
  exit 1
fi

mkdir -p "$OUT_DIR"

CSV_NAME="$(basename "$CSV_PATH")"
CSV_STEM="${CSV_NAME%.csv}"
NVS_BIN="${OUT_DIR}/${CSV_STEM}.bin"

if [[ $# -eq 2 ]]; then
  PORT_MATCHES=("$PORT_PATTERN")
else
  shopt -s nullglob
  PORT_MATCHES=( $PORT_PATTERN )
  shopt -u nullglob
fi

if [[ ${#PORT_MATCHES[@]} -eq 0 ]]; then
  echo "No serial port matched: ${PORT_PATTERN}"
  exit 1
fi

if [[ ${#PORT_MATCHES[@]} -gt 1 ]]; then
  echo "Multiple serial ports matched:"
  printf '  %s\n' "${PORT_MATCHES[@]}"
  echo "Pass the exact port as the second argument."
  exit 1
fi

PORT="${PORT_MATCHES[0]}"
NVS_GEN="${IDF_PATH}/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"

echo "Generating NVS image from ${CSV_PATH}"
python3 "$NVS_GEN" generate "$CSV_PATH" "$NVS_BIN" "$NVS_SIZE_HEX"

echo "Flashing credentials to ${PORT} at ${NVS_OFFSET}"
python3 -m esptool --chip esp32 --port "$PORT" write_flash "$NVS_OFFSET" "$NVS_BIN"

echo "Done: ${NVS_BIN}"
