#!/usr/bin/env bash

set -euo pipefail

cd "$(dirname "$0")"

chmod +x tools/metadata.py

rm -rf bricks/technichub/build
make -C micropython/mpy-cross clean
make mpy-cross -j4
make -C bricks/technichub -j4
