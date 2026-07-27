#!/usr/bin/env bash

set -euo pipefail

cd "$(dirname "$0")"

chmod +x tools/metadata.py

make mpy-cross -j4
make -C bricks/technichub -j4
