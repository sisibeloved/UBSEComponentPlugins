#!/bin/sh
set -eu

build_dir=${1:?build directory required}
lbc_prefix=${2:?LBC INI mock prefix directory required}
socket="/tmp/ubse-lbc-mock-real-$$.fifo"
aid_file="/tmp/ubse-lbc-mock-real-aid-$$"
subnqn="nqn.2025-01.io.ssu:m0"
ssu_mgr="$build_dir/src/user/runtime/ssu-mgr"
ubsectl="$build_dir/tools/ubsectl"

test -r "$lbc_prefix/mock/setup_mock_target.sh"
test -r "$lbc_prefix/mock/run_mock.sh"
test -x "$build_dir/tests/lbc_mock/lbc_mock_plugin_flow"
test -x "$ssu_mgr"
test -x "$ubsectl"

"$build_dir/tests/lbc_mock/lbc_mock_plugin_flow" \
    --real-prefix "$lbc_prefix"

rm -f "$socket" "$aid_file"
cleanup() {
    if [ "${mgr_pid:-}" ]; then
        kill "$mgr_pid" 2>/dev/null || true
    fi
    rm -f "$socket" "$aid_file"
}
trap cleanup EXIT

(
    LBC_PREFIX="$lbc_prefix" "$ssu_mgr" --role=manager --socket "$socket"
) &
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

SSU_MGR_SOCKET="$socket" "$ubsectl" query --type pool |
    grep -q '^lbc-mock-ssu0[[:space:]]\+lbc-mock-host0[[:space:]]\+ONLINE'

SSU_MGR_SOCKET="$socket" "$ubsectl" alloc \
    --size 512M --stripe --share exclusive --out "$aid_file"
aid=$(cat "$aid_file")

SSU_MGR_SOCKET="$socket" "$ubsectl" mount \
    --aid "$aid" --host local --dev /dev/ssu0

dev_path=$(SSU_MGR_SOCKET="$socket" "$ubsectl" query --type logdev |
    awk '/^\/dev\/ssu0[[:space:]]/ { print $6; exit }')
test -n "$dev_path"
test -e "$dev_path"
test "$(cat "/sys/class/block/$(basename "$dev_path")/size")" = "1048576"
test -d "/sys/kernel/config/nvmet/subsystems/$subnqn/namespaces/1"

SSU_MGR_SOCKET="$socket" "$ubsectl" unmount --dev /dev/ssu0
SSU_MGR_SOCKET="$socket" "$ubsectl" release --aid "$aid"

if [ -e "$dev_path" ]; then
    echo "$dev_path still exists after release" >&2
    exit 1
fi

if [ -d "/sys/kernel/config/nvmet/subsystems/$subnqn/namespaces/1" ]; then
    echo "$subnqn namespace 1 still exists after release" >&2
    exit 1
fi
