#!/bin/bash
# local build of the GL shim (same flags as userspace/build.sh, clang 18 + ld64.lld + theos iPhoneOS16.5 SDK)
set -euo pipefail
# needs clang 18, ld64.lld (a link named ld64.lld to lld on PATH) and theos/sdks iPhoneOS16.5.sdk
SDK=${SDK:-$HOME/theos-sdks/iPhoneOS16.5.sdk}
gl=$(cd "$(dirname "$0")/../../userspace/jackpotlte-gl" && pwd)
out=${1:-/tmp/glb/libjackpotlte-gl.dylib}
mkdir -p "$(dirname "$out")"
clang --target=arm64-apple-ios16.0 -isysroot $SDK -O2 -Wall -Wextra -Werror -Wpedantic \
  -Wl,-undefined,dynamic_lookup -fuse-ld=lld -I $gl -dynamiclib \
  -Wl,-install_name,/private/var/mobile/Library/libjackpotlte-gl.dylib \
  -o "$out" $gl/gl_slots.c $gl/jackpotlte_gl_osmesa.c $gl/eagl.c $gl/ca_fixes.c $gl/trace.c $gl/trap.c
llvm-strip-18 -x -o "${out%.dylib}.stripped.dylib" "$out"
ls -l "$out" "${out%.dylib}.stripped.dylib"
sha256sum "${out%.dylib}.stripped.dylib"
