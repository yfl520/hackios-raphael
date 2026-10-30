#!/usr/bin/env bash
# Compile only source-owned components; this does not boot or flash a device.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
export XNU_SRC="$ROOT/kernel"
source "$ROOT/xnu-env.sh"
export JOBS=${JOBS:-6}
stage=${1:-all}
mkdir -p "$ROOT/build/artifacts" "$ROOT/build/reports"
run_stage() {
    case "$1" in
    deps)
        bash "$ROOT/xnu-deps/setup.sh" --stage1
        bash "$ROOT/xnu-deps/setup.sh" --stage2
        ;;
    wifi)
        python3 "$ROOT/tools/mk-ath10k.py"
        cp "$ROOT/xnu-deps/build/ath10k/out/ath10k.o" "$ROOT/build/artifacts/ath10k.o"
        ;;
    kernel)
        test -s "$ROOT/xnu-deps/build/ath10k/out/ath10k.o"
        WIFI_OBJECT="$ROOT/xnu-deps/build/ath10k/out/ath10k.o" bash "$ROOT/xnu-build.sh" -j"$JOBS"
        cp "$ROOT/kernel/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837" "$ROOT/build/artifacts/kernel.macho"
        ;;
    mesa)
        bash "$ROOT/tools/mk-mesa.sh"
        cp "$ROOT/xnu-deps/build/mesa/out/libOSMesa.8.dylib" "$ROOT/build/artifacts/"
        ;;
    userland)
        bash "$ROOT/scripts/build-userland-source.sh"
        ;;
    loader)
        if [ ! -s "$ROOT/xnuloader/identity.h" ]; then
            echo 'Missing identity.h: run xnuloader/mkidentity.sh with your own DeviceTree.im4p.' >&2
            exit 1
        fi
        bash "$ROOT/xnuloader/build.sh"
        cp "$ROOT/xnuloader/loader.efi" "$ROOT/build/artifacts/"
        ;;
    *) echo "Unknown stage: $1" >&2;exit 2;;
    esac
}
if [ "$stage" = all ]; then
    for item in deps wifi kernel mesa userland loader; do run_stage "$item"; done
else
    run_stage "$stage"
fi
python3 "$ROOT/scripts/verify-products.py"
