#!/bin/sh
set -eu

sdk_so=${1:?libubse_ssu_sdk.so path required}
needed=$(readelf -d "$sdk_so")

printf '%s\n' "$needed" | grep -q 'libssu_plugin_' && {
    echo "SDK must not depend on vendor plugins" >&2
    exit 1
}

printf '%s\n' "$needed" | grep -q 'libssu_api.so' && {
    echo "SDK must not depend on server-side libssu_api.so" >&2
    exit 1
}

printf '%s\n' "$needed" | grep -q 'libssu_controller.so' && {
    echo "SDK must not depend on server-side libssu_controller.so" >&2
    exit 1
}

exit 0
