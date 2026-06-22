#!/bin/sh
set -eu

kernel_src_dir=$1
out_dir=$2
shift 2

mkdir -p "$out_dir"
kernel_src_dir=$(cd "$kernel_src_dir" && pwd -P)
out_dir=$(cd "$out_dir" && pwd -P)

for input in "$@"; do
    cp "$input" "$out_dir/"
done

make -C "$kernel_src_dir" M="$out_dir" modules
