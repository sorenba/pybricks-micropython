#!/usr/bin/env bash

set -euo pipefail

cd "$(dirname "$0")"

pybricksdev run ble power-tests/read_power_log.py
