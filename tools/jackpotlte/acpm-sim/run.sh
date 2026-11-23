#!/bin/bash
# Host check of the ACPM clock driver (xnu patch 0192, bsd/dev/a8_acpm.c).
#
# Builds the real file for the PC against stand-in kernel headers and runs it
# against the ACPM SRAM words the phone printed over the USB console
# ("peek 0x02056b80 40" and friends, as "A8SH: 0020xxxxx: ..." lines in
# serial.log), with a toy APM core answering the IPC (harness.c).  It walks
# the framework's tables, the frequency/voltage map, the command queues in
# both directions, the plug-in load and the refusals.  No phone needed.
#
#   run.sh XNU_TREE SERIAL.LOG FVP.BIN
#
# XNU_TREE: a patched tree with bsd/dev/a8_acpm.c in it.
# FVP.BIN:  the 12288-byte plug-in (tools/jackpotlte/acpm-fvp-from-boot.py).
set -e
here=$(cd "$(dirname "$0")" && pwd)
xnu=${1:?XNU tree}
log=${2:?serial.log}
fvp=${3:?exynos7885_acpm_fvp.fw}
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

mkdir -p "$out/inc/pexpert/arm64" "$out/inc/sys" "$out/inc/kern" "$out/inc/machine"
cat > "$out/inc/a8host.h" <<'EOF'
#ifndef A8HOST_H
#define A8HOST_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
typedef uintptr_t vm_offset_t;
typedef uintptr_t vm_size_t;
typedef void *thread_t;
typedef int wait_result_t;
#define __unused __attribute__((unused))
#define KERN_SUCCESS 0
#define THREAD_UNINT 0
#define THREAD_CONTINUE_NULL ((void *)0)
#define NSEC_PER_USEC 1000u
#define NSEC_PER_MSEC 1000000u
#define NSEC_PER_SEC 1000000000u
#define Z_WAITOK 1
#define Z_ZERO 2
#define kalloc_data(size, flags) calloc(1, (size))
#define kfree_data(p, size) free(p)
#define LCK_GRP_DECLARE(var, name) int var __attribute__((unused))
#define LCK_MTX_DECLARE(var, grp) int var
#define lck_mtx_lock(m) ((void)(m))
#define lck_mtx_unlock(m) ((void)(m))
void delay(int usec);
uint64_t mach_absolute_time(void);
void clock_interval_to_deadline(uint32_t interval, uint32_t scale, uint64_t *deadline);
int PE_parse_boot_argn(const char *name, void *out, int size);
int kernel_thread_start(void (*fn)(void *, wait_result_t), void *arg, thread_t *thread);
void thread_deallocate(thread_t thread);
int assert_wait_deadline(void *chan, int interruptible, uint64_t deadline);
int thread_block(void *continuation);
#endif
EOF
echo '#define ARM64_BOARD_CONFIG_A5XELTE 1' > "$out/inc/pexpert/arm64/board_config.h"
for h in sys/param.h sys/systm.h sys/errno.h kern/clock.h kern/kalloc.h kern/locks.h \
    kern/thread.h kern/sched_prim.h machine/machine_routines.h pexpert/pexpert.h; do
	echo '#include "a8host.h"' > "$out/inc/$h"
done
# the host has no "dsb sy"
sed 's/__asm__ volatile ("dsb sy" ::: "memory");/;/' "$xnu/bsd/dev/a8_acpm.c" > "$out/a8_acpm_host.c"
gcc -std=gnu11 -O1 -g -Wall -Wextra -Wshadow \
    -DJACKPOTLTE=1 -I"$out/inc" -o "$out/harness" "$here/harness.c" "$out/a8_acpm_host.c"
"$out/harness" "$log" "$fvp"
