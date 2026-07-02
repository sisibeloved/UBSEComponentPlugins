#!/bin/sh
set -eu

repo_root=${1:?source root required}
root="/tmp/ubse-self-hosted-lbc-mock-$$"
bin_dir="$root/bin"
dev_dir="$root/dev"
configfs_root="$root/configfs/nvmet"
sys_class_nvme="$root/sys/class/nvme"
state_dir="$root/state"
backing_dir="$root/backing"
log_file="$root/mock.log"
subnqn="nqn.2025-01.io.ssu:m0"

cleanup() {
    rm -rf "$root"
}
trap cleanup EXIT

mkdir -p "$bin_dir" "$dev_dir" "$configfs_root" "$sys_class_nvme" \
    "$state_dir" "$backing_dir"

cat > "$bin_dir/modprobe" <<'EOF'
#!/bin/sh
exit 0
EOF

cat > "$bin_dir/nvme" <<'EOF'
#!/bin/sh
echo "nvme $*" >> "$UBSE_LBC_MOCK_LOG"
exit 0
EOF

cat > "$bin_dir/losetup" <<'EOF'
#!/bin/sh
set -eu
if [ "$1" = "--find" ] && [ "$2" = "--show" ]; then
    mkdir -p "$UBSE_LBC_MOCK_STATE_DIR"
    idx=0
    while [ -e "$UBSE_LBC_MOCK_STATE_DIR/loop-$idx" ]; do
        idx=$((idx + 1))
    done
    printf '%s\n' "/dev/loopubse$idx"
    printf '%s\n' "$3" > "$UBSE_LBC_MOCK_STATE_DIR/loop-$idx"
    exit 0
fi
if [ "$1" = "-d" ]; then
    name=$(basename "$2")
    idx=${name#loopubse}
    rm -f "$UBSE_LBC_MOCK_STATE_DIR/loop-$idx"
    exit 0
fi
echo "unexpected losetup args: $*" >&2
exit 2
EOF

chmod +x "$bin_dir/modprobe" "$bin_dir/nvme" "$bin_dir/losetup"

test -x "$repo_root/mock/setup_mock_target.sh"
test -x "$repo_root/mock/run_mock.sh"
test -x "$repo_root/sample_create_attach"
test -x "$repo_root/sample_detach_delete"

PATH="$bin_dir:$PATH" \
UBSE_LBC_MOCK_TEST_MODE=1 \
UBSE_LBC_MOCK_DEV_DIR="$dev_dir" \
UBSE_LBC_MOCK_CONFIGFS_ROOT="$configfs_root" \
UBSE_LBC_MOCK_SYS_CLASS_NVME="$sys_class_nvme" \
UBSE_LBC_MOCK_STATE_DIR="$state_dir" \
UBSE_LBC_MOCK_BACKING_DIR="$backing_dir" \
UBSE_LBC_MOCK_LOG="$log_file" \
    "$repo_root/mock/setup_mock_target.sh" "$subnqn" 4420

test -d "$configfs_root/subsystems/$subnqn"
test -d "$configfs_root/subsystems/$subnqn/namespaces"
test -d "$configfs_root/ports/4420"

chmod 444 "$configfs_root/subsystems/$subnqn/attr_allow_any_host" \
    "$configfs_root/ports/4420/addr_trtype" \
    "$configfs_root/ports/4420/addr_adrfam" \
    "$configfs_root/ports/4420/addr_traddr" \
    "$configfs_root/ports/4420/addr_trsvcid"

PATH="$bin_dir:$PATH" \
UBSE_LBC_MOCK_TEST_MODE=1 \
UBSE_LBC_MOCK_DEV_DIR="$dev_dir" \
UBSE_LBC_MOCK_CONFIGFS_ROOT="$configfs_root" \
UBSE_LBC_MOCK_SYS_CLASS_NVME="$sys_class_nvme" \
UBSE_LBC_MOCK_STATE_DIR="$state_dir" \
UBSE_LBC_MOCK_BACKING_DIR="$backing_dir" \
UBSE_LBC_MOCK_LOG="$log_file" \
    "$repo_root/mock/setup_mock_target.sh" "$subnqn" 4420

chmod 644 "$configfs_root/subsystems/$subnqn/attr_allow_any_host" \
    "$configfs_root/ports/4420/addr_trtype" \
    "$configfs_root/ports/4420/addr_adrfam" \
    "$configfs_root/ports/4420/addr_traddr" \
    "$configfs_root/ports/4420/addr_trsvcid"

PATH="$bin_dir:$PATH" \
UBSE_LBC_MOCK_TEST_MODE=1 \
UBSE_LBC_MOCK_DEV_DIR="$dev_dir" \
UBSE_LBC_MOCK_CONFIGFS_ROOT="$configfs_root" \
UBSE_LBC_MOCK_SYS_CLASS_NVME="$sys_class_nvme" \
UBSE_LBC_MOCK_STATE_DIR="$state_dir" \
UBSE_LBC_MOCK_BACKING_DIR="$backing_dir" \
UBSE_LBC_MOCK_LOG="$log_file" \
    "$repo_root/mock/run_mock.sh" ./sample_create_attach \
        --dev-ip 127.0.0.1 --port 4420 --sub-nqn "$subnqn" \
        --nsze 1048576

test -e "$dev_dir/nvme1n1"
test -d "$configfs_root/subsystems/$subnqn/namespaces/1"
test -s "$state_dir/$subnqn/1.env"
test -f "$backing_dir/${subnqn}_ns1.img"
test "$(wc -c < "$backing_dir/${subnqn}_ns1.img")" = "536870912"

PATH="$bin_dir:$PATH" \
UBSE_LBC_MOCK_TEST_MODE=1 \
UBSE_LBC_MOCK_DEV_DIR="$dev_dir" \
UBSE_LBC_MOCK_CONFIGFS_ROOT="$configfs_root" \
UBSE_LBC_MOCK_SYS_CLASS_NVME="$sys_class_nvme" \
UBSE_LBC_MOCK_STATE_DIR="$state_dir" \
UBSE_LBC_MOCK_BACKING_DIR="$backing_dir" \
UBSE_LBC_MOCK_LOG="$log_file" \
    "$repo_root/mock/run_mock.sh" ./sample_create_attach \
        --dev-ip 127.0.0.1 --port 4420 --sub-nqn "$subnqn" \
        --nsze 2048

test -e "$dev_dir/nvme1n2"
test -d "$configfs_root/subsystems/$subnqn/namespaces/2"

PATH="$bin_dir:$PATH" \
UBSE_LBC_MOCK_TEST_MODE=1 \
UBSE_LBC_MOCK_DEV_DIR="$dev_dir" \
UBSE_LBC_MOCK_CONFIGFS_ROOT="$configfs_root" \
UBSE_LBC_MOCK_SYS_CLASS_NVME="$sys_class_nvme" \
UBSE_LBC_MOCK_STATE_DIR="$state_dir" \
UBSE_LBC_MOCK_BACKING_DIR="$backing_dir" \
UBSE_LBC_MOCK_LOG="$log_file" \
    "$repo_root/mock/run_mock.sh" ./sample_detach_delete \
        --nsid 1 --dev-path "$dev_dir/nvme1n1" \
        --dev-ip 127.0.0.1 --port 4420 --sub-nqn "$subnqn"

test ! -e "$dev_dir/nvme1n1"
test ! -d "$configfs_root/subsystems/$subnqn/namespaces/1"
test ! -e "$state_dir/$subnqn/1.env"
test ! -e "$backing_dir/${subnqn}_ns1.img"
test -e "$dev_dir/nvme1n2"
test -d "$configfs_root/subsystems/$subnqn/namespaces/2"
