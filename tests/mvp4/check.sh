#!/bin/sh
set -eu

build_dir=${1:?build directory required}
backend_dir="/tmp/ubse-ssu-mvp4-backend-$$"
ssu_smoke="$build_dir/tests/stubs/ubs_io/ssu_smoke"

rm -rf "$backend_dir"
mkdir -p "$backend_dir"
trap 'rm -rf "$backend_dir"' EXIT

SSU_MOCK_SSU_COUNT=2 SSU_MOCK_BACKEND_DIR="$backend_dir" \
    "$ssu_smoke" \
        --alloc --size 65536 --stripe \
        --mount --dev /dev/ssu0 \
        --io --pattern verify \
        --unmount --release

test -s "$backend_dir/mock-ssu0.img"
test -s "$backend_dir/mock-ssu1.img"
