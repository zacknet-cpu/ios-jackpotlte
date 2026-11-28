#!/bin/bash
# SPDX-License-Identifier: MIT
#
# P13 phase 3 prep (build only, nothing is installed): JackpotlteMetal.bundle --
# Jesvi-iOS's RedwoodMetal driver (tools/metal-bundle/jesvi, MIT, NOTICE.md) with
# 0001-jackpotlte.patch -- and metal_props, for arm64 iOS 16.
#
#   tools/metal-bundle/mk-metal-bundle.sh [WORKDIR]
#
# Products in WORKDIR/out: JackpotlteMetal.bundle/ (and .tar of it), metal_props,
# build-record.txt. The driver needs no Mesa at build time: it dlopens
# /private/var/mobile/Library/a8air/libredwoodair.dylib (P13 phase 2's library) at run
# time. Metal's private base classes (_MTLDevice, _MTLResource, ...) are not in the
# SDK's Metal.tbd: they are left undefined with -U and bound by name when Metal loads
# the bundle (Metal itself is the image that defines them).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
work=${1:-${TMPDIR:-/tmp}/jackpotlte-metal}
[ "$(uname)" = Darwin ] || { echo "error: $0 needs macOS (the iPhoneOS SDK)" >&2; exit 1; }

sdk=$(xcrun --sdk iphoneos --show-sdk-path)
cc=$(xcrun --sdk iphoneos --find clang)
rm -rf "$work"
mkdir -p "$work/src" "$work/inc" "$work/out"
cp "$here"/jesvi/* "$here/m2m.h" "$work/src/"
patch --no-backup-if-mismatch -d "$work/src" -p1 < "$here/0001-jackpotlte.patch"

# IOKit's user headers: the iPhoneOS SDK has IOKit.tbd; when it has no IOKitLib.h,
# the macOS SDK's headers stand in (the same API; only the declarations are borrowed)
if [ ! -f "$sdk/System/Library/Frameworks/IOKit.framework/Headers/IOKitLib.h" ]; then
	mac=$(xcrun --sdk macosx --show-sdk-path)
	ln -s "$mac/System/Library/Frameworks/IOKit.framework/Headers" "$work/inc/IOKit"
	echo "IOKit headers: from the macOS SDK ($mac)"
else
	echo "IOKit headers: the iPhoneOS SDK's own"
fi

cflags=(-target arm64-apple-ios16.0 -isysroot "$sdk" -I"$work/inc" -I"$work/src"
	-O2 -g -fno-objc-arc -fblocks -Wall -Wno-unused-function -Wno-deprecated-declarations)

cd "$work"
for f in redwoodmetal redwooddrawable; do
	echo "--- $f.m"
	"$cc" "${cflags[@]}" -c "src/$f.m" -o "$f.o"
done

# Metal's private classes this driver subclasses: undefined on purpose (see the top)
uflags=()
for s in $(nm -u redwoodmetal.o redwooddrawable.o | awk '{print $NF}' |
	grep -E '^_OBJC_(METACLASS|CLASS)_\$__MTL' | sort -u); do
	uflags+=("-Wl,-U,$s")
done
echo "left for Metal to define at load: ${#uflags[*]} symbols"

bundle=out/JackpotlteMetal.bundle
mkdir -p "$bundle"
"$cc" -target arm64-apple-ios16.0 -isysroot "$sdk" -bundle -o "$bundle/JackpotlteMetal" \
	redwoodmetal.o redwooddrawable.o -framework Foundation -framework Metal \
	-framework IOKit -framework CoreFoundation -lobjc ${uflags[@]+"${uflags[@]}"}
cp "$here/Info.plist" "$bundle/Info.plist"
codesign -s - -f "$bundle"

echo "--- metal_props"
"$cc" -target arm64-apple-ios16.0 -isysroot "$sdk" -I"$work/inc" -O2 -Wall \
	src/metal_props.c -o out/metal_props -framework CoreFoundation -framework IOKit
codesign -s - -f out/metal_props

(cd out && tar cf JackpotlteMetal.bundle.tar JackpotlteMetal.bundle)

{
	echo "=== JackpotlteMetal build: from ${GITHUB_SHA:-local} (run ${GITHUB_RUN_ID:-local}) ==="
	for f in "$bundle/JackpotlteMetal" out/metal_props out/JackpotlteMetal.bundle.tar; do
		echo "$(shasum -a 256 "$f" | awk '{print $1}') $(wc -c < "$f" | tr -d ' ') ${f#out/}"
	done
	file "$bundle/JackpotlteMetal"
	echo "loads:"; otool -L "$bundle/JackpotlteMetal" | tail -n +2
	echo "classes:"; nm -gU "$bundle/JackpotlteMetal" | awk '$3 ~ /^_OBJC_CLASS_\$_/ {print "  " substr($3, 15)}'
	echo "left undefined for Metal:"; for u in ${uflags[@]+"${uflags[@]}"}; do echo "  ${u#-Wl,-U,}"; done
	echo "signature: $(codesign -dv "$bundle" 2>&1 | grep -E '^(Identifier|Signature|CodeDirectory)' | tr '\n' ' ')"
	echo "patch: $(shasum -a 256 "$here/0001-jackpotlte.patch" | awk '{print $1}')"
} | tee out/build-record.txt
