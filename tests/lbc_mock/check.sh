#!/bin/sh
set -eu

build_dir=${1:?build directory required}
lbc_prefix=${2:?LBC INI mock prefix directory required}

test -r "$lbc_prefix/mock/setup_mock_target.sh"
test -r "$lbc_prefix/mock/run_mock.sh"
test -x "$build_dir/tests/lbc_mock/lbc_mock_plugin_flow"

"$build_dir/tests/lbc_mock/lbc_mock_plugin_flow" \
    --real-prefix "$lbc_prefix"
