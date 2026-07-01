#!/bin/sh
set -eu

build_dir=${1:?build directory required}
root="/tmp/ssu-lbc-mock-error-$$"
prefix="$root/prefix"
dev_dir="$root/dev"
configfs_dir="$root/configfs"
config_file="$root/ssu_lbc_mock.conf"
socket="$root/ssu-mgr.fifo"
aid_file="$root/aid"
log_file="$root/lbc.log"
subnqn="nqn.2025-01.io.ssu:m0"
ssu_mgr="$build_dir/src/user/runtime/ssu-mgr"
ubsectl="$build_dir/tools/ubsectl"

rm -rf "$root"
mkdir -p "$prefix/mock" "$dev_dir" "$configfs_dir/$subnqn/namespaces"

cat > "$config_file" <<EOF
dev_dir=$dev_dir
configfs_dir=$configfs_dir
log_file=$log_file
EOF

test ! -e "$prefix/mock/ssu_lbc_mock.conf"

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
    LBC_PREFIX="$prefix" SSU_LBC_MOCK_CONFIG="$config_file" \
        "$ssu_mgr" --role=manager --socket "$socket"
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

cat > "$prefix/mock/run_mock.sh" <<EOF
#!/bin/sh
set -eu
sample=\$1
shift
subnqn=
nsid=
nsze=
devpath=
next_nsid() {
    i=1
    while [ -e "$dev_dir/nvme1n\$i" ]; do
        i=\$((i + 1))
    done
    printf '%s\n' "\$i"
}
echo slow-run "\$sample" "\$@" >> "$log_file"
while [ "\$#" -gt 0 ]; do
    case "\$1" in
        --sub-nqn) subnqn=\$2; shift 2 ;;
        --nsid) nsid=\$2; shift 2 ;;
        --nsze) nsze=\$2; shift 2 ;;
        --dev-path) devpath=\$2; shift 2 ;;
        --dev-ip|--port) shift 2 ;;
        *) shift ;;
    esac
done

if [ "\$sample" = "./sample_create_attach" ]; then
    sleep 1
    nsid=\$(next_nsid)
    : > "$dev_dir/nvme1n\$nsid"
    mkdir -p "$configfs_dir/\$subnqn/namespaces/\$nsid"
elif [ "\$sample" = "./sample_detach_delete" ]; then
    rm -f "\$devpath"
    rmdir "$configfs_dir/\$subnqn/namespaces/\$nsid"
else
    exit 2
fi
EOF
chmod +x "$prefix/mock/run_mock.sh"

set +e
timeout_output=$(UBSE_SSU_SDK_RESPONSE_TIMEOUT_MS=100 \
    SSU_MGR_SOCKET="$socket" "$ubsectl" allocate \
    --size 512M --name slow-timeout --physical-disks 1 2>&1)
timeout_rc=$?
set -e

if [ "$timeout_rc" -eq 0 ]; then
    echo "slow allocate unexpectedly succeeded" >&2
    exit 1
fi

printf '%s\n' "$timeout_output" | grep -q 'allocate failed: SSU_ERR_IO (-5)'
sleep 2
kill -0 "$mgr_pid"
SSU_MGR_SOCKET="$socket" "$ubsectl" list >/dev/null
