#!/usr/bin/env bash

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    echo "Run: source ./poetry.sh"
    exit 1
fi

cd "$(dirname "${BASH_SOURCE[0]}")" || return 1
eval "$(poetry env activate)"
