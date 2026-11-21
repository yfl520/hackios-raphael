#!/usr/bin/env bash
# Optional persistence init. Default userland build keeps its RAM init.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source "$ROOT/xnu-env.sh"
OUT="$ROOT/build/artifacts/persistence"
OBJ="$ROOT/build/persistence-obj"
mkdir -p "$OUT" "$OBJ"
CC=(xnu-clang -arch arm64 -Os -static -nostdlib -ffreestanding
    -fno-stack-protector -fno-builtin -Wall -Wextra)
"${CC[@]}" -I"$ROOT/userland/libmini" -c "$ROOT/userland/libmini/mini.c" -o "$OBJ/mini.o"
"${CC[@]}" -c "$ROOT/userland/libmini/start.S" -o "$OBJ/start.o"
"${CC[@]}" -I"$ROOT/userland/libmini" -Wl,-e,_start -Wl,-pagezero_size,0x100000000 \
    "$ROOT/components/persistence/init.c" "$OBJ/mini.o" "$OBJ/start.o" -o "$OUT/init.persistence"
