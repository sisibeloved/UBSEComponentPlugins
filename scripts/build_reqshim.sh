#!/bin/sh
set -eu

kernel_src_dir=$1
out_dir=$2
shift 2

mkdir -p "$out_dir"

for input in "$@"; do
    cp "$input" "$out_dir/"
done

make -C "$kernel_src_dir" M="$out_dir" modules
