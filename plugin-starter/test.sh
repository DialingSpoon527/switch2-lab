#!/bin/sh
set -eu
cd "$(dirname "$0")"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export LIBOUNCE="${LIBOUNCE:-$DEVKITPRO/libounce}"
kaihen=$(cd ../hen-updater && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
python3 -m unittest discover -s tests -p 'test_controller*.py' -v
python3 test-coordination.py
g++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined     -I"$kaihen/common" tests/paced_launch_test.cpp -o "$work/paced"
"$work/paced"
for test in private-sm runtime; do
    if [ "$test" = private-sm ]; then file=tests/private_sm_lookup_test.cpp; else file=host_tests/lifecycle_runtime_test.cpp; fi
    g++ -std=gnu++20 -O1 -Wall -Wextra -Werror -Wno-missing-field-initializers         -ffunction-sections -fdata-sections -Wl,--gc-sections -pthread         -DLIBNX_HOST_TEST_TLS -D__SWITCH__ -D__SWITCH2__         -Ihost_tests/shims -I"$LIBOUNCE/include" -I"$kaihen/hen/source"         -I"$kaihen/libkaihen/source" -I"$kaihen/libkaihen/include"         "$file" -o "$work/$test"
    "$work/$test"
done
