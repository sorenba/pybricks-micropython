#!/usr/bin/env bash

set -euo pipefail

cd "$(dirname "$0")"

chmod +x tools/metadata.py

python tools/flash bricks/technichub/build/firmware.zip
