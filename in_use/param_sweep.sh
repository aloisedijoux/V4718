#!/usr/bin/env bash

set -u

PID="${1:?Usage: $0 <PID_V4718> <base_address_hex> [duree_par_config_s]}"
BASE="${2:?Usage: $0 <PID_V4718> <base_address_hex> [duree_par_config_s]}"
DURATION="${3:-5}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="${SCRIPT_DIR}/sweep_results"

mkdir -p "$OUT_DIR"
rm -f "$OUT_DIR"/*.csv "$OUT_DIR/manifest.txt"

CONFIGS=(
  "100 -80 8 4"
  "40 -80 8 4"
  "20 -80 8 4"
  "100 -60 8 4"
)

i=0

for cfg in "${CONFIGS[@]}"; do
  read -r width offset search reject <<< "$cfg"

  width_ns=$((width * 25))
  offset_ns=$((offset * 25))
  label="w${width_ns}_o${offset_ns}"
  csv="${OUT_DIR}/sweep_${i}_${label}.csv"

  echo "Configuration ${i}: width=${width_ns} ns, offset=${offset_ns} ns"

  "${SCRIPT_DIR}/config_trigger_matching" \
    "$PID" "$BASE" "$width" "$offset" "$search" "$reject" >/dev/null

  "${SCRIPT_DIR}/enable_trigger_subtraction" \
    "$PID" "$BASE" >/dev/null

  "${SCRIPT_DIR}/clear_buffer" \
    "$PID" "$BASE" >/dev/null

  timeout "$DURATION" \
    "${SCRIPT_DIR}/read_output_buffer_blt" \
    "$PID" "$BASE" 4096 -c -s "$offset_ns" \
    >"$csv" 2>/dev/null

  n_lines=$(wc -l <"$csv")
  echo "${n_lines} hits capturés"

  echo "${label},${width_ns},${offset_ns},${csv}" \
    >>"${OUT_DIR}/manifest.txt"

  ((i += 1))
done

python3 "${SCRIPT_DIR}/plot_sweep.py" "${OUT_DIR}/manifest.txt"