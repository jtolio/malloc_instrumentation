#!/bin/bash
#
# For running a binary with formatted malloc instrumentation. Usage like:
#   malloc_run.sh <command>
#

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd)"

if ! [ -e "$DIR/malloc_instrument.so" ]; then
    ( cd $DIR; make )
fi

if ! [ -e "$DIR/malloc_instrument.so" ]; then
    echo "can't find malloc_instrument.so"
    exit 1
fi

( LD_PRELOAD="$DIR/malloc_instrument.so" "$@" 2>&1 ) | \
        python -u "$DIR/malloc_format.py"
