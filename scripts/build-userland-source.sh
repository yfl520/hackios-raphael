#!/usr/bin/env bash
# No Apple system image or prebuilt project object is read by this build.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source "$ROOT/xnu-env.sh"
cd "$ROOT/userland"
OUT="$ROOT/build/artifacts/userland"
OBJ="$ROOT/build/userland-obj"
mkdir -p "$OUT" "$OBJ"
SDK="$THEOS/sdks/iPhoneOS16.5.sdk"
static=(xnu-clang -arch arm64 -Os -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin -Wall -Wextra)
dynamic=(xnu-clang -arch arm64 -isysroot "$SDK" -O1)
"${static[@]}" -Ilibmini -c libmini/mini.c -o "$OBJ/mini.o"
"${static[@]}" -c libmini/start.S -o "$OBJ/start.o"
"${static[@]}" -Wl,-e,_start -Wl,-pagezero_size,0x100000000 -o "$OUT/hello" src/hello.c
for name in init child var_store; do
    "${static[@]}" -Ilibmini -Wl,-e,_start -Wl,-pagezero_size,0x100000000 -o "$OUT/$name" "src/$name.c" "$OBJ/mini.o" "$OBJ/start.o"
done
"${dynamic[@]}" -dynamiclib -install_name /redwood/lib/libhakios.dylib -framework CoreFoundation -framework IOKit -lobjc -o "$OUT/libhakios.dylib" src/inject.c
"${dynamic[@]}" -dynamiclib -install_name /redwood/lib/libhakios.sb.dylib -framework CoreFoundation -framework IOKit -lobjc -o "$OUT/libhakios.sb.dylib" src/inject_sb.c
"${dynamic[@]}" -dynamiclib -install_name /redwood/lib/libhakios.tweaks.dylib -framework CoreFoundation -lobjc -o "$OUT/libhakios.tweaks.dylib" src/inject_tweaks.c
"${dynamic[@]}" -dynamiclib -install_name /redwood/lib/libhakios.app.dylib -lobjc -o "$OUT/libhakios.app.dylib" src/inject_app.c
"${dynamic[@]}" -O2 -dynamiclib -install_name /redwood/lib/libhakios.media.dylib -framework CoreFoundation -framework CoreMedia -framework CoreVideo -framework IOSurface -lobjc -o "$OUT/libhakios.media.dylib" src/inject_media.c src/camera_device.c src/camera_isp.c
"${dynamic[@]}" -O2 -dynamiclib -install_name /redwood/lib/libhakios.store.dylib -framework CoreFoundation -lobjc -lc++abi -o "$OUT/libhakios.store.dylib" src/inject_store.c
"${dynamic[@]}" -O2 -dynamiclib -install_name /redwood/lib/libhakios.fairplay.dylib -framework CoreFoundation -framework IOKit -framework Security -lMobileGestalt -o "$OUT/libhakios.fairplay.dylib" src/inject_fairplay.c
"${dynamic[@]}" -bundle -framework CoreFoundation -o "$OUT/HakiosAudio" src/audio_plugin.c
"${dynamic[@]}" -fblocks -Wall -Wextra -o "$OUT/BlueTool_raphael" src/bluetool_raphael.c
"$THEOS/toolchain/linux/iphone/bin/ldid" -Cadhoc -S "$OUT/BlueTool_raphael"
M="$ROOT/xnu-deps/build/mesa/out"
test -s "$M/libOSMesa.8.dylib"
python3 mkeagl.py src/eagl_slots.txt "$M/gl.xml" "$M/exported-gl.txt" src/eagl.c > "$OBJ/eagl_gen.h"
"${dynamic[@]}" -dynamiclib -install_name /redwood/lib/libhakios.gl.dylib -I"$OBJ" -framework CoreFoundation -framework IOKit -framework IOSurface -lobjc "$M/libOSMesa.8.dylib" -o "$OUT/libhakios.gl.dylib" src/eagl.c src/gl_osmesa.c
cp src/audio_plugin.plist "$OUT/HakiosAudio.Info.plist"
printf 'SOURCE_USERLAND_COMPILE_OK\n'
