#!/bin/bash
# Build libjackpotlte-fix.dylib (arm64 iOS 16) on Linux with clang + ld64.lld:
# no SDK, the two system libraries come from the small .tbd stubs here.
#   userspace/jackpotlte-fix/build.sh [OUT]
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/libjackpotlte-fix.dylib}
ld=$(command -v ld64.lld || echo /usr/lib/llvm-18/bin/ld64.lld)
clang --target=arm64-apple-ios16.0 -O2 -Wall -Wextra -Werror -fno-stack-protector \
    -fno-builtin -nostdlib -fblocks -c "$here/fix.c" -o "$out.o"
"$ld" -arch arm64 -platform_version ios 16.0 16.0 -dylib -adhoc_codesign \
    -install_name /private/var/mobile/Library/libjackpotlte-fix.dylib \
    -o "$out" "$out.o" "$here/libobjc.tbd" "$here/libSystem.tbd" "$here/Foundation.tbd" "$here/libMobileGestalt.tbd" \
    "$here/CoreGraphics.tbd" "$here/CoreFoundation.tbd"
rm -f "$out.o"
echo "$out: $(stat -c %s "$out") bytes"
