#!/bin/bash
# Build bsd/dev/a5xelte_emmc.c from xnu/patches, swap its MMIO accessors for
# the dw_mmc + eMMC model in sim.c, and run the one-sector, multi-block,
# self-test and fault-injection cases. Needs gcc and zlib (Linux or macOS).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
git -C "$work" init -q
for p in $(grep -l "a5xelte_emmc.c" "$repo"/xnu/patches/*.patch | sort); do
	git -C "$work" apply --include='bsd/dev/a5xelte_emmc.c' "$p"
done
python3 - "$work/bsd/dev/a5xelte_emmc.c" "$work/driver.c" <<'PY'
import sys
s = open(sys.argv[1]).read()
rd = "\treturn *(volatile uint32_t *)(emmc_regs + off);"
wr = "\t*(volatile uint32_t *)(emmc_regs + off) = value;"
assert rd in s and wr in s
s = s.replace(rd, "\treturn sim_rd(off);").replace(wr, "\tsim_wr(off, value);")
open(sys.argv[2], "w").write(s)
PY
mkdir -p "$work/hdr"
for h in sys/param.h sys/systm.h sys/buf.h sys/conf.h sys/disk.h sys/errno.h \
    sys/proc.h sys/uio.h miscfs/devfs/devfs.h kern/locks.h kern/sched_prim.h \
    pexpert/pexpert.h pexpert/device_tree.h libkern/libkern.h machine/atomic.h \
    kern/clock.h kern/cpu_data.h; do
	mkdir -p "$work/hdr/$(dirname "$h")"
	: > "$work/hdr/$h"
done
cp "$here/sim.c" "$here/sim_kernel.h" "$work/"
gcc -O1 -g -w -I"$work/hdr" -I"$work" -include "$work/sim_kernel.h" \
    "$work/sim.c" -o "$work/sim" -lz
"$work/sim"
