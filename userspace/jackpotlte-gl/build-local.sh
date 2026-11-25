#!/bin/bash
# Build libjackpotlte-gl.dylib on Linux without CI (11-25): clang + ld64.lld
# against an iPhoneOS SDK's headers and .tbd stubs.  The SDK is NOT in this
# repository (Apple's files): fetch only the parts needed, outside the repo:
#   git clone --depth 1 --filter=blob:none --sparse https://github.com/theos/sdks SDKDIR
#   cd SDKDIR && git sparse-checkout set --no-cone 'iPhoneOS16.5.sdk/usr/include/' \
#       '/iPhoneOS16.5.sdk/usr/lib/libSystem.tbd' '/iPhoneOS16.5.sdk/usr/lib/libSystem.B.tbd' \
#       '/iPhoneOS16.5.sdk/usr/lib/libobjc.tbd' '/iPhoneOS16.5.sdk/usr/lib/libobjc.A.tbd' \
#       'iPhoneOS16.5.sdk/usr/lib/system/'
#   userspace/jackpotlte-gl/build-local.sh SDKDIR/iPhoneOS16.5.sdk OUT.dylib
# Same flags as userspace/build.sh (-O2 -Wall -Wextra -Werror -Wpedantic,
# -undefined dynamic_lookup); checked 11-25: glca22e built here ran on the
# phone like the CI copy (a8island 16/16, Control Center speed the same).
set -euo pipefail
sdk=$1
out=$2
here=$(cd "$(dirname "$0")" && pwd)
ld=$(command -v ld64.lld || echo /usr/lib/llvm-18/bin/ld64.lld)
obj=$(mktemp -d)
for f in gl_slots jackpotlte_gl_osmesa eagl ca_fixes trace trap; do
	clang --target=arm64-apple-ios16.0 -isysroot "$sdk" -O2 -Wall -Wextra -Werror -Wpedantic \
		-I "$here" -c "$here/$f.c" -o "$obj/$f.o"
done
"$ld" -arch arm64 -platform_version ios 16.0 16.5 -dylib -adhoc_codesign \
	-install_name /private/var/mobile/Library/libjackpotlte-gl.dylib -undefined dynamic_lookup \
	-o "$out" "$obj"/*.o -L"$sdk/usr/lib" -lSystem -lobjc
rm -rf "$obj"
# no gl* function by name (see userspace/build.sh)
if nm=$(command -v llvm-nm || echo /usr/lib/llvm-18/bin/llvm-nm); "$nm" -u "$out" | grep -E ' _gl[A-Z]'; then
	echo "error: the shim refers to an OpenGL ES function by name" >&2
	exit 1
fi
echo "$out: $(stat -c %s "$out") bytes, sha256 $(sha256sum "$out" | cut -c1-16)"
