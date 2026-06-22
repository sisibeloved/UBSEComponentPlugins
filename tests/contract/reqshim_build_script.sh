#!/bin/sh
set -eu

source_root=${1:?source root required}
root="${TMPDIR:-/tmp}/reqshim-build-script-$$"
work_dir="$root/work"
kernel_dir="$root/fake-kernel"
input_dir="$root/inputs"
out_dir="relative-out/src/kernel/reqshim"

cleanup() {
    rm -rf "$root"
}
trap cleanup EXIT

mkdir -p "$work_dir" "$kernel_dir" "$input_dir"

cat > "$kernel_dir/Makefile" <<'EOF'
modules:
	@case "$(M)" in /*) ;; *) echo "M must be absolute, got $(M)" >&2; exit 2;; esac
	@test -f "$(M)/Makefile" || { echo "missing module Makefile at $(M)" >&2; exit 3; }
	@touch "$(M)/ssu_reqshim.ko"
EOF

for file in Kbuild Makefile reqshim_blk.c reqshim_cmd.c reqshim_ioctl.c \
    reqshim_internal.h reqshim_main.c reqshim_map.c reqshim_phys.c \
    reqshim_uapi.h; do
    printf '/* %s */\n' "$file" > "$input_dir/$file"
done

(
    cd "$work_dir"
    "$source_root/scripts/build_reqshim.sh" \
        "$kernel_dir" \
        "$out_dir" \
        "$input_dir/Kbuild" \
        "$input_dir/Makefile" \
        "$input_dir/reqshim_blk.c" \
        "$input_dir/reqshim_cmd.c" \
        "$input_dir/reqshim_ioctl.c" \
        "$input_dir/reqshim_internal.h" \
        "$input_dir/reqshim_main.c" \
        "$input_dir/reqshim_map.c" \
        "$input_dir/reqshim_phys.c" \
        "$input_dir/reqshim_uapi.h"
)

test -f "$work_dir/$out_dir/ssu_reqshim.ko"
