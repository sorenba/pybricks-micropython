#!/usr/bin/env bash

set -euo pipefail

cd "$(dirname "$0")"

git pull --ff-only
./rebuild.sh
./flash.sh
