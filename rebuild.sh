#!/usr/bin/env bash

set -euo pipefail

cd "$(dirname "$0")"

chmod +x tools/metadata.py

rm -rf bricks/technichub/build
poetry run make -C micropython/mpy-cross clean
poetry run make mpy-cross -j4
poetry run make -C bricks/technichub -j4
