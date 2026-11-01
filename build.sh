#!/bin/sh
set -e
export CC=clang
export CFLAGS="-Wall -Wextra -O3 -march=native -flto"
export LDFLAGS="-larchive -O3 -flto -s"

$CC $CFLAGS avp.c -o avp $LDFLAGS

echo "ok"
