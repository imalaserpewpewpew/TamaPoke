#!/usr/bin/env bash
# Build TamaPoke for the Waveshare ESP32-S3-Touch-AMOLED-1.43 (experimental).
#
# Same flash layout as the original 1.75 (16 MB, 3 MB app + FAT, sprites on the
# microSD card), so the only difference is the compile flag -DBOARD_AMOLED_143=1.
# See PORTING_143.md.
#
# Usage:
#   tools/build_143.sh                      # -> ../out-143 (or $TAMAPOKE_OUT)
#   tools/build_143.sh --out DIR
#   tools/build_143.sh --flags '-DPOWER_DEEPSLEEP_MS=300000UL'   # extra compiler flags
#
# Flash (app only, keeps the pet and settings):
#   esptool --chip esp32s3 --port <port> --baud 921600 write-flash 0x10000 OUT/TamaPoke.ino.bin
# First flash (bootloader + partition table + app):
#   esptool --chip esp32s3 --port <port> --baud 921600 write-flash 0x0 OUT/TamaPoke.ino.merged.bin
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${TAMAPOKE_OUT:-${ROOT}/../out-143}"
EXTRA=""
FQBN="${FQBN_143:-esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --out)   OUT="$2"; shift 2 ;;
    --flags) EXTRA="$2"; shift 2 ;;
    -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
    *) echo "Unknown arg: $1" >&2; exit 1 ;;
  esac
done

if [[ -f "${ROOT}/partitions.csv" ]]; then
  echo "A partitions.csv in the sketch root would override every build; remove it." >&2
  exit 1
fi
mkdir -p "$OUT"

arduino-cli compile --warnings=all \
  --fqbn "$FQBN" \
  --build-property "compiler.cpp.extra_flags=-DBOARD_AMOLED_143=1 ${EXTRA}" \
  --output-dir "$OUT" \
  "$ROOT"

echo "1.43 build OK -> $OUT"
