#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo_root="$(cd "$script_dir/.." && pwd -P)"
build_dir="${HOST_TEST_BUILD_DIR:-$repo_root/build/host-codec-tests}"
build_type="${CMAKE_BUILD_TYPE:-Debug}"

cmake -S "$repo_root/test/host" \
    -B "$build_dir" \
    -DCMAKE_BUILD_TYPE="$build_type"
cmake --build "$build_dir" --parallel
ctest --test-dir "$build_dir" --output-on-failure
