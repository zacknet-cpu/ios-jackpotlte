#!/bin/bash
# SPDX-License-Identifier: MIT
#
# P13 route A, phase 1 (build only): Jesvi-iOS's AIR (Metal shader) frontend
# built into our Mesa 25.0.7 panfrost v6 tree, for arm64 iOS.
#
#   tools/mesa-air/mk-mesa-air.sh [--with-osmesa] [WORKDIR]
#
# --with-osmesa also builds libOSMesa.8.dylib (Mesa's default targets) into
# out/ (used 11-28 while the CI login had no `workflow` scope; the branch's
# mesa.yml now calls this script directly, without it). build.log ends with
# every product's size and sha256.
#
# Same tarball, same drm face and same 0001-0007 series as
# tools/mesa-jackpotlte/mk-mesa-jackpotlte.sh; then tools/mesa-air/air is
# copied to src/compiler/air and tools/mesa-air/01xx-*.patch applied (the one
# meson line that reaches the frontend, and the panfrost-side targets).
#
# Products, in WORKDIR/out (test binaries, nothing here is installed anywhere):
#   libredwoodair.dylib  air_metal.h's contract: a Metal function's bitcode ->
#                        NIR -> a Gallium compute state on panfrost v6
#   air_run              AIR -> NIR -> drawn/dispatched, read back (phase 2)
#   air_metal_test       libredwoodair driven through its own header
#   air2nir              the frontend alone (prints NIR, --report)
#   built-from, meson-setup.log, build.log
#
# Debug-optimized with asserts on: these are for measuring correctness on the
# phone (phase 2), so nir_validate and Mesa's asserts stay in.
set -euo pipefail

with_osmesa=0
if [ "${1:-}" = --with-osmesa ]; then with_osmesa=1; shift; fi
repo=$(cd "$(dirname "$0")/../.." && pwd)
work=${1:-${TMPDIR:-/tmp}/jackpotlte-mesa-air}
version=25.0.7
tarball=mesa-$version.tar.xz
sha256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
patchset=$repo/tools/mesa-jackpotlte
airset=$repo/tools/mesa-air
shim=$repo/userspace/libjackpotlte

[ "$(uname)" = Darwin ] || { echo "error: $0 needs macOS (the iPhoneOS SDK and Mach-O)" >&2; exit 1; }

sdk=$(xcrun --sdk iphoneos --show-sdk-path)
cc=$(xcrun --sdk iphoneos --find clang)
cxx=$(xcrun --sdk iphoneos --find clang++)

mkdir -p "$work"
cd "$work"

if [ ! -f "$tarball" ]; then
	echo "--- fetching Mesa $version ---"
	curl -sSL --fail -o "$tarball" "https://archive.mesa3d.org/$tarball"
fi
echo "$sha256  $tarball" | shasum -a 256 -c --quiet

# ---- the tree: our series, then the frontend and its patches ----------------
stamp=$(cat "$patchset"/*.patch "$airset"/*.patch "$airset"/air/* | shasum -a 256 | awk '{print $1}')
src=mesa-$version
if [ "$(cat "$src/.jackpotlte-air" 2>/dev/null || true)" != "$stamp" ]; then
	echo "--- extracting and patching Mesa $version (+ AIR) ---"
	rm -rf "$src" build
	tar xJf "$tarball"
	for p in "$patchset"/*.patch; do
		patch -s --no-backup-if-mismatch -d "$src" -p1 < "$p"
	done
	rm -rf "$src/src/compiler/air"
	cp -R "$airset/air" "$src/src/compiler/air"
	for p in "$airset"/01*.patch; do
		echo "applying $(basename "$p")"
		patch --no-backup-if-mismatch -d "$src" -p1 < "$p"
	done
	echo "$stamp" > "$src/.jackpotlte-air"
fi

# ---- the drm face, always the current one (as mk-mesa-jackpotlte.sh) --------
rm -rf "$src/src/jackpotlte-drm"
mkdir -p "$src/src/jackpotlte-drm"
for f in jackpotlte_drm.c jackpotlte_drm.h jackpotlte_gpu.c jackpotlte_gpu.h \
	jackpotlte_iokit.c; do
	cp "$shim/$f" "$src/src/jackpotlte-drm/$f"
done
cp "$src/src/jackpotlte-drm/jackpotlte_drm.h" "$src/src/jackpotlte-drm/xf86drm.h"

if [ ! -x "$work/venv/bin/meson" ]; then
	echo "--- creating the Python environment (meson, ninja, mako) ---"
	python3 -m venv "$work/venv"
	"$work/venv/bin/pip" -q install meson ninja mako pyyaml packaging
fi
export PATH=$work/venv/bin:$PATH

ar=llvm-ar ranlib=llvm-ranlib strip=llvm-strip
if ! command -v llvm-ar >/dev/null; then
	ar=ar ranlib=ranlib strip=strip
fi

cat > cross.ini <<INI
[binaries]
c = ['$cc', '-target', 'arm64-apple-ios16.0', '-isysroot', '$sdk']
cpp = ['$cxx', '-target', 'arm64-apple-ios16.0', '-isysroot', '$sdk', '-fuse-cxa-atexit']
objc = ['$cc', '-target', 'arm64-apple-ios16.0', '-isysroot', '$sdk']
ar = '$ar'
ranlib = '$ranlib'
strip = '$strip'
pkg-config = 'false'

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'

[properties]
needs_exe_wrapper = true
INI

build=build
if [ ! -f "$build/build.ninja" ]; then
	echo "--- meson setup ---"
	meson setup "$build" "$src" --cross-file cross.ini --wrap-mode=nofallback \
		--prefix=/jackpotlte --libdir=lib --buildtype=debugoptimized \
		-Db_ndebug=false \
		-Dplatforms= -Dgallium-drivers=softpipe,panfrost -Dvulkan-drivers= \
		-Dglx=disabled -Degl=disabled -Dgbm=disabled -Dosmesa=true \
		-Dllvm=disabled -Dshared-glapi=enabled -Dgles1=disabled -Dgles2=enabled \
		-Dopengl=true -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled \
		-Dshader-cache=disabled -Dvalgrind=disabled -Dlibunwind=disabled \
		-Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-vdpau=disabled \
		-Dgallium-va=disabled -Dgallium-xa=disabled -Dlmsensors=disabled \
		-Dbuild-tests=false > meson-setup.log 2>&1 || {
		tail -n 60 meson-setup.log; exit 1; }
fi

targets="src/compiler/air/air2nir
src/gallium/drivers/panfrost/libredwoodair.dylib
src/gallium/drivers/panfrost/air_run
src/gallium/drivers/panfrost/air_metal_test"

echo "--- ninja: the AIR targets (this takes a while) ---"
# -k 0: keep going, so one run shows every file that does not compile
all=$targets
[ $with_osmesa = 1 ] && all="$all
src/gallium/targets/osmesa/libOSMesa.8.dylib"
if ! ninja -C "$build" -k 0 $all > build.log 2>&1; then
	grep -E 'error:|FAILED:' build.log | head -n 80 || true
	tail -n 60 build.log
	exit 1
fi

mkdir -p out
for t in $all; do
	cp "$build/$t" out/
done

# ---- the phone layout (P13 phase 2) ----------------------------------------
# The tests go to /private/var/mobile/Library/a8air/ (the fput area). dyld on
# this port has no AMFI policy outside the jtexecinject list (flags 0: no
# @-paths, no DYLD_* variables), so air_metal_test names the library by its
# absolute phone path; the library keeps its own install name. Every product is
# ad-hoc signed (arm64 iOS code needs a signature; no entitlements: the Mali
# user client checks none and this port has no sandbox kext).
if [ -f out/air_metal_test ]; then
	install_name_tool -change /jackpotlte/lib/libredwoodair.dylib \
		/private/var/mobile/Library/a8air/libredwoodair.dylib out/air_metal_test
fi
for f in out/*; do
	case "$(basename "$f")" in built-from) continue ;; esac
	codesign -s - -f "$f"
done
cat "$patchset"/*.patch "$airset"/*.patch "$airset"/air/* "$shim"/*.c "$shim"/*.h |
	shasum -a 256 | awk '{print $1}' > out/built-from

echo
for f in out/*; do
	[ "$(basename "$f")" = built-from ] && continue
	echo "built: $f ($(wc -c < "$f" | tr -d ' ') bytes)"
	file "$f"
done
echo "libredwoodair exports: $(nm -gU out/libredwoodair.dylib | awk '$3 ~ /^_rwa_/' | wc -l | tr -d ' ') rwa_* functions"

# the record of what was built, at the end of build.log (which the a8-phase2
# mesa.yml uploads): commit, products, sizes, hashes, exports
{
	echo
	echo "=== P13 AIR build: built from ${GITHUB_SHA:-local} (run ${GITHUB_RUN_ID:-local}) ==="
	for f in out/*; do
		[ "$(basename "$f")" = built-from ] && continue
		echo "$(shasum -a 256 "$f" | awk '{print $1}') $(wc -c < "$f" | tr -d ' ') $(basename "$f")"
		lipo -info "$f" 2>/dev/null || true
	done
	echo "install name: $(otool -D out/libredwoodair.dylib | tail -n 1)"
	echo "air_metal_test loads: $(otool -L out/air_metal_test | grep redwoodair | awk '{print $1}')"
	for f in out/*; do
		case "$(basename "$f")" in built-from) continue ;; esac
		echo "signature $(basename "$f"): $(codesign -dv "$f" 2>&1 | grep -E '^Signature|^CodeDirectory' | tr '\n' ' ')"
	done
	echo "rwa_* exports:"
	nm -gU out/libredwoodair.dylib | awk '$3 ~ /^_rwa_/ {print "  " substr($3, 2)}'
	echo "undefined in libredwoodair: $(nm -u out/libredwoodair.dylib | wc -l | tr -d ' ')"
	echo "built-from: $(cat out/built-from)"
} | tee -a build.log
