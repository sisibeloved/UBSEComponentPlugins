#!/bin/sh
set -eu

build_dir=${1:?build directory required}
root="/tmp/ssu-lbc-mock-userguide-$$"
prefix="$root/prefix"
dev_dir="$root/dev"
configfs_dir="$root/configfs"
config_file="$root/ssu_lbc_mock.conf"
socket="$root/ssu-mgr.fifo"
rid_file="$root/ssu-rid"
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

SSU_MGR_SOCKET="$socket" "$ubsectl" list |
    grep -q '^pool entries: 3$'

allocate_output=$(SSU_MGR_SOCKET="$socket" "$ubsectl" allocate \
    --size 512M \
    --name demo-disk \
    --user user-demo \
    --share exclusive \
    --host local \
    --out "$rid_file")
printf '%s\n' "$allocate_output" | grep -qx 'alloc-0'
test "$(cat "$rid_file")" = "$allocate_output"

rid=$(cat "$rid_file")
result=$(SSU_MGR_SOCKET="$socket" "$ubsectl" allocate-result-get --request-id "$rid")
dev=$(printf '%s\n' "$result" | sed -n '1p')
test "$dev" = "/dev/ssu/demo-disk"
printf '%s\n' "$result" |
    grep -q '^physical 0 lbc-mock-ssu0 1 0 536870912 lba=0$'

SSU_MGR_SOCKET="$socket" "$ubsectl" mount --dev "$dev" --host local
SSU_MGR_SOCKET="$socket" "$ubsectl" query --type logdev |
    grep -q "^$dev[[:space:]]\\+local[[:space:]]\\+$rid[[:space:]]\\+0[[:space:]]\\+536870912[[:space:]]\\+$dev_dir/nvme1n1"

SSU_MGR_SOCKET="$socket" "$ubsectl" unmount --dev "$dev"
SSU_MGR_SOCKET="$socket" "$ubsectl" free --dev "$dev"

test ! -e "$dev_dir/nvme1n1"
test ! -d "$configfs_dir/$subnqn/namespaces/1"
