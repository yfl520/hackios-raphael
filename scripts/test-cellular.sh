#!/usr/bin/env bash
# Portable protocol tests; no phone, system image or radio firmware is read.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT="$ROOT/build/cellular-tests"
mkdir -p "$OUT"
for name in bridge ipa-qmi gsi2; do
    clang -std=c11 -D_DEFAULT_SOURCE -include strings.h -O1 -g -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-variable \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I"$ROOT/kernel/iokit/Kernel" -I"$ROOT/kernel/bsd/dev" \
        "$ROOT/components/cellular/test-$name.c" -o "$OUT/$name"
    "$OUT/$name"
done
clang++ -std=c++17 -O1 -g -Wall -Wextra -Werror \
    -Wno-unused-function -Wno-unused-variable -fsanitize=address,undefined \
    "$ROOT/components/cellular/tests/queue.cpp" -o "$OUT/queue"
"$OUT/queue"
cp "$ROOT/xnuloader/resource-config.example.h" "$OUT/resource-config.h"
clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$OUT" "$ROOT/components/cellular/tests/header-reader.c" -o "$OUT/header-reader"
"$OUT/header-reader"
