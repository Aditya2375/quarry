#!/bin/sh
# Rebuild the browser demo in docs/ (requires emsdk on PATH).
set -e
cd "$(dirname "$0")/.."
emcc -O2 -Isrc src/pager.c src/btree.c src/record.c src/sql.c src/db.c wasm/quarry_wasm.c \
    -o docs/quarry.js \
    -sEXPORTED_FUNCTIONS=_quarry_exec,_quarry_reset,_quarry_tables \
    -sEXPORTED_RUNTIME_METHODS=ccall,cwrap \
    -sMODULARIZE=1 -sEXPORT_NAME=QuarryModule \
    -sSTACK_SIZE=4194304 -sALLOW_MEMORY_GROWTH=1
