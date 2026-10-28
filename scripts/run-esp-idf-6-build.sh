#!/usr/bin/env bash

set -euo pipefail

# Requires act and a running Docker daemon.
# This runner image includes the Docker CLI used by the workflow.
# Additional arguments are forwarded to act, e.g. --list or --pull=false.

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo_root="$(cd "$script_dir/.." && pwd -P)"
cd "$repo_root"

if ! command -v act >/dev/null 2>&1; then
    echo "error: act is not installed or not available in PATH" >&2
    exit 127
fi

exec act pull_request \
    -W .github/workflows/esp_box_mp3_build.yml \
    -j esp-idf-6-build \
    -P ubuntu-latest=ghcr.io/catthehacker/ubuntu:act-latest \
    "$@"
