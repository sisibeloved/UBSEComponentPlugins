#!/bin/sh
set -eu

build_dir=${1:?build directory required}
source_dir=${2:?source directory required}
ubsectl="$build_dir/tools/ubsectl"
ssu_mgr="$build_dir/src/user/runtime/ssu-mgr"
config="$source_dir/tests/mvp1/ssu.conf"
socket="/tmp/ubse-ssu-mgr-mvp1-$$.fifo"
mgr_pid=

cleanup() {
    if [ -n "$mgr_pid" ]; then
        kill "$mgr_pid" 2>/dev/null || true
    fi
    rm -f "$socket"
}
trap cleanup EXIT

manager_output=$(SSU_MOCK_SSU_COUNT=2 "$ssu_mgr" --role=manager --config "$config" --once)
printf '%s\n' "$manager_output"

printf '%s\n' "$manager_output" | grep -q '^ssu-mgr role=manager$'
printf '%s\n' "$manager_output" | grep -q '^pool entries: 2$'

rm -f "$socket"
SSU_MOCK_SSU_COUNT=2 "$ssu_mgr" --role=manager --config "$config" \
    --socket "$socket" &
mgr_pid=$!

i=0
while [ ! -p "$socket" ]; do
    i=$((i + 1))
    if [ "$i" -gt 50 ]; then
        echo "manager socket did not appear" >&2
        exit 1
    fi
    sleep 0.1
done

output=$(SSU_MGR_SOCKET="$socket" "$ubsectl" query --type pool)
printf '%s\n' "$output"

printf '%s\n' "$output" | grep -q '^pool entries: 2$'
printf '%s\n' "$output" | grep -Eq '^mock-ssu0[[:space:]]+mock-host0[[:space:]]+ONLINE[[:space:]]+0/1073741824$'
printf '%s\n' "$output" | grep -Eq '^mock-ssu1[[:space:]]+mock-host1[[:space:]]+ONLINE[[:space:]]+0/1073741824$'
