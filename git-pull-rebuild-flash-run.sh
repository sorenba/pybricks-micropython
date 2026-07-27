#!/usr/bin/env bash

set -euo pipefail

cd "$(dirname "$0")"

printf '\033[2J\033[H'

git pull --ff-only
bash rebuild.sh

printf '\nRebuild successful. Press any key to flash...'
IFS= read -r -n 1 -s
printf '\n'

bash flash.sh
bash run.sh
