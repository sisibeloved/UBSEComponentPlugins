#!/bin/sh
set -eu

build_dir=${1:?build directory required}
root="/tmp/ssu-lbc-mock-error-$$"
prefix="$root/prefix"
dev_dir="$root/dev"
configfs_dir="$root/configfs"
socket="$root/ssu-mgr.fifo"
aid_file="$root/aid"
log_file="$root/lbc.log"
subnqn="nqn.2025-01.io.ssu:m0"
ssu_mgr="$build_dir/src/user/runtime/ssu-mgr"
ubsectl="$build_dir/tools/ubsectl"

rm -rf "$root"
mkdir -p "$prefix/mock" "$dev_dir" "$configfs_dir/$subnqn/namespaces"

cat > "$prefix/mock/ssu_lbc_mock.conf" <<EOF
dev_dir=$dev_dir
configfs_dir=$configfs_dir
log_file=$log_file
EOF

cat > "$prefix/mock/setup_mock_target.sh" <<EOF
#!/bin/sh
set -eu
echo setup "\$1" "\$2" >> "$log_file"
mkdir -p "$configfs_dir/\$1/namespaces"
EOF

cat > "$prefix/mock/run_mock.sh" <<EOF
#!/bin/sh
set -eu
echo run "\$@" >> "$log_file"
exit 0
EOF

touch "$prefix/sample_create_attach" "$prefix/sample_detach_delete"
chmod +x "$prefix/mock/setup_mock_target.sh" "$prefix/mock/run_mock.sh"

cleanup() {
    if [ "${mgr_pid:-}" ]; then
        kill "$mgr_pid" 2>/dev/null || true
    fi
    rm -rf "$root"
}
trap cleanup EXIT

(
    LBC_PREFIX="$prefix" "$ssu_mgr" --role=manager --socket "$socket"
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

set +e
output=$(SSU_MGR_SOCKET="$socket" "$ubsectl" alloc \
    --size 512M --stripe --share exclusive --out "$aid_file" 2>&1)
rc=$?
set -e

if [ "$rc" -eq 0 ]; then
    echo "alloc unexpectedly succeeded" >&2
    exit 1
fi

printf '%s\n' "$output" | grep -q 'alloc failed: SSU_ERR_NOT_FOUND (-3)'
printf '%s\n' "$output" | grep -q 'plugin: lbc_mock'
printf '%s\n' "$output" | grep -q 'no new /dev/nvmeXnY namespace was found'
grep -q 'create_ns start allocate_id=alloc-0' "$log_file"
grep -q 'target ready subnqn=nqn.2025-01.io.ssu:m0 port=4420' "$log_file"
grep -q 'create_attach completed but no new NVMe namespace was found' "$log_file"
