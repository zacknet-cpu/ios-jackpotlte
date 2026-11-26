#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the Galaxy A8 2018 (SM-A530F, jackpotlte, Exynos 7885) ADT.

Stage 1: an entry-only tree. Every value is sourced from the mainline 7885
device tree (MAINLINE) and is NOT yet confirmed against a live readback from
the phone. Anything not sourced is left out, not guessed.

Sources: arch/arm64/boot/dts/exynos/exynos7885.dtsi and
exynos7885-jackpotlte.dts (mainline). See shim/jackpotlte/board.h.

The identity stays the donor iPhone X (D22AP), as on dreamlte/A5XELTE: the A8
has no Home key, so the no-home-button UI matches.
"""
import argparse
import os
from pathlib import Path

import adt
import mkadt

# MAINLINE exynos7885-jackpotlte.dts memory@80000000, high bank.
DRAM_BASE = 0x880000000
# The whole high bank: 2 GiB on the 4 GB SM-A530W (Android /proc/iomem:
# 880000000-8ffffffff System RAM). Was 512 MiB, which left iOS paging
# ~1.3 GiB a minute from eMMC (a8log-6). The loader clamps the window to the
# bank S-Boot reports (1 GiB on 3 GB boards). DRAM_SIZE=0x20000000 in the
# environment gives the old window back.
DRAM_SIZE = int(os.environ.get('DRAM_SIZE', '0x80000000'), 0)
TIMEBASE = 26000000                      # Eureka/mainline oscclk 26 MHz
GICD, GICC = 0x12301000, 0x12302000      # Eureka gic@123001000
FB_PHYS = 0xec000000                     # Eureka fb_handover (exynos,dpu_fb_handover)
FB_W, FB_H = 1080, 2220                  # Eureka panel s6e3fa7 (1080x2220)
SOC_BASE = 0x01000000                    # /arm-io ranges base (unchanged from donor)


def set_prop(node, name, value):
    for i, (key, flags, _) in enumerate(node.props):
        if key == name:
            node.props[i] = (key, flags, value)
            return
    node.props.append((name, 0, value))


def child(node, name):
    for candidate in node.children:
        if candidate.prop('name') == mkadt.cstr(name):
            return candidate
    raise ValueError(f'missing donor ADT node {name}')


# MAINLINE exynos7885.dtsi cpus: 6x A53 (0x100-0x103, 0x200-0x201) then
# 2x A73 (0x0-0x1). Order here is the order XNU enumerates; MPIOR->physical.
CPUS = [
    (0, 0x100, 'a53'), (1, 0x101, 'a53'), (2, 0x102, 'a53'), (3, 0x103, 'a53'),
    (4, 0x200, 'a53'), (5, 0x201, 'a53'),
    (6, 0x000, 'a73'), (7, 0x001, 'a73'),
]


def cpu_node(logical, mpidr, kind):
    a53 = kind == 'a53'
    cluster = 0 if mpidr < 0x200 else 1      # A53 clusters 0/1
    if not a53:
        cluster = 2
    return [
        ('device_type', mkadt.cstr('cpu')),
        ('compatible', mkadt.cstr('arm,cortex-a53' if a53 else 'arm,cortex-a73') +
         mkadt.cstr('ARM,v8')),
        ('reg', mkadt.u32(mpidr)),
        ('cpu-id', mkadt.u32(logical)),
        # Every CPU advertises logical cluster 0 so XNU builds a single pset
        # (patch 0137): a logical cluster per physical AFF1 gives three psets
        # on this non-__AMP__ build and the boot CPU deadlocks on a half-owned
        # ticket lock. die-cluster-id / l2-cache-id keep the hardware grouping
        # for the cluster power-up logging in 0136.
        ('cluster-id', mkadt.u32(0)),
        ('cluster-type', mkadt.u32(0)),
        ('die-cluster-id', mkadt.u32(cluster)),
        ('cluster-core-id', mkadt.u32(mpidr & 0xff)),
        ('l2-cache-id', mkadt.u32(cluster)),
        ('l3-cache-id', mkadt.u32(0)),
        ('state', mkadt.cstr('running' if logical == 0 else 'stopped')),
        ('timebase-frequency', mkadt.u32(TIMEBASE)),
        # TODO(SAM-K): Exynos7885 A53/A73 clock rates; placeholder only.
        ('clock-frequency', mkadt.u32(1600000000)),
        ('bus-frequency', mkadt.u32(100000000)),
        ('memory-frequency', mkadt.u32(TIMEBASE)),
        ('peripheral-frequency', mkadt.u32(TIMEBASE)),
        ('fixed-frequency', mkadt.u32(TIMEBASE)),
    ]


def build():
    root = mkadt.build(seed=os.urandom(64), qemu=True)
    # The kernel's SweetPlatformExpert personality matches 'samsung,a5xelte'
    # (and, since 0124, 'samsung,dreamlte') in iokit/KernelConfigTables.cpp.
    # No personality matches 'samsung,jackpotlte', and IODTPlatformExpert
    # matches against *every* entry of this list, so it must contain a name
    # the kernel accepts -- otherwise no IOPlatformExpert matches, the
    # IOPanicPlatform fallback runs and the boot dies at
    # "Unable to find driver for this platform". The samsung entries stay for
    # that matching (every entry is tried).
    #
    # This append was previously an uncommitted local edit: the jt10/jt13
    # images carried it, jt14 did not, and jt14 panicked at IOPlatformExpert.
    # Commit it so it cannot be lost again.
    #
    # D22AP goes *first* (patch 0169): IODTPlatformExpert::getModelName
    # returns compatible[0], and on arm64 getTargetName == getModelName, so
    # compatible[0] is what sysctl hw.target and MobileGestalt HWModelStr read
    # (NOTES.md 32). The donor ADT has the same order (tools/mkadt.py). With
    # jackpotlte first, HWModelStr read "samsung,jackpotlte"; the Samsung name
    # only has to be somewhere in the list for the platform to bind.
    set_prop(root, 'compatible', mkadt.cstr('D22AP') +
             mkadt.cstr('samsung,jackpotlte') +
             mkadt.cstr('samsung,exynos7885') + mkadt.cstr('AppleARM') +
             mkadt.cstr('samsung,a5xelte'))
    chosen = child(root, 'chosen')
    set_prop(chosen, 'dram-base', mkadt.u64(DRAM_BASE))
    set_prop(chosen, 'dram-size', mkadt.u64(DRAM_SIZE))
    set_prop(chosen, 'firmware-version', mkadt.cstr('jackpotlte-entry'))
    set_prop(chosen, 'system-firmware-version', mkadt.cstr('jackpotlte-entry'))
    cpus = child(root, 'cpus')
    cpu0 = child(cpus, 'cpu0')
    for key, value in cpu_node(*CPUS[0]):
        if key != 'cpu-id':
            set_prop(cpu0, key, value)
    for logical, mpidr, kind in CPUS[1:]:
        cpus.children.append(mkadt.node(f'cpu{logical}', cpu_node(logical, mpidr, kind)))
    arm_io = child(root, 'arm-io')
    set_prop(arm_io, 'device_type', mkadt.cstr('jackpotlte-io'))
    set_prop(arm_io, 'compatible', mkadt.cstr('samsung,exynos7885-io') +
             mkadt.cstr('arm-io'))
    set_prop(arm_io, 'ranges', mkadt.u64(0, SOC_BASE, 0x10000000 - SOC_BASE))
    arm_io.children = [n for n in arm_io.children if n.prop('name') == mkadt.cstr('wdt')]
    arm_io.children.append(mkadt.node('gic', [
        ('compatible', mkadt.cstr('arm,cortex-a15-gic')),
        ('interrupt-controller', mkadt.cstr('master')),
        # MAINLINE timer: non-secure physical timer PPI 14 = INTID 30. The
        # kernel uses CNTP when A5_USE_PHYS_TIMER is set for the board.
        ('virtual-timer-intid', mkadt.u32(30)),
        ('reg', mkadt.u64(GICD - SOC_BASE, 0x1000, GICC - SOC_BASE, 0x1000)),
    ]))
    # Display service. The A5XELTE build's AppleCLCD personality matches
    # 'samsung,a5xelte-display' (xnu/patches/0109 + 0124 in
    # KernelConfigTables.cpp), so the A8 publishes that same compatible and
    # AppleCLCD binds to the S-Boot handover framebuffer at 0xec000000. The
    # panel is the s6e3fa7 at 1080x2220 (Eureka exynos7885-display-lcd.dtsi:
    # 173-193); IOMobileFramebuffer reads canvas-width/height from this node,
    # so the canvas is the panel.
    arm_io.children.append(mkadt.node('disp0', [
        ('device_type', mkadt.cstr('display-subsystem')),
        ('compatible', mkadt.cstr('samsung,a5xelte-display')),
        ('canvas-width', mkadt.u32(FB_W)),
        ('canvas-height', mkadt.u32(FB_H)),
    ]))
    # The framebuffer is confirmed live: after the DECON trigger the loader's
    # banner appears at 0xec000000 (phone screen, 2026-11-08). The A5XELTE
    # serial driver looks for "a5xelte-fbcon" with reg/width/height/stride.
    root.children = [n for n in root.children if n.prop('name') !=
                     mkadt.cstr('sweet-ufs')]
    root.children.append(mkadt.node('a5xelte-fbcon', [
        ('reg', mkadt.u64(FB_PHYS, 0x01400000)),
        ('width', mkadt.u32(FB_W)),
        ('height', mkadt.u32(FB_H)),
        ('stride', mkadt.u32(FB_W * 4)),
    ]))
    # eMMC: Samsung exynos7885.dtsi dwmmc0@13500000 (the embedded device,
    # clock MMC_EMBD; dwmmc1 0x13510000 is SDIO, dwmmc2 0x13550000 is the SD
    # card). bsd/dev/a5xelte_emmc.c maps this window and reads the GPT on
    # emmcprobe=1 / rd=emmcroot0. Absolute physical address, as there.
    root.children = [n for n in root.children if n.prop('name') !=
                     mkadt.cstr('a5xelte-emmc')]
    root.children.append(mkadt.node('a5xelte-emmc', [
        ('reg', mkadt.u64(0x13500000, 0x2000)),
    ]))
    # Dynamic Island (2026-11-24): libMobileGestalt reads ArtworkTraits from
    # IODeviceTree:/product; with A8_ARTWORK_SUBTYPE=2556 (iPhone 14 Pro)
    # SpringBoard shows the system aperture. The node is the one the 0198-island
    # slot booted with (artwork-device-idiom iphone, subtype, scale 3, dynamic
    # displaymode 0, phandle 999).
    subtype = os.environ.get('A8_ARTWORK_SUBTYPE')
    if subtype:
        root.children = [n for n in root.children if n.prop('name') != mkadt.cstr('product')]
        props = [
            ('AAPL,phandle', mkadt.u32(999)),
            ('artwork-device-idiom', mkadt.cstr('iphone')),
            ('artwork-device-subtype', mkadt.u32(int(subtype, 0))),
            ('artwork-scale-factor', mkadt.u32(int(os.environ.get('A8_ARTWORK_SCALE', '3'), 0))),
            ('artwork-dynamic-displaymode', mkadt.u32(0)),
        ]
        # P17 (2026-11-26): ArtworkTraits also wants the display gamut and the
        # graphics feature set (CoreUI logs a fault in every app without
        # them). Key names are the ones libMobileGestalt reads from
        # IODeviceTree:/product (strings of the phone's own 16.7 copy); the
        # feature set strings are its built-in A11 pair, matching the D22AP
        # identity above. sRGB, not P3, as the S7 port (wardevsa/ios-a5xelte
        # mkadt_herolte.py): P3 makes CoreAnimation ask for 10-bit surfaces
        # the GL path does not have. Off by default since 11-26 23:20: the try
        # boot (0228c-p17traits) removed CoreUI's faults and drew apps and
        # icons right, but Control Center ran ~1.5-2% slower (483-490 vs
        # 490-495, A/B in one session); A8_PRODUCT_TRAITS=1 adds them.
        if os.environ.get('A8_PRODUCT_TRAITS', '0') == '1':
            props += [
                ('artwork-display-gamut', mkadt.cstr('sRGB')),
                ('graphics-featureset-class', mkadt.cstr('APPLE4')),
                ('graphics-featureset-fallbacks',
                 mkadt.cstr('APPLE3:APPLE3v1:APPLE2:APPLE1:GLES2,0')),
            ]
        root.children.append(mkadt.node('product', props))
    return root


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    blob = adt.serialize(build())
    args.output.write_bytes(blob)
    print(f'{args.output}: {len(blob)} bytes')
