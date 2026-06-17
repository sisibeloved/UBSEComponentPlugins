#!/bin/sh
set -eu

build_dir=${1:?build directory required}
source_dir=${2:?source directory required}
socket="/tmp/ubse-ssu-mvp3-$$.fifo"
backend_dir="/tmp/ubse-ssu-mvp3-backend-$$"
aid_file="$build_dir/tests/mvp3/aid"
ubsectl="$build_dir/tools/ubsectl"
ssu_mgr="$build_dir/src/user/runtime/ssu-mgr"
ssu_smoke="$build_dir/tests/stubs/ubs_io/ssu_smoke"
config="$source_dir/tests/mvp3/ssu.conf"

rm -f "$socket" "$aid_file"
rm -rf "$backend_dir"
mkdir -p "$backend_dir"

SSU_MOCK_SSU_COUNT=1 SSU_MOCK_BACKEND_DIR="$backend_dir" \
    "$ssu_mgr" --role=manager --config "$config" --socket "$socket" &
mgr_pid=$!
trap 'kill "$mgr_pid" 2>/dev/null || true; rm -f "$socket" "$aid_file"; rm -rf "$backend_dir"' EXIT

i=0
while [ ! -p "$socket" ]; do
    i=$((i + 1))
    if [ "$i" -gt 50 ]; then
        echo "manager fifo did not appear" >&2
        exit 1
    fi
    sleep 0.1
done

SSU_MGR_SOCKET="$socket" "$ubsectl" alloc --size 65536 --stripe --share exclusive --out "$aid_file"
aid=$(cat "$aid_file")
SSU_MGR_SOCKET="$socket" "$ubsectl" mount --aid "$aid" --host local --dev /dev/ssu0
SSU_MGR_SOCKET="$socket" "$ubsectl" query --type logdev | grep -q "$backend_dir/mock-ssu0.img"
SSU_MGR_SOCKET="$socket" "$ssu_smoke" /dev/ssu0 --bytes 65536
test -s "$backend_dir/mock-ssu0.img"
