#!/usr/bin/env bash
# Flash a model blob into the 'model' partition defined in partitions.csv.
#
# Usage:
#   scripts/flash_model.sh [model.bin]
#   PORT=/dev/ttyUSB0 scripts/flash_model.sh
#
# The offset and size are read from partitions.csv, so the two cannot drift.
# If data/SHA256SUMS lists the file, its hash is checked first.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODEL="${1:-$ROOT/data/stories260K.bin}"
PART_NAME="model"
CSV="$ROOT/partitions.csv"
ESPTOOL="${ESPTOOL:-$HOME/.platformio/packages/tool-esptool/esptool}"
BAUD="${BAUD:-460800}"

[[ -f "$MODEL" ]] || { echo "model not found: $MODEL" >&2; exit 1; }
[[ -x "$ESPTOOL" ]] || { echo "esptool not found: $ESPTOOL" >&2; exit 1; }

row="$(awk -F',' -v name="$PART_NAME" '
    { gsub(/^[ \t]+|[ \t]+$/, "", $1) }
    $1 == name && $1 !~ /^#/ {
        o = $4; s = $5
        gsub(/[ \t]/, "", o); gsub(/[ \t]/, "", s)
        print o " " s
        exit
    }' "$CSV")"
[[ -n "$row" ]] || { echo "partition '$PART_NAME' not found in $CSV" >&2; exit 1; }
OFFSET="${row%% *}"
SIZE="${row##* }"
[[ -n "$OFFSET" ]] || { echo "partition '$PART_NAME' has no explicit offset" >&2; exit 1; }

(( OFFSET % 0x10000 == 0 )) || {
    echo "offset $OFFSET is not 64 KiB aligned; esp_partition_mmap would fail" >&2
    exit 1
}

model_size=$(stat -c %s "$MODEL")
part_size=$((SIZE))
(( model_size <= part_size )) || {
    echo "model is $model_size B but partition is $part_size B" >&2
    exit 1
}

if [[ -f "$ROOT/data/SHA256SUMS" ]]; then
    want="$(awk -v f="$(basename "$MODEL")" '{
        n = split($2, a, "/"); if (a[n] == f) { print $1; exit }
    }' "$ROOT/data/SHA256SUMS")"
    if [[ -n "$want" ]]; then
        got="$(sha256sum "$MODEL" | cut -d' ' -f1)"
        [[ "$got" == "$want" ]] || {
            echo "sha256 mismatch for $MODEL" >&2
            echo "  expected $want" >&2
            echo "  got      $got" >&2
            exit 1
        }
        echo "sha256 OK: $got"
    fi
fi

if [[ -z "${PORT:-}" ]]; then
    for candidate in /dev/ttyUSB0 /dev/ttyUSB1 /dev/ttyACM0 /dev/ttyACM1; do
        [[ -e "$candidate" ]] && { PORT="$candidate"; break; }
    done
fi
[[ -n "${PORT:-}" ]] || {
    echo "no serial port found; set PORT=/dev/ttyUSB0 (see 'pio device list')" >&2
    exit 1
}

echo "flashing $(basename "$MODEL") ($model_size B) -> $PORT @ $OFFSET"
"$ESPTOOL" --chip esp32 -p "$PORT" -b "$BAUD" \
    write_flash "$OFFSET" "$MODEL"
echo "done. verify at boot: partition size $SIZE B"
