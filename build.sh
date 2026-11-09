#!/bin/sh
set -e
export CC=cc
export CFLAGS="-Wall -Wextra -O3 -march=native -flto"
export LDFLAGS="-larchive -lcurl -O3 -flto -s"

$CC $CFLAGS avp.c -o avp $LDFLAGS

echo "ok"
