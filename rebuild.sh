#!/usr/bin/env bash

set -euo pipefail

cd "$(dirname "$0")"

chmod +x tools/metadata.py

run_build() {
    local label="$1"
    shift

    local log_file
    log_file="$(mktemp)"

    echo "$label..."

    if "$@" >"$log_file" 2>&1; then
        grep -E '^(PLATFORM:|LINK |Creating firmware|Firmware|[[:space:]]*text[[:space:]]+data[[:space:]]+bss|[[:space:]]*[0-9]+[[:space:]]+[0-9]+[[:space:]]+[0-9]+[[:space:]]+[0-9]+[[:space:]]+[0-9a-fA-F]+[[:space:]])' "$log_file" || true
        echo "$label complete."
        rm -f "$log_file"
        return 0
    else
        local status=$?
        echo "$label failed with exit code $status. Full output:"
        cat "$log_file"
        rm -f "$log_file"
        return "$status"
    fi
}

rm -rf bricks/technichub/build
run_build "Cleaning mpy-cross" make -C micropython/mpy-cross clean
run_build "Building mpy-cross" make mpy-cross -j4
run_build "Building Technic Hub firmware" make -C bricks/technichub -j4
