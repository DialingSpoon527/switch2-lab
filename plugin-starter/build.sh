#!/bin/sh
set -eu
cd "$(dirname "$0")"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export LIBOUNCE="${LIBOUNCE:-$DEVKITPRO/libounce}"
kaihen=$(cd ../hen-updater && pwd)
make -C "$kaihen" -j2
for example in ping-plugin plugin-control; do
    make -C "examples/$example" KAIHEN_SOURCE="$kaihen" -j2
done
printf '%s
' 'Built examples remain in examples/. New builds have not inherited the included binaries hardware result.'
