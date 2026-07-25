#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo_root="$(cd "$script_dir/.." && pwd -P)"
cd "$repo_root"

cppcheck_bin="${CPPCHECK:-cppcheck}"

if ! command -v "$cppcheck_bin" >/dev/null 2>&1; then
    echo "error: cppcheck is not installed or not available in PATH" >&2
    exit 127
fi

sources=()

if (( $# > 0 )); then
    sources=("$@")
else
    while IFS= read -r -d '' source; do
        sources+=("$source")
    done < <(git ls-files -z -- '*.c' '*.cc' '*.cpp' '*.cxx')
fi

if (( ${#sources[@]} == 0 )); then
    echo "error: no C or C++ source files found" >&2
    exit 1
fi

"$cppcheck_bin" --version

# cppcheck does not parse GNU asm("linker_symbol") declarations used for
# ESP-IDF embedded binary boundaries. Strip only the annotation during analysis.
exec "$cppcheck_bin" \
    -Itest/host/stubs \
    --enable=all \
    --inconclusive \
    --inline-suppr \
    --std=c++20 \
    --check-level=exhaustive \
    --suppress=missingInclude \
    --suppress=missingIncludeSystem \
    --suppress=unusedFunction \
    --error-exitcode=1 \
    -DCONFIG_AUDIO_PLAYER_ENABLE_MP3 \
    -DCONFIG_AUDIO_PLAYER_ENABLE_WAV \
    -DCONFIG_AUDIO_PLAYER_ENABLE_HTTP_STREAM \
    '-Dasm(x)=' \
    "${sources[@]}"
