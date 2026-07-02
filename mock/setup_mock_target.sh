#!/bin/sh
set -eu

usage() {
    echo "usage: setup_mock_target.sh SUBNQN PORT" >&2
}

if [ "$#" -ne 2 ]; then
    usage
    exit 2
fi

subnqn=$1
port=$2
configfs_root=${UBSE_LBC_MOCK_CONFIGFS_ROOT:-/sys/kernel/config/nvmet}
sys_class_nvme=${UBSE_LBC_MOCK_SYS_CLASS_NVME:-/sys/class/nvme}
dev_ip=${UBSE_LBC_MOCK_DEV_IP:-127.0.0.1}
fabrics_device=${UBSE_LBC_MOCK_FABRICS_DEVICE:-/dev/nvme-fabrics}
log_file=${UBSE_LBC_MOCK_LOG:-/tmp/ubse-lbc-mock.log}
test_mode=${UBSE_LBC_MOCK_TEST_MODE:-0}

log() {
    mkdir -p "$(dirname "$log_file")"
    printf '%s setup %s\n' "$(date '+%Y-%m-%dT%H:%M:%S%z')" "$*" >> "$log_file"
}

fail() {
    log "failed: $*"
    echo "setup_mock_target: $*" >&2
    exit 1
}

write_attr() {
    path=$1
    value=$2
    if [ -e "$path" ] || [ "$test_mode" = "1" ]; then
        if [ -r "$path" ] && [ "$(sed -n '1p' "$path" 2>/dev/null || true)" = "$value" ]; then
            return 0
        fi
        printf '%s\n' "$value" > "$path"
    fi
}

controller_exists() {
    [ -d "$sys_class_nvme" ] || return 1
    for ctrl in "$sys_class_nvme"/nvme*; do
        [ -d "$ctrl" ] || continue
        [ -r "$ctrl/subsysnqn" ] || continue
        if [ "$(sed -n '1p' "$ctrl/subsysnqn")" = "$subnqn" ]; then
            return 0
        fi
    done
    return 1
}

wait_for_controller() {
    i=0
    while [ "$i" -lt 50 ]; do
        if controller_exists; then
            return 0
        fi
        i=$((i + 1))
        sleep 0.1
    done
    return 1
}

connect_target() {
    if [ "$test_mode" = "1" ]; then
        mkdir -p "$sys_class_nvme/nvme1"
        printf '%s\n' "$subnqn" > "$sys_class_nvme/nvme1/subsysnqn"
        : > "$sys_class_nvme/nvme1/rescan_controller"
        return 0
    fi

    if ! command -v nvme >/dev/null 2>&1; then
        if [ ! -w "$fabrics_device" ]; then
            fail "nvme CLI not found and $fabrics_device is not writable"
        fi
        if ! printf 'transport=tcp,traddr=%s,trsvcid=%s,nqn=%s\n' \
            "$dev_ip" "$port" "$subnqn" > "$fabrics_device"; then
            wait_for_controller ||
                fail "nvme fabrics connect failed subnqn=$subnqn dev_ip=$dev_ip port=$port"
            log "fabrics connect reported busy but controller is present subnqn=$subnqn"
            return 0
        fi
        wait_for_controller ||
            fail "nvme fabrics connect timed out subnqn=$subnqn dev_ip=$dev_ip port=$port"
        return 0
    fi

    if nvme connect -t tcp -a "$dev_ip" -s "$port" -n "$subnqn" \
        >> "$log_file" 2>&1; then
        wait_for_controller ||
            fail "nvme connect timed out subnqn=$subnqn dev_ip=$dev_ip port=$port"
        return 0
    fi

    if controller_exists; then
        log "connect skipped: controller already connected subnqn=$subnqn"
        return 0
    fi

    fail "nvme connect failed subnqn=$subnqn dev_ip=$dev_ip port=$port"
}

case "$subnqn" in
    "") fail "empty SUBNQN" ;;
    */*) fail "SUBNQN must not contain slash: $subnqn" ;;
esac

case "$port" in
    ''|*[!0-9]*) fail "invalid PORT: $port" ;;
esac

if [ "$test_mode" != "1" ]; then
    modprobe nvmet >/dev/null 2>&1 || true
    modprobe nvmet-tcp >/dev/null 2>&1 || true
    modprobe nvme-tcp >/dev/null 2>&1 || true
fi

subsys_dir="$configfs_root/subsystems/$subnqn"
port_dir="$configfs_root/ports/$port"

mkdir -p "$subsys_dir/namespaces" "$port_dir/subsystems" ||
    fail "cannot create nvmet configfs directories under $configfs_root"

write_attr "$subsys_dir/attr_allow_any_host" 1
write_attr "$port_dir/addr_trtype" tcp
write_attr "$port_dir/addr_adrfam" ipv4
write_attr "$port_dir/addr_traddr" "$dev_ip"
write_attr "$port_dir/addr_trsvcid" "$port"

if [ ! -e "$port_dir/subsystems/$subnqn" ]; then
    ln -s "$subsys_dir" "$port_dir/subsystems/$subnqn" ||
        fail "cannot link subsystem $subnqn to port $port"
fi

connect_target
log "success subnqn=$subnqn port=$port dev_ip=$dev_ip configfs_root=$configfs_root"
