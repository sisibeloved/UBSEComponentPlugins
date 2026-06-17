#!/bin/sh
set -eu

build_dir=${1:?build directory required}
source_dir=${2:?source directory required}
socket="/tmp/ubse-ssu-mgr-$$.sock"
aid_file="$build_dir/tests/mvp2/aid"
ubsectl="$build_dir/tools/ubsectl"
ssu_mgr="$build_dir/src/user/runtime/ssu-mgr"
config="$source_dir/tests/mvp2/ssu.conf"

rm -f "$socket" "$aid_file"

SSU_MOCK_SSU_COUNT=1 "$ssu_mgr" --role=manager --config "$config" --socket "$socket" &
mgr_pid=$!
trap 'kill "$mgr_pid" 2>/dev/null || true; rm -f "$socket" "$aid_file"' EXIT

i=0
while [ ! -p "$socket" ]; do
    i=$((i + 1))
    if [ "$i" -gt 50 ]; then
        echo "manager socket did not appear" >&2
        exit 1
    fi
    sleep 0.1
done

SSU_MGR_SOCKET="$socket" "$ubsectl" alloc --size 8192 --stripe --share exclusive --out "$aid_file"
aid=$(cat "$aid_file")
test "$aid" = "alloc-0"

SSU_MGR_SOCKET="$socket" "$ubsectl" mount --aid "$aid" --host local --dev /dev/ssu0
SSU_MGR_SOCKET="$socket" "$ubsectl" query --type logdev | grep -q '^/dev/ssu0[[:space:]]\+local[[:space:]]\+alloc-0[[:space:]]'

if SSU_MGR_SOCKET="$socket" "$ubsectl" release --aid "$aid"; then
    echo "release while mounted should fail" >&2
    exit 1
fi

SSU_MGR_SOCKET="$socket" "$ubsectl" unmount --dev /dev/ssu0
SSU_MGR_SOCKET="$socket" "$ubsectl" query --type allocation | grep -q '^alloc-0[[:space:]]\+ACTIVE[[:space:]]'
SSU_MGR_SOCKET="$socket" "$ubsectl" mount --aid "$aid" --host local --dev /dev/ssu0
SSU_MGR_SOCKET="$socket" "$ubsectl" unmount --dev /dev/ssu0
SSU_MGR_SOCKET="$socket" "$ubsectl" release --aid "$aid"

if SSU_MGR_SOCKET="$socket" "$ubsectl" query --type allocation | grep -q '^alloc-0[[:space:]]'; then
    echo "released allocation should not remain active" >&2
    exit 1
fi

if SSU_MGR_SOCKET="$socket" "$ubsectl" alloc --size 8192 --replica 3; then
    echo "replica alloc should be unsupported" >&2
    exit 1
fi
