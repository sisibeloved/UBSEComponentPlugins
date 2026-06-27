#!/bin/sh
set -eu

build_dir=${1:?build directory required}
sdk_flow=${2:?sdk flow executable required}
sdk_so=${3:?libubse_ssu_sdk.so path required}
root="/tmp/ssu-lbc-mock-sdk-$$"
prefix="$root/prefix"
dev_dir="$root/dev"
configfs_dir="$root/configfs"
config_file="$root/ssu_lbc_mock.conf"
socket="$root/ssu-mgr.fifo"
log_file="$root/lbc.log"
subnqn="nqn.2025-01.io.ssu:m0"
ssu_mgr="$build_dir/src/user/runtime/ssu-mgr"

rm -rf "$root"
mkdir -p "$prefix/mock" "$dev_dir" "$configfs_dir/$subnqn/namespaces"

cat > "$config_file" <<EOF
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
echo run "\$sample" "\$@" >> "$log_file"
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
    nsid=\$(next_nsid)
    : > "$dev_dir/nvme1n\$nsid"
    mkdir -p "$configfs_dir/\$subnqn/namespaces/\$nsid"
elif [ "\$sample" = "./sample_detach_delete" ]; then
    rm -f "\$devpath"
    rm -rf "$configfs_dir/\$subnqn/namespaces/\$nsid"
else
    exit 2
fi
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

"$sdk_flow" "$sdk_so" "$socket"

test ! -e "$dev_dir/nvme1n1"
test ! -e "$dev_dir/nvme1n2"
test ! -d "$configfs_dir/$subnqn/namespaces/1"
test ! -d "$configfs_dir/$subnqn/namespaces/2"
grep -q 'setup nqn.2025-01.io.ssu:m0 4420' "$log_file"
grep -q -- '--nsze 8' "$log_file"
grep -q 'mount success allocate_id=alloc-0 host_id=local logical_dev=/dev/ssu/sdk-disk' "$log_file"
grep -q 'unmount success logical_dev=/dev/ssu/sdk-disk allocate_id=alloc-0 host_id=local' "$log_file"
