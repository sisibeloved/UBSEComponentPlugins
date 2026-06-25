#!/bin/sh
set -eu

build_dir=${1:?build directory required}
root="/tmp/ssu-lbc-mock-ubsectl-$$"
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
    test "\$nsze" = "1048576"
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

SSU_MGR_SOCKET="$socket" "$ubsectl" query --type pool |
    grep -q '^lbc-mock-ssu0[[:space:]]\+lbc-mock-host0[[:space:]]\+ONLINE'
SSU_MGR_SOCKET="$socket" "$ubsectl" query --type pool |
    grep -q '^pool entries: 3'
SSU_MGR_SOCKET="$socket" "$ubsectl" query --type pool |
    grep -q '^lbc-mock-ssu2[[:space:]]\+lbc-mock-host2[[:space:]]\+ONLINE'

SSU_MGR_SOCKET="$socket" "$ubsectl" alloc \
    --size 512M --stripe --share exclusive --out "$aid_file"
aid=$(cat "$aid_file")
test "$aid" = "alloc-0"

SSU_MGR_SOCKET="$socket" "$ubsectl" mount \
    --aid "$aid" --host local --dev /dev/ssu0

SSU_MGR_SOCKET="$socket" "$ubsectl" query --type logdev |
    grep -q "$dev_dir/nvme1n1"
test -e "$dev_dir/nvme1n1"
test -d "$configfs_dir/$subnqn/namespaces/1"

SSU_MGR_SOCKET="$socket" "$ubsectl" unmount --dev /dev/ssu0
SSU_MGR_SOCKET="$socket" "$ubsectl" release --aid "$aid"

test ! -e "$dev_dir/nvme1n1"
test ! -d "$configfs_dir/$subnqn/namespaces/1"

SSU_MGR_SOCKET="$socket" "$ubsectl" allocate \
    --size 12288 \
    --user user-lbc \
    --physical-disks 3 \
    --aggregate \
    --share exclusive \
    --host local \
    --out "$aid_file"
rid=$(cat "$aid_file")
test "$rid" = "alloc-1"

result=$(SSU_MGR_SOCKET="$socket" "$ubsectl" allocate-result-get --request-id "$rid")
dev=$(printf '%s\n' "$result" | sed -n '1p')
test "$dev" = "/dev/ssu/ssu1"
printf '%s\n' "$result" | grep -q '^physical 0 lbc-mock-ssu0 2 0 4096 lba=0$'
printf '%s\n' "$result" | grep -q '^physical 1 lbc-mock-ssu1 3 4096 4096 lba=0$'
printf '%s\n' "$result" | grep -q '^physical 2 lbc-mock-ssu2 4 8192 4096 lba=0$'

SSU_MGR_SOCKET="$socket" "$ubsectl" mount --dev "$dev" --host local
SSU_MGR_SOCKET="$socket" "$ubsectl" query --type logdev |
    grep -q "^$dev[[:space:]]\+local[[:space:]]\+$rid[[:space:]]\+8192[[:space:]]\+4096[[:space:]]\+$dev_dir/nvme1n3"
SSU_MGR_SOCKET="$socket" "$ubsectl" unmount --dev "$dev"
SSU_MGR_SOCKET="$socket" "$ubsectl" free --dev "$dev"

test ! -e "$dev_dir/nvme1n1"
test ! -e "$dev_dir/nvme1n2"
test ! -e "$dev_dir/nvme1n3"
test ! -d "$configfs_dir/$subnqn/namespaces/2"
test ! -d "$configfs_dir/$subnqn/namespaces/3"
test ! -d "$configfs_dir/$subnqn/namespaces/4"

grep -q 'setup nqn.2025-01.io.ssu:m0 4420' "$log_file"
grep -q "config file loaded: $config_file" "$log_file"
grep -q "config path=$config_file prefix=$prefix" "$log_file"
grep -q 'run cwd=' "$log_file"
grep -q -- '--nsze 1048576' "$log_file"
grep -q -- '--nsid 1' "$log_file"
grep -q 'target ready subnqn=nqn.2025-01.io.ssu:m0 port=4420' "$log_file"
grep -q 'create_ns success allocate_id=alloc-0 ns_id=1' "$log_file"
grep -q 'mount success allocate_id=alloc-0 host_id=local logical_dev=/dev/ssu0' "$log_file"
grep -q 'unmount success logical_dev=/dev/ssu0 allocate_id=alloc-0 host_id=local' "$log_file"
grep -q 'delete_ns success ssu_id=lbc-mock-ssu0 ns_id=1' "$log_file"
grep -q 'delete_ns success ssu_id=lbc-mock-ssu2 ns_id=4' "$log_file"

SSU_MGR_SOCKET="$socket" "$ubsectl" allocate \
    --size 8192 \
    --name data-a \
    --user user-lbc \
    --physical-disks 2 \
    --aggregate \
    --share exclusive \
    --host local \
    --out "$aid_file"
named=$(cat "$aid_file")
test "$named" = "data-a"

SSU_MGR_SOCKET="$socket" "$ubsectl" allocate \
    --size 8192 \
    --name data-a \
    --user user-lbc \
    --physical-disks 2 \
    --aggregate \
    --share exclusive \
    --host local \
    --out "$aid_file"
test "$(cat "$aid_file")" = "data-a"

alloc_rows=$(SSU_MGR_SOCKET="$socket" "$ubsectl" query --type allocation |
    grep -c '^data-a[[:space:]]')
test "$alloc_rows" = "2"

if SSU_MGR_SOCKET="$socket" "$ubsectl" allocate \
    --size 12288 \
    --name data-a \
    --user user-lbc \
    --physical-disks 2 \
    --aggregate \
    --share exclusive \
    --host local; then
    echo "conflicting named allocate should fail" >&2
    exit 1
fi

result=$(SSU_MGR_SOCKET="$socket" "$ubsectl" allocate-result-get --request-id "$named")
dev=$(printf '%s\n' "$result" | sed -n '1p')
test "$dev" = "/dev/ssu/ssu2"
SSU_MGR_SOCKET="$socket" "$ubsectl" free --dev "$dev"
test ! -e "$dev_dir/nvme1n1"
test ! -e "$dev_dir/nvme1n2"
