#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source "$ROOT/xnu-env.sh"
OUT="$ROOT/build/artifacts/userland"
mkdir -p "$OUT"
xnu-clang -arch arm64 -isysroot "$THEOS/sdks/iPhoneOS16.5.sdk" \
    -miphoneos-version-min=16.5 -O1 -Wall -Wextra -Wno-unused-variable \
    -dynamiclib -install_name /redwood/lib/libhakios.cellular-data.dylib \
    "$ROOT/components/cellular/commcenter-cellular-release.c" \
    -o "$OUT/libhakios.cellular-data.dylib"
"$THEOS/toolchain/linux/iphone/bin/ldid" -Cadhoc -S "$OUT/libhakios.cellular-data.dylib"
