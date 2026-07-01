#!/bin/sh
set -eu

build_dir=${1:?build directory required}
source_dir=${2:?source directory required}
socket="/tmp/ubse-ssu-mgr-$$.sock"
aid_file="$build_dir/tests/mvp2/aid"
rid_file="$build_dir/tests/mvp2/rid"
ubsectl="$build_dir/tools/ubsectl"
ssu_mgr="$build_dir/src/user/runtime/ssu-mgr"
config="$source_dir/tests/mvp2/ssu.conf"

rm -f "$socket" "$aid_file" "$rid_file"

SSU_MOCK_SSU_COUNT=3 "$ssu_mgr" --role=manager --config "$config" --socket "$socket" &
mgr_pid=$!
trap 'kill "$mgr_pid" 2>/dev/null || true; rm -f "$socket" "$aid_file" "$rid_file"' EXIT

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
SSU_MGR_SOCKET="$socket" "$ubsectl" query --type logdev |
    grep -q '^logdev\[0\]: logical_dev=/dev/ssu0 host_id=local allocate_id=alloc-0 '

if SSU_MGR_SOCKET="$socket" "$ubsectl" release --aid "$aid"; then
    echo "release while mounted should fail" >&2
    exit 1
fi

SSU_MGR_SOCKET="$socket" "$ubsectl" unmount --dev /dev/ssu0
SSU_MGR_SOCKET="$socket" "$ubsectl" query --type allocation |
    grep -q '^allocation\[0\]: allocate_id=alloc-0 state=ACTIVE '
SSU_MGR_SOCKET="$socket" "$ubsectl" mount --aid "$aid" --host local --dev /dev/ssu0
SSU_MGR_SOCKET="$socket" "$ubsectl" unmount --dev /dev/ssu0
SSU_MGR_SOCKET="$socket" "$ubsectl" release --aid "$aid"

if SSU_MGR_SOCKET="$socket" "$ubsectl" query --type allocation |
    grep -q '^allocation\[[0-9][0-9]*\]: allocate_id=alloc-0 '; then
    echo "released allocation should not remain active" >&2
    exit 1
fi

if SSU_MGR_SOCKET="$socket" "$ubsectl" alloc --size 8192 --replica 3; then
    echo "replica alloc should be unsupported" >&2
    exit 1
fi

SSU_MGR_SOCKET="$socket" "$ubsectl" list | grep -q '^pool entries: 3'

if SSU_MGR_SOCKET="$socket" "$ubsectl" allocate \
    --size 8192 \
    --user user-cli \
    --no-aggregate \
    --host local; then
    echo "logical allocate without aggregation should be unsupported" >&2
    exit 1
fi

SSU_MGR_SOCKET="$socket" "$ubsectl" allocate \
    --size 12288 \
    --user user-cli \
    --physical-disks 3 \
    --aggregate \
    --share exclusive \
    --host local \
    --out "$rid_file"
rid=$(cat "$rid_file")
test "$rid" = "alloc-1"

result=$(SSU_MGR_SOCKET="$socket" "$ubsectl" allocate-result-get --request-id "$rid")
dev=$(printf '%s\n' "$result" | sed -n '1p')
test "$dev" = "/dev/ssu/ssu1"
printf '%s\n' "$result" | grep -q '^physical_disks=3$'
printf '%s\n' "$result" | grep -q '^physical\[0\]: ssu_id=mock-ssu0 ns_id=mock-ns[0-9][0-9]* logical_offset_bytes=0 length_bytes=4096 physical_lba_512b=0 physical_offset_bytes=0$'
printf '%s\n' "$result" | grep -q '^physical\[1\]: ssu_id=mock-ssu1 ns_id=mock-ns[0-9][0-9]* logical_offset_bytes=4096 length_bytes=4096 physical_lba_512b=0 physical_offset_bytes=0$'
printf '%s\n' "$result" | grep -q '^physical\[2\]: ssu_id=mock-ssu2 ns_id=mock-ns[0-9][0-9]* logical_offset_bytes=8192 length_bytes=4096 physical_lba_512b=0 physical_offset_bytes=0$'

SSU_MGR_SOCKET="$socket" "$ubsectl" mount --dev "$dev" --host local
logdev_output=$(SSU_MGR_SOCKET="$socket" "$ubsectl" query --type logdev)
printf '%s\n' "$logdev_output" | grep -q "^logdev\\[0\\]: logical_dev=$dev host_id=local allocate_id=$rid logical_offset_bytes=0 length_bytes=4096 physical_dev=/dev/nullb0 ns_id=mock-ns[0-9][0-9]* physical_lba_512b=0 physical_offset_bytes=0$"
printf '%s\n' "$logdev_output" | grep -q "^logdev\\[1\\]: logical_dev=$dev host_id=local allocate_id=$rid logical_offset_bytes=4096 length_bytes=4096 physical_dev=/dev/nullb1 ns_id=mock-ns[0-9][0-9]* physical_lba_512b=0 physical_offset_bytes=0$"
printf '%s\n' "$logdev_output" | grep -q "^logdev\\[2\\]: logical_dev=$dev host_id=local allocate_id=$rid logical_offset_bytes=8192 length_bytes=4096 physical_dev=/dev/nullb2 ns_id=mock-ns[0-9][0-9]* physical_lba_512b=0 physical_offset_bytes=0$"

if SSU_MGR_SOCKET="$socket" "$ubsectl" free --dev "$dev"; then
    echo "free while mounted should fail" >&2
    exit 1
fi

SSU_MGR_SOCKET="$socket" "$ubsectl" unmount --dev "$dev"
SSU_MGR_SOCKET="$socket" "$ubsectl" free --dev "$dev"

if SSU_MGR_SOCKET="$socket" "$ubsectl" query --type allocation |
    grep -q "^allocation\\[[0-9][0-9]*\\]: allocate_id=$rid "; then
    echo "freed logical allocation should not remain active" >&2
    exit 1
fi
