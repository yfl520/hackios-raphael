#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
mkdir -p "$ROOT/build/host-tests"
for name in bt-power legacy-hci rx-packets uart-speed charge-policy charge-state qc-detect; do
    clang -std=c99 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
        -fno-sanitize-recover=all -I"$ROOT/kernel/bsd/dev" \
        "$ROOT/tests/test-$name.c" -o "$ROOT/build/host-tests/$name"
    "$ROOT/build/host-tests/$name"
done
