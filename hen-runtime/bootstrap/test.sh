#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
cd "$(dirname "$0")"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined tests/autostart_test.cpp -o "$work/autostart-test"
"$work/autostart-test"
