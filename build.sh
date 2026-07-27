#!/usr/bin/env bash

set -euo pipefail

cd "$(dirname "$0")"

chmod +x tools/metadata.py

poetry run make mpy-cross -j4
poetry run make -C bricks/technichub -j4
