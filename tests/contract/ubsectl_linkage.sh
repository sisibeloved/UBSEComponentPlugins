#!/bin/sh
set -eu

ubsectl=${1:?ubsectl path required}
needed=$(readelf -d "$ubsectl")

printf '%s\n' "$needed" | grep -q 'libssu_plugin_' && {
    echo "ubsectl must not depend on vendor plugins" >&2
    exit 1
}

printf '%s\n' "$needed" | grep -q 'libssu_api.so' && {
    echo "ubsectl must not depend on server-side libssu_api.so" >&2
    exit 1
}

printf '%s\n' "$needed" | grep -q 'libssu_controller.so' && {
    echo "ubsectl must not depend on server-side libssu_controller.so" >&2
    exit 1
}

exit 0
