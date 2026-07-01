#!/bin/sh
set -eu

build_dir=${1:?build directory required}
source_dir=${2:?source directory required}
socket="/tmp/ubse-ssu-mvp5-$$.fifo"
backend_dir="/tmp/ubse-ssu-mvp5-backend-$$"
aid_file="$build_dir/tests/mvp5/aid"
ubsectl="$build_dir/tools/ubsectl"
ssu_mgr="$build_dir/src/user/runtime/ssu-mgr"
ssu_smoke="$build_dir/tests/stubs/ubs_io/ssu_smoke"
config="$source_dir/tests/mvp5/ssu.conf"

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

SSU_MGR_SOCKET="$socket" "$ubsectl" alloc --size 65536 --stripe --share shared --out "$aid_file"
aid=$(cat "$aid_file")

SSU_MGR_SOCKET="$socket" "$ubsectl" mount --aid "$aid" --host nodeA --dev /dev/ssu0
SSU_MGR_SOCKET="$socket" "$ssu_smoke" /dev/ssu0 --bytes 65536 --write-only

SSU_MGR_SOCKET="$socket" "$ubsectl" mount --aid "$aid" --host nodeB --dev /dev/ssu1
SSU_MGR_SOCKET="$socket" "$ssu_smoke" /dev/ssu1 --bytes 65536 --read-only

SSU_MGR_SOCKET="$socket" "$ubsectl" unmount --dev /dev/ssu1
SSU_MGR_SOCKET="$socket" "$ubsectl" query --type logdev |
    grep -q '^logdev\[[0-9][0-9]*\]: logical_dev=/dev/ssu0 host_id=nodeA '
if SSU_MGR_SOCKET="$socket" "$ubsectl" query --type logdev |
    grep -q '^logdev\[[0-9][0-9]*\]: logical_dev=/dev/ssu1 '; then
    echo "/dev/ssu1 still mounted after nodeB unmount" >&2
    exit 1
fi

SSU_MGR_SOCKET="$socket" "$ssu_smoke" /dev/ssu0 --bytes 65536 --read-only
SSU_MGR_SOCKET="$socket" "$ubsectl" unmount --dev /dev/ssu0
SSU_MGR_SOCKET="$socket" "$ubsectl" release --aid "$aid"

if SSU_MGR_SOCKET="$socket" "$ubsectl" query --type allocation |
    grep -q "^allocation\\[[0-9][0-9]*\\]: allocate_id=$aid "; then
    echo "$aid still active after release" >&2
    exit 1
fi
