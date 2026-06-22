#!/bin/sh
set -eu

build_dir=${1:?build directory required}
root="/tmp/ssu-lbc-mock-ubsectl-$$"
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
    : > "$dev_dir/nvme1n1"
    mkdir -p "$configfs_dir/\$subnqn/namespaces/1"
elif [ "\$sample" = "./sample_detach_delete" ]; then
    test "\$nsid" = "1"
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
    cd "$prefix"
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

grep -q 'setup nqn.2025-01.io.ssu:m0 4420' "$log_file"
grep -q -- '--nsze 1048576' "$log_file"
grep -q -- '--nsid 1' "$log_file"
