#!/bin/sh
set -eu

usage() {
    echo "usage: run_mock.sh ./sample_create_attach|./sample_detach_delete [args]" >&2
}

if [ "$#" -lt 1 ]; then
    usage
    exit 2
fi

sample=$1
shift

dev_dir=${UBSE_LBC_MOCK_DEV_DIR:-/dev}
configfs_root=${UBSE_LBC_MOCK_CONFIGFS_ROOT:-/sys/kernel/config/nvmet}
sys_class_nvme=${UBSE_LBC_MOCK_SYS_CLASS_NVME:-/sys/class/nvme}
state_dir=${UBSE_LBC_MOCK_STATE_DIR:-/run/ubse-lbc-mock}
backing_dir=${UBSE_LBC_MOCK_BACKING_DIR:-/var/lib/ubse-lbc-mock}
fabrics_device=${UBSE_LBC_MOCK_FABRICS_DEVICE:-/dev/nvme-fabrics}
log_file=${UBSE_LBC_MOCK_LOG:-/tmp/ubse-lbc-mock.log}
test_mode=${UBSE_LBC_MOCK_TEST_MODE:-0}

dev_ip=127.0.0.1
port=
subnqn=
nsze=
nsid=
dev_path=

log() {
    mkdir -p "$(dirname "$log_file")"
    printf '%s run %s\n' "$(date '+%Y-%m-%dT%H:%M:%S%z')" "$*" >> "$log_file"
}

fail() {
    log "failed: sample=$sample $*"
    echo "run_mock: $*" >&2
    exit 1
}

write_attr() {
    path=$1
    value=$2
    if [ -e "$path" ] || [ "$test_mode" = "1" ]; then
        printf '%s\n' "$value" > "$path"
    else
        fail "missing configfs attribute $path"
    fi
}

parse_args() {
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --dev-ip)
                [ "$#" -ge 2 ] || fail "missing value for --dev-ip"
                dev_ip=$2
                shift 2
                ;;
            --port)
                [ "$#" -ge 2 ] || fail "missing value for --port"
                port=$2
                shift 2
                ;;
            --sub-nqn)
                [ "$#" -ge 2 ] || fail "missing value for --sub-nqn"
                subnqn=$2
                shift 2
                ;;
            --nsze)
                [ "$#" -ge 2 ] || fail "missing value for --nsze"
                nsze=$2
                shift 2
                ;;
            --nsid)
                [ "$#" -ge 2 ] || fail "missing value for --nsid"
                nsid=$2
                shift 2
                ;;
            --dev-path)
                [ "$#" -ge 2 ] || fail "missing value for --dev-path"
                dev_path=$2
                shift 2
                ;;
            *)
                fail "unknown argument $1"
                ;;
        esac
    done
}

require_common_args() {
    [ -n "$port" ] || fail "missing --port"
    [ -n "$subnqn" ] || fail "missing --sub-nqn"
    case "$port" in
        ''|*[!0-9]*) fail "invalid --port $port" ;;
    esac
    case "$subnqn" in
        "") fail "empty --sub-nqn" ;;
        */*) fail "--sub-nqn must not contain slash: $subnqn" ;;
    esac
}

namespace_root() {
    printf '%s/subsystems/%s/namespaces\n' "$configfs_root" "$subnqn"
}

safe_subnqn() {
    printf '%s' "$subnqn" | tr '/' '_'
}

state_subdir() {
    printf '%s/%s\n' "$state_dir" "$subnqn"
}

state_file() {
    printf '%s/%s.env\n' "$(state_subdir)" "$1"
}

state_get() {
    file=$1
    key=$2
    [ -r "$file" ] || return 1
    sed -n "s/^$key=//p" "$file" | sed -n '1p'
}

next_nsid() {
    root=$(namespace_root)
    i=1
    while [ -e "$root/$i" ]; do
        i=$((i + 1))
    done
    printf '%s\n' "$i"
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

    if controller_exists; then
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

    controller_exists || fail "nvme connect failed subnqn=$subnqn dev_ip=$dev_ip port=$port"
}

rescan_controllers() {
    [ -d "$sys_class_nvme" ] || return 0
    for ctrl in "$sys_class_nvme"/nvme*; do
        [ -d "$ctrl" ] || continue
        [ -r "$ctrl/subsysnqn" ] || continue
        [ "$(sed -n '1p' "$ctrl/subsysnqn")" = "$subnqn" ] || continue
        if [ -e "$ctrl/rescan_controller" ] || [ "$test_mode" = "1" ]; then
            printf '1\n' > "$ctrl/rescan_controller"
        fi
    done
}

fake_dev_path() {
    printf '%s/nvme1n%s\n' "$dev_dir" "$1"
}

create_fake_dev() {
    [ "$test_mode" = "1" ] || return 0
    mkdir -p "$dev_dir"
    : > "$(fake_dev_path "$1")"
}

delete_host_dev() {
    path=$1
    if [ "$test_mode" = "1" ]; then
        rm -f "$path"
        return 0
    fi

    base=$(basename "$path")
    if [ -e "/sys/class/block/$base/device/delete" ]; then
        printf '1\n' > "/sys/class/block/$base/device/delete" || true
    fi
}

create_attach() {
    require_common_args
    [ -n "$nsze" ] || fail "missing --nsze"
    case "$nsze" in
        ''|*[!0-9]*) fail "invalid --nsze $nsze" ;;
    esac
    [ "$nsze" -gt 0 ] || fail "--nsze must be greater than zero"

    ns_root=$(namespace_root)
    [ -d "$ns_root" ] || fail "target not set up for subnqn=$subnqn"

    mkdir -p "$(state_subdir)" "$backing_dir"
    new_nsid=$(next_nsid)
    backing_file="$backing_dir/$(safe_subnqn)_ns${new_nsid}.img"
    size_bytes=$((nsze * 512))

    truncate -s "$size_bytes" "$backing_file" ||
        fail "cannot create backing file $backing_file size=$size_bytes"

    loop_dev=$(losetup --find --show "$backing_file") ||
        fail "cannot attach loop device for $backing_file"

    ns_dir="$ns_root/$new_nsid"
    if ! mkdir "$ns_dir"; then
        losetup -d "$loop_dev" >/dev/null 2>&1 || true
        rm -f "$backing_file"
        fail "cannot create namespace $new_nsid"
    fi

    if ! write_attr "$ns_dir/device_path" "$loop_dev"; then
        rmdir "$ns_dir" >/dev/null 2>&1 || true
        losetup -d "$loop_dev" >/dev/null 2>&1 || true
        rm -f "$backing_file"
        exit 1
    fi

    if ! write_attr "$ns_dir/enable" 1; then
        rmdir "$ns_dir" >/dev/null 2>&1 || true
        losetup -d "$loop_dev" >/dev/null 2>&1 || true
        rm -f "$backing_file"
        exit 1
    fi

    {
        printf 'subnqn=%s\n' "$subnqn"
        printf 'nsid=%s\n' "$new_nsid"
        printf 'nsze=%s\n' "$nsze"
        printf 'size_bytes=%s\n' "$size_bytes"
        printf 'loop_dev=%s\n' "$loop_dev"
        printf 'backing_file=%s\n' "$backing_file"
    } > "$(state_file "$new_nsid")"

    connect_target
    rescan_controllers
    create_fake_dev "$new_nsid"
    log "create_attach success subnqn=$subnqn nsid=$new_nsid nsze=$nsze loop=$loop_dev backing=$backing_file"
}

detach_delete() {
    require_common_args
    [ -n "$nsid" ] || fail "missing --nsid"
    [ -n "$dev_path" ] || fail "missing --dev-path"
    case "$nsid" in
        ''|*[!0-9]*) fail "invalid --nsid $nsid" ;;
    esac

    ns_root=$(namespace_root)
    ns_dir="$ns_root/$nsid"
    file=$(state_file "$nsid")
    loop_dev=
    backing_file=

    if [ -r "$file" ]; then
        loop_dev=$(state_get "$file" loop_dev || true)
        backing_file=$(state_get "$file" backing_file || true)
    elif [ -r "$ns_dir/device_path" ]; then
        loop_dev=$(sed -n '1p' "$ns_dir/device_path")
    fi

    delete_host_dev "$dev_path"
    if [ -e "$ns_dir/enable" ]; then
        printf '0\n' > "$ns_dir/enable" || true
    fi
    if [ "$test_mode" = "1" ]; then
        rm -f "$ns_dir/device_path" "$ns_dir/enable"
    fi
    if [ -d "$ns_dir" ]; then
        rmdir "$ns_dir" || fail "cannot remove namespace $nsid"
    fi

    if [ -n "$loop_dev" ]; then
        losetup -d "$loop_dev" >/dev/null 2>&1 || true
    fi
    if [ -n "$backing_file" ]; then
        rm -f "$backing_file"
    fi
    rm -f "$file"
    rescan_controllers
    log "detach_delete success subnqn=$subnqn nsid=$nsid dev_path=$dev_path loop=$loop_dev backing=$backing_file"
}

parse_args "$@"

case "$sample" in
    ./sample_create_attach|sample_create_attach)
        create_attach
        ;;
    ./sample_detach_delete|sample_detach_delete)
        detach_delete
        ;;
    *)
        usage
        fail "unsupported sample $sample"
        ;;
esac
