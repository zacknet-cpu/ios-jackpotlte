/* SPDX-License-Identifier: GPL-2.0-only
 * Galaxy A8 2018 (SM-A530F, jackpotlte, Exynos 7885) XNU loader.
 *
 * Adapted from shim/dreamlte/loader.c (Galaxy S8, Exynos 8895). Same S-Boot
 * handoff and same tools/mkpayload.py "SWEETXNU" v1 container, but:
 *   - the 8895 hardware probes (UFS, PMIC, touch, display, battery, Mongoose)
 *     are removed: they touch Exynos 8895 MMIO and are not jackpotlte facts;
 *   - the screen console and the pstore log are compiled out until the A8's
 *     framebuffer base and ramoops zones are read from the live DT / Samsung
 *     source (see shim/jackpotlte/board.h). With the MMU off, writing to a
 *     guessed address is a silent hang, so we refuse to do it.
 *
 * The loader's real job is unchanged: copy the SWEETXNU payload from the BOOT
 * ramdisk into the XNU RAM window named by the ADT, build boot_args and jump to
 * XNU's physical entry with the MMU and caches off.
 */
#include <stdint.h>
#include "board.h"
#include "../fdt.h"
#include "inflate.h"
#if JT_HAVE_FB
#include "../font.h"
#include "logo.h"
#endif

#define PAGE_SIZE		4096UL
#define BOOT_LINE_LENGTH	608

/* ---- board aliases (single place to change once facts arrive) ---- */
#define LOW_RAM_BASE		JT_LOW_RAM_BASE
#define LOW_RAM_END		JT_LOW_RAM_END
#define XNU_RAM_BASE		JT_XNU_RAM_BASE
#define XNU_RAM_END		JT_XNU_RAM_END

/* ---- payload (tools/mkpayload.py v1) ---- */
struct payload_seg {
	uint64_t vmaddr, vmsize, file_off, file_size;
};
struct payload {
	char magic[8];
	uint32_t version, nsegs;
	uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
	uint64_t phys_base, mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};
struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};
struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};
_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args size");

uint64_t jt_boot_args;	/* read by start.S */

/* ---- console -----------------------------------------------------------
 * Two independent sinks, each gated by a board fact. If neither is known the
 * messages are dropped (the loader still runs; we just cannot show them yet).
 */
#if JT_HAVE_RAMOOPS
struct prz {
	uint32_t sig, start, size;
	uint8_t data[];
};
#define PRZ_SIG			0x43474244U	/* "DBGC" */
#define PRZ_DATA_MAX		(JT_RAMOOPS_SIZE - 12)
static volatile struct prz *const prz = (volatile struct prz *)JT_RAMOOPS_CONSOLE;
static volatile struct prz *const prz2 = (volatile struct prz *)JT_RAMOOPS_PMSG;
static uint32_t log_len, log2_len;

static void prz_put(volatile struct prz *z, uint32_t *len, char c)
{
	if (*len >= PRZ_DATA_MAX)
		return;
	z->data[(*len)++] = (uint8_t)c;
	z->start = *len;
	z->size = *len;
}
#endif

static uint32_t fg = 0xffffffffU;

#if JT_HAVE_FB
static volatile uint32_t *const fb = (volatile uint32_t *)JT_FB_BASE;
#define SCALE	3
#define CELL_W	(8 * SCALE)
#define CELL_H	(16 * SCALE + 6)
#define COLS	((JT_FB_WIDTH - 48) / CELL_W)
/* Rows start at scr_top. The default is the top of the screen; with the boot
 * logo enabled scr_top moves below the logo so text never overwrites it. */
#define SCR_TOP_DEFAULT	120
#define SCR_ROWS(top)	((JT_FB_HEIGHT - (top)) / CELL_H)
static unsigned int row, col;
static unsigned int scr_top = SCR_TOP_DEFAULT;
static unsigned int scr_rows = SCR_ROWS(SCR_TOP_DEFAULT);

static void fill(int x0, int y0, int w, int h, uint32_t c)
{
	for (int y = y0; y < y0 + h && y < (int)JT_FB_HEIGHT; y++)
		for (int x = x0; x < x0 + w && x < (int)JT_FB_WIDTH; x++)
			fb[y * (JT_FB_STRIDE / 4) + x] = c;
}

static void scr_ch(char c)
{
	if (c == '\n' || col >= COLS) {
		col = 0;
		row = (row + 1) % scr_rows;
		fill(0, (int)scr_top + (int)row * CELL_H, JT_FB_WIDTH, CELL_H,
		     0xff000000U);
		if (c == '\n')
			return;
	}
	if (c < 0x20 || c > 0x7e)
		c = '?';
	const unsigned char *g = font8x16[c - 0x20];
	int x0 = 24 + (int)col * CELL_W, y0 = (int)scr_top + (int)row * CELL_H;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++)
			fill(x0 + gx * SCALE, y0 + gy * SCALE, SCALE, SCALE,
			     (g[gy] & (0x80 >> gx)) ? fg : 0xff000000U);
	col++;
}

/* The iOS userspace boot logo, 1 bpp white on black, drawn 1:1 (the bitmap is
 * already at panel size, so there is no scaling and no float anywhere). */
static void blit_logo(int x0, int y0)
{
	for (unsigned int y = 0; y < LOGO_H; y++) {
		int py = y0 + (int)y;
		if (py < 0 || py >= (int)JT_FB_HEIGHT)
			continue;
		for (unsigned int x = 0; x < LOGO_W; x++) {
			int px = x0 + (int)x;
			if (px < 0 || px >= (int)JT_FB_WIDTH)
				continue;
			if (logo_bits[y * LOGO_STRIDE + (x >> 3)] & (0x80U >> (x & 7)))
				fb[py * (JT_FB_STRIDE / 4) + px] = 0xffffffffU;
		}
	}
}
#endif

static void log_ch(char c)
{
#if JT_HAVE_RAMOOPS
	prz_put(prz, &log_len, c);
	prz_put(prz2, &log2_len, c);
#endif
#if JT_HAVE_FB
	scr_ch(c);
#else
	(void)c;
#endif
}

static void out(const char *s)
{
	while (*s)
		log_ch(*s++);
}

static void hex(uint64_t v)
{
	static const char digits[] = "0123456789abcdef";
	char b[19] = "0x";
	int n = 2, started = 0;
	for (int i = 60; i >= 0; i -= 4) {
		int d = (v >> i) & 0xf;
		if (d || started || i == 0) {
			b[n++] = digits[d];
			started = 1;
		}
	}
	b[n] = 0;
	out(b);
}

static void kv(const char *k, uint64_t v)
{
	out(k);
	out("=");
	hex(v);
	out(" ");
}

/* decimal, for sizes the reader can compare at a glance */
static void dec(uint64_t v)
{
	char b[24];
	int n = 0;
	if (v == 0) {
		b[n++] = '0';
	} else {
		while (v && n < 23) {
			b[n++] = (char)('0' + v % 10);
			v /= 10;
		}
	}
	while (n)
		log_ch(b[--n]);
}

/* Wait about ms milliseconds. S-Boot has been seen to hand over with
 * CNTFRQ_EL0 = 0 (dreamlte probe), so fall back to a fixed instruction count:
 * the canary must not depend on a timer that may never have been programmed. */
static void delay_ms(uint32_t ms)
{
	uint64_t frq;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
	if (frq) {
		uint64_t start, now, want = (frq / 1000U) * ms;
		__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(start));
		do {
			__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(now));
		} while (now - start < want);
		return;
	}
	for (volatile uint64_t i = 0; i < 30000000ULL; i++)
		;
}

/* Reset the SoC with the "recovery" reboot reason (exynos-reboot.c: write the
 * reason to PMU SYSIP_DAT0, then 1 to PMU SWRESET). The screen may show
 * nothing, so a phone that ends up in recovery by itself is the proof that the
 * shim ran. Returns only if the PMU write is ignored. */
static void canary(void)
{
#if JT_CANARY_MS > 0
	delay_ms(JT_CANARY_MS);
	out("reboot -> recovery (canary)\n");
	*(volatile uint32_t *)(JT_PMU_BASE + JT_PMU_SYSIP_DAT0) = JT_REBOOT_RECOVERY;
	__asm__ volatile("dsb sy" ::: "memory");
	*(volatile uint32_t *)(JT_PMU_BASE + JT_PMU_SWRESET) = 1;
	__asm__ volatile("dsb sy" ::: "memory");
#endif
}

/* MMU off: everything is Device memory, keep accesses naturally aligned. */
static void copy(uintptr_t dst, uintptr_t src, uint64_t n)
{
	if (!((dst | src) & 7)) {
		for (; n >= 8; n -= 8, dst += 8, src += 8)
			*(volatile uint64_t *)dst = *(volatile uint64_t *)src;
	}
	for (; n; n--, dst++, src++)
		*(volatile uint8_t *)dst = *(volatile uint8_t *)src;
}

static void zero(uintptr_t dst, uint64_t n)
{
	for (; n && (dst & 7); n--, dst++)
		*(volatile uint8_t *)dst = 0;
	for (; n >= 8 && (dst & 63); n -= 8, dst += 8)
		*(volatile uint64_t *)dst = 0;
	/* 64 bytes a loop in four aligned STPs: the window is now up to 2 GiB
	 * and this runs with the MMU off (Device memory), one store at a time. */
	for (; n >= 64; n -= 64, dst += 64)
		__asm__ volatile(
			"stp xzr, xzr, [%0]\n"
			"stp xzr, xzr, [%0, #16]\n"
			"stp xzr, xzr, [%0, #32]\n"
			"stp xzr, xzr, [%0, #48]\n"
			: : "r"(dst) : "memory");
	for (; n >= 8; n -= 8, dst += 8)
		*(volatile uint64_t *)dst = 0;
	for (; n; n--, dst++)
		*(volatile uint8_t *)dst = 0;
}

static void clean(uintptr_t start, uintptr_t end)
{
	uint64_t ctr;
	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
	uintptr_t line = 4UL << ((ctr >> 16) & 0xf);
	for (uintptr_t p = start & ~(line - 1); p < end; p += line)
		__asm__ volatile("dc civac, %0" : : "r"(p) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static int magic_ok(const volatile char *m)
{
	const char *want = "SWEETXNU";
	for (int i = 0; i < 8; i++)
		if (m[i] != want[i])
			return 0;
	return 1;
}

#if JT_HAVE_FB
/* Does a space-separated cmdline token match exactly? Bounded by n (the
 * payload cmdline has no guaranteed NUL); absent or malformed -> no. */
static int cmdline_has(const volatile char *cl, uint64_t n, const char *tok)
{
	uint64_t tlen = 0;
	while (tok[tlen])
		tlen++;
	for (uint64_t i = 0; i + tlen <= n; i++) {
		if (i && cl[i - 1] != ' ')
			continue;
		uint64_t k = 0;
		while (k < tlen && cl[i + k] == tok[k])
			k++;
		if (k != tlen)
			continue;
		if (i + tlen == n || cl[i + tlen] == ' ' || cl[i + tlen] == 0)
			return 1;
	}
	return 0;
}

/* The payload's own cmdline -- the same bytes copied into boot_args -- asks
 * for the panel logo. A missing/placeholder payload has no cmdline: off. */
static int payload_logo(uintptr_t start, uintptr_t end)
{
	if (end - start < sizeof(struct payload))
		return 0;
	const volatile struct payload *p = (const volatile struct payload *)start;
	if (!magic_ok(p->magic) || p->version != 1)
		return 0;
	return cmdline_has(p->cmdline, sizeof(p->cmdline), "jtlogo=1");
}
#endif

static uint64_t fail(const char *why)
{
	fg = 0xffff4040U;
	out("FAIL: ");
	out(why);
	out("\n");
	canary();
	return 0;
}

/* ---- Phase 2: two kernel slots (tools/jackpotlte/mkslots.py) ----
 * The ramdisk may hold a "SWEETSLT" container instead of one SWEETXNU
 * payload: two raw-deflated v1 payloads, each described by a record in its
 * own 512-byte sector with its own CRC. The default slot is the good one with
 * the highest generation. The running kernel can leave a "try slot N once"
 * request at JT_SLOT_FLAG_PA; it is wiped here before that slot is even read,
 * so a test kernel that hangs, panics or is force-restarted comes back on the
 * default. Anything wrong with a slot (record CRC, size, data CRC, decode)
 * also falls back. What was booted, and why, is left at JT_SLOT_BOOT_PA. */
#define SLOT_MAGIC0	0x45455753U	/* "SWEE" */
#define SLOT_MAGIC1	0x544c5354U	/* "TSLT" */
#define SLOT_VERSION	2U
#define SLOT_HDR	4096U
#define SLOT_REC_OFF(i)	(512U * (1U + (uint32_t)(i)))
#define SLOT_SCRATCH	0xa0000000UL	/* low bank, decoded payload */
#define SLOT_MAX_PAYLOAD (64UL << 20)
#define TRY_MAGIC	0x52543841U	/* "A8TR" */
#define BOOT_MAGIC	0x54423841U	/* "A8BT" */

/* Boot record reasons, also read by the kernel (bsd/dev/a8_slots.c). */
#define WHY_DEFAULT	0U	/* no request: the default slot */
#define WHY_TRY		1U	/* the requested slot, once */
#define WHY_TRY_FAILED	2U	/* request refused, default instead */
#define WHY_OTHER_GOOD	3U	/* default refused, the other good slot */

static uint32_t rd32(uintptr_t a)
{
	return *(volatile uint32_t *)a;
}

static uint64_t rd64(uintptr_t a)
{
	return (uint64_t)rd32(a) | ((uint64_t)rd32(a + 4) << 32);
}

/* CRC-32 (zlib) over RAM. The MMU is off, so every load is an uncached
 * Device access: read whole aligned words and fold them bytewise. */
static uint32_t crc32_dev(uintptr_t p, uint64_t n)
{
	uint32_t c = 0xffffffffU;

	for (; n >= 4 && !(p & 3); n -= 4, p += 4) {
		c ^= *(volatile uint32_t *)p;
		for (int k = 0; k < 32; k++)
			c = (c >> 1) ^ (0xedb88320U & (0U - (c & 1U)));
	}
	for (; n; n--, p++) {
		c ^= *(volatile uint8_t *)p;
		for (int k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xedb88320U & (0U - (c & 1U)));
	}
	return c ^ 0xffffffffU;
}

static uint64_t ticks(void)
{
	uint64_t t;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t));
	return t;
}

/* Milliseconds since an earlier ticks() value; 0 if CNTFRQ was never set. */
static uint32_t ms_since(uint64_t t0)
{
	uint64_t frq;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
	return frq ? (uint32_t)((ticks() - t0) / (frq / 1000U)) : 0;
}

/* A slot record: present (CRC ok, not empty), good, generation. */
static int slot_ok(uintptr_t rd, int i)
{
	uintptr_t r = rd + SLOT_REC_OFF(i);
	return crc32_dev(r, 124) == rd32(r + 124) && rd64(r + 16) != 0;
}

static int slot_good(uintptr_t rd, int i)
{
	return slot_ok(rd, i) && rd32(rd + SLOT_REC_OFF(i) + 112) == 1;
}

static uint32_t slot_gen(uintptr_t rd, int i)
{
	return rd32(rd + SLOT_REC_OFF(i) + 44);
}

/* One slot: check it, decode it into SLOT_SCRATCH. 0 = ok. */
static const char *slot_decode(uintptr_t rd, uint64_t len, int i,
    uintptr_t dtb, uint64_t *plen)
{
	uintptr_t r = rd + SLOT_REC_OFF(i);
	uint64_t off = rd64(r), cap = rd64(r + 8), csize = rd64(r + 16);
	uint64_t usize = rd64(r + 24);
	uint32_t codec = rd32(r + 32), crc_c = rd32(r + 36), crc_u = rd32(r + 40);
	uint64_t dtb_end = dtb + __builtin_bswap32(rd32(dtb + 4));
	extern char _head[], _end[];

	if (crc32_dev(r, 124) != rd32(r + 124))
		return "record CRC differs";
	if (!csize)
		return "empty";
	if (codec != 1)
		return "unknown codec";
	if (off < SLOT_HDR || off > len || cap > len - off || csize > cap)
		return "bad offset or size";
	if (!usize || usize > SLOT_MAX_PAYLOAD)
		return "bad payload size";
	if (SLOT_SCRATCH + usize > LOW_RAM_END ||
	    (SLOT_SCRATCH < rd + len && rd < SLOT_SCRATCH + usize) ||
	    (SLOT_SCRATCH < dtb_end && dtb < SLOT_SCRATCH + usize) ||
	    (SLOT_SCRATCH < (uintptr_t)_end && (uintptr_t)_head < SLOT_SCRATCH + usize))
		return "scratch RAM overlaps";
	out("crc...");
	if (crc32_dev(rd + off, csize) != crc_c)
		return "compressed CRC differs";
	out(" inflate...");
	uint64_t got = 0;
	int err = jt_inflate((uint8_t *)SLOT_SCRATCH, usize, &got,
	    (const uint8_t *)(rd + off), csize);
	if (err || got != usize)
		return "inflate failed";
	out(" crc...");
	if (crc32_dev(SLOT_SCRATCH, usize) != crc_u)
		return "payload CRC differs";
	out(" ok\n");
	*plen = usize;
	return 0;
}

static void slot_record(uint32_t slot, uint32_t reason, uint32_t seq,
    uint32_t ms, uint32_t def, uint32_t gen)
{
	volatile uint32_t *b = (volatile uint32_t *)JT_SLOT_BOOT_PA;
	uint32_t x = 0;

	b[0] = BOOT_MAGIC;
	b[1] = 2;
	b[2] = slot;
	b[3] = reason;
	b[4] = seq;
	b[5] = *(volatile uint32_t *)(JT_PMU_BASE + JT_PMU_RST_STAT);
	b[6] = ms;
	b[7] = def;
	b[8] = gen;
	b[9] = 0;
	b[10] = 0;
	for (int i = 0; i < 11; i++)
		x ^= b[i];
	b[11] = x ^ 0xffffffffU;
	__asm__ volatile("dsb sy" ::: "memory");
}

/* 0: not a container (rd unchanged); 1: rd now points at the decoded
 * payload; -1: nothing bootable (fail() already printed). */
static int slots_select(uint64_t *rd_start, uint64_t *rd_end, uintptr_t dtb)
{
	uintptr_t rd = (uintptr_t)*rd_start;
	uint64_t len = *rd_end - *rd_start, plen = 0;
	volatile uint32_t *f = (volatile uint32_t *)JT_SLOT_FLAG_PA;
	int want = -1, def = -1, order[3], n = 0;
	uint32_t seq = 0, why[3];
	uint64_t t0 = ticks();

	if (len < SLOT_HDR || rd32(rd) != SLOT_MAGIC0 || rd32(rd + 4) != SLOT_MAGIC1)
		return 0;
	out("slots: container\n");
	if (rd32(rd + 8) != SLOT_VERSION || rd32(rd + 12) != SLOT_HDR ||
	    rd32(rd + 16) != 2 || crc32_dev(rd, 24) != rd32(rd + 24)) {
		fail("slots: bad container header");
		return -1;
	}
	if (f[0] == TRY_MAGIC && f[1] == 1 && f[2] < 2 &&
	    f[4] == (f[0] ^ f[1] ^ f[2] ^ f[3] ^ 0xffffffffU)) {
		want = (int)f[2];
		seq = f[3];
	}
	/* Wipe the request before the slot is touched: it is good for one try. */
	for (int i = 0; i < 8; i++)
		f[i] = 0;
	__asm__ volatile("dsb sy" ::: "memory");

	for (int i = 0; i < 2; i++)
		if (slot_good(rd, i) && (def < 0 || slot_gen(rd, i) > slot_gen(rd, def)))
			def = i;
	if (want >= 0 && want != def) {
		order[n] = want;
		why[n++] = WHY_TRY;
	}
	if (def >= 0) {
		order[n] = def;
		why[n++] = want >= 0 && want != def ? WHY_TRY_FAILED : WHY_DEFAULT;
		if (slot_good(rd, 1 - def) && 1 - def != want) {
			order[n] = 1 - def;
			why[n++] = WHY_OTHER_GOOD;
		}
	}
	for (int k = 0; k < n; k++) {
		int i = order[k];
		const char *bad;

		out(i ? "slots: slot 1" : "slots: slot 0");
		out(why[k] == WHY_TRY ? " (try once)\n" : "\n");
		bad = slot_decode(rd, len, i, dtb, &plen);
		if (bad) {
			out("slots: refused: ");
			out(bad);
			out("\n");
			continue;
		}
		uint32_t ms = ms_since(t0);
		slot_record((uint32_t)i, why[k], seq, ms,
		    def < 0 ? 0xffffffffU : (uint32_t)def, slot_gen(rd, i));
		out("slots: booting slot ");
		dec((uint64_t)i);
		out(", ");
		dec(ms);
		out(" ms\n");
		*rd_start = SLOT_SCRATCH;
		*rd_end = SLOT_SCRATCH + plen;
		return 1;
	}
	fail("slots: nothing bootable");
	return -1;
}

static void node_line(uintptr_t dtb, const char *name)
{
	out(fdt_has_node(dtb, name) ? "has " : "no  ");
	out(name);
	out("\n");
}

/* Dump the facts we can only confirm on the phone. Runs before any payload
 * work, so a first flash teaches us the real device even without a payload. */
static void dt_probe(uintptr_t dtb)
{
	out("--- live device tree ---\n");
	char buf[80];

	if (fdt_node_str(dtb, "chosen", "bootargs", buf, sizeof(buf)) > 0) {
		out("bootargs: ");
		out(buf);
		out("\n");
	}
	uint64_t v;
	if (fdt_chosen_u64(dtb, "linux,initrd-start", &v))
		kv("initrd-start", v);
	if (fdt_chosen_u64(dtb, "linux,initrd-end", &v))
		kv("initrd-end", v);
	out("\n");
	uint64_t mbase[8], msize[8];
	int nm = fdt_memory_all(dtb, mbase, msize, 8);
	if (nm > 0) {
		uint64_t total = 0;
		for (int i = 0; i < nm; i++) {
			out("mem bank ");
			dec((uint64_t)i);
			kv(" base", mbase[i]);
			kv("size", msize[i]);
			out("(");
			dec(msize[i] >> 20);
			out(" MiB)\n");
			total += msize[i];
		}
		out("mem total ");
		dec(total >> 20);
		out(" MiB\n");
	} else {
		out("no /memory node parsed\n");
	}
	node_line(dtb, "chosen");
	node_line(dtb, "memory");
	node_line(dtb, "reserved-memory");
	node_line(dtb, "framebuffer");
	node_line(dtb, "decon_f");
	node_line(dtb, "dwmmc0");
	node_line(dtb, "dwmmc_0");
	node_line(dtb, "dwmmc1");
	node_line(dtb, "dwmmc2");
	node_line(dtb, "system-controller");
	node_line(dtb, "interrupt-controller");
	node_line(dtb, "ramoops");
	out("------------------------\n");
}

uint64_t jt_main(uintptr_t dtb, uint64_t el, uint64_t sctlr)
{
	/* Hoisted (not printed here): the logo decision needs the ramdisk bounds
	 * before the banner draws anything, and the probe below reports them in
	 * exactly the old order. */
	uint64_t rd_start = 0, rd_end = 0;
	int have_rd = fdt_chosen_u64(dtb, "linux,initrd-start", &rd_start) &&
		      fdt_chosen_u64(dtb, "linux,initrd-end", &rd_end);
#if JT_HAVE_FB
	/* Re-arm the DECON HW trigger so the panel keeps scanning the handover
	 * framebuffer, then clear it for the banner. This is a mode bit, not a
	 * one-shot: once armed, DECON pushes the buffer on every TE. */
#if JT_DECON_TRIG
	*(volatile uint32_t *)JT_DECON_TRIG = JT_DECON_TRIG_UNMASK;
#endif
	for (int y = 0; y < (int)JT_FB_HEIGHT; y++)
		for (int x = 0; x < (int)JT_FB_WIDTH; x++)
			((volatile uint32_t *)JT_FB_BASE)[y * (JT_FB_STRIDE / 4) + x] = 0xff000000U;
	fg = 0xffffff00U;

	/* Optional Apple logo at the iOS userspace boot position/size. Only the
	 * payload cmdline can switch it on, so a probe-only image stays off. */
	int logo = 0;
	if (have_rd && rd_start >= LOW_RAM_BASE && rd_end <= LOW_RAM_END &&
	    rd_end > rd_start)
		logo = payload_logo((uintptr_t)rd_start, (uintptr_t)rd_end);
	if (logo) {
		blit_logo(LOGO_X, LOGO_Y);
		/* Keep the console below the logo instead of over it. */
		scr_top = (unsigned int)(LOGO_Y + (int)LOGO_H + 24);
		scr_rows = SCR_ROWS(scr_top);
	}
#endif
#if JT_HAVE_RAMOOPS
	prz->sig = PRZ_SIG;
	prz->start = prz->size = 0;
	prz2->sig = PRZ_SIG;
	prz2->start = prz2->size = 0;
#endif
	out("JACKPOTLTE LOADER BEGIN  (ios-jackpotlte, Exynos 7885)\n");
	fg = 0xffffffffU;
#if JT_HAVE_FB
	if (logo) {
		out("logo: on ");
		dec(LOGO_W);
		out("x");
		dec(LOGO_H);
		out(" at ");
		dec(LOGO_X);
		out(",");
		dec(LOGO_Y);
		out("\n");
	} else {
		out("logo: off\n");
	}
#endif
	kv("entry EL", el);
	if (el == 2)
		out("(EL2 handoff, dropped to EL1 in start.S)\n");
	kv("SCTLR", sctlr);
	kv("DTB", dtb);
	out("\n");
	dt_probe(dtb);
	/* start.S drops an EL2 handoff to EL1; anything else means the shim is
	 * running below EL1 and the EL1 register accesses above would have
	 * trapped. */
	if (el != 1 && el != 2)
		return fail("entered below EL1");

	if (!have_rd)
		return fail("no initrd in /chosen");
	kv("initrd", rd_start);
	kv("end", rd_end);
	out("\n");
	if (rd_start < LOW_RAM_BASE || rd_end > LOW_RAM_END || rd_end <= rd_start)
		return fail("initrd outside low RAM");
	if (slots_select(&rd_start, &rd_end, dtb) < 0)
		return 0;

	const volatile struct payload *p = (const volatile struct payload *)rd_start;
	uint64_t length = rd_end - rd_start;
	if (length < sizeof(struct payload) || !magic_ok(p->magic) || p->version != 1) {
		/* Stage 2: the loader has printed the banner and the DT facts; a
		 * missing/placeholder payload is expected until a kernel exists.
		 * Stop cleanly (green) instead of a red FAIL, so the screen shows
		 * the probe result rather than looking like a crash. */
		fg = 0xff40ff40U;
		out("no SWEETXNU payload in the ramdisk; probe-only run\n");
		canary();
		for (;;)
			__asm__ volatile("wfe");
	}
	uint64_t nsegs = p->nsegs;
	kv("segs", nsegs);
	kv("virt", p->virt_base);
	kv("entry", p->entry_va);
	out("\n");
	kv("phys", p->phys_base);
	kv("mem", p->mem_size);
	kv("adt", p->adt_size);
	out("\n");
	/*
	 * The window may not pass the end of the /memory bank S-Boot reports
	 * for it: 2 GiB at 0x880000000 on the 4 GB SM-A530W (Android's
	 * /proc/iomem: 880000000-8ffffffff System RAM), 1 GiB on 3 GB boards
	 * (exynos7885-jackpotlte_common.dtsi). Clamp to the live bank, and to
	 * the old 512 MiB if no bank holds the window at all.
	 */
	uint64_t mem_size = p->mem_size;
	{
		uint64_t mb[8], ms[8], bank_end = 0;
		int nb = fdt_memory_all(dtb, mb, ms, 8);

		for (int i = 0; i < nb; i++) {
			if (p->phys_base >= mb[i] && p->phys_base - mb[i] < ms[i])
				bank_end = mb[i] + ms[i];
		}
		if (bank_end == 0) {
			if (mem_size > 0x20000000ULL)
				mem_size = 0x20000000ULL;
			out("no /memory bank holds the XNU window: 512 MiB\n");
		} else if (mem_size > bank_end - p->phys_base) {
			mem_size = (bank_end - p->phys_base) & ~0x1fffffULL;
			out("XNU window clamped to its /memory bank\n");
		}
		kv("mem used", mem_size);
		out("\n");
	}
	if (!nsegs || nsegs > 32 ||
	    __builtin_offsetof(struct payload, segs) + nsegs * sizeof(struct payload_seg) > length ||
	    p->adt_off > length || p->adt_size > length - p->adt_off)
		return fail("bad payload header");
	if (p->phys_base < XNU_RAM_BASE || p->phys_base >= XNU_RAM_END || !mem_size ||
	    mem_size > XNU_RAM_END - p->phys_base ||
	    p->entry_va < p->virt_base || p->kernel_end < p->virt_base)
		return fail("payload RAM outside the XNU bank");
	uintptr_t mem_end = p->phys_base + mem_size;
	uintptr_t kernel_end = p->phys_base + (p->kernel_end - p->virt_base);
	if (kernel_end >= mem_end)
		return fail("kernel larger than RAM window");

	/* iBoot hands XNU zeroed RAM; S-Boot leaves the previous OS's data.
	 * (dreamlte boot 3-7: not zeroing made vm_mem_bootstrap hang.) */
	out("zeroing XNU RAM...\n");
	zero(p->phys_base, mem_size);
	out("zeroed\n");

	for (uint64_t i = 0; i < nsegs; i++) {
		const volatile struct payload_seg *s = &p->segs[i];
		if (s->vmaddr < p->virt_base || s->file_size > s->vmsize ||
		    s->file_off > length || s->file_size > length - s->file_off ||
		    s->vmsize > mem_size || s->vmaddr - p->virt_base > mem_size - s->vmsize)
			return fail("bad segment");
		uintptr_t dst = p->phys_base + (s->vmaddr - p->virt_base);
		copy(dst, rd_start + s->file_off, s->file_size);
		zero(dst + s->file_size, s->vmsize - s->file_size);
		clean(dst, dst + s->vmsize);
		out("seg ");
		hex(i);
		out(" -> ");
		hex(dst);
		out(" ");
		hex(s->vmsize);
		out("\n");
	}

	uintptr_t adt_pa = kernel_end;
	uintptr_t ba_pa = (adt_pa + p->adt_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	if (ba_pa + PAGE_SIZE > mem_end)
		return fail("no room for ADT/boot_args");
	copy(adt_pa, rd_start + p->adt_off, p->adt_size);
	zero(ba_pa, PAGE_SIZE);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = p->phys_base;
	ba->memSize = mem_size;
	ba->topOfKernelData = ba_pa + PAGE_SIZE;
	ba->deviceTreeP = p->virt_base + (adt_pa - p->phys_base);
	ba->deviceTreeLength = (uint32_t)p->adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = p->cmdline[i];
	ba->memSizeActual = mem_size;
	clean(adt_pa, ba_pa + PAGE_SIZE);

	uintptr_t entry_pa = p->phys_base + (p->entry_va - p->virt_base);
	if (entry_pa >= kernel_end)
		return fail("entry outside kernel");
	jt_boot_args = ba_pa;
	kv("adt_pa", adt_pa);
	kv("boot_args", ba_pa);
	out("\n");
	kv("entry_pa", entry_pa);
	out("\ncmdline: ");
	for (int i = 0; i < BOOT_LINE_LENGTH && p->cmdline[i]; i++) {
		char c[2] = { p->cmdline[i], 0 };
		out(c);
	}
	out("\n");
	fg = 0xff40ff40U;
	out("\nJUMP TO XNU\n");
#if JT_HAVE_RAMOOPS
	clean(JT_RAMOOPS_CONSOLE, JT_RAMOOPS_CONSOLE + JT_RAMOOPS_SIZE);
	clean(JT_RAMOOPS_PMSG, JT_RAMOOPS_PMSG + JT_RAMOOPS_SIZE);
#endif
	return entry_pa;
}

void jt_exception(uint64_t slot)
{
	uint64_t esr, elr, far;
	__asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
	__asm__ volatile("mrs %0, elr_el1" : "=r"(elr));
	__asm__ volatile("mrs %0, far_el1" : "=r"(far));
	fg = 0xffff4040U;
	out("\nLOADER EXCEPTION ");
	kv("slot", slot);
	kv("ESR", esr);
	kv("ELR", elr);
	kv("FAR", far);
	out("\n");
	canary();
	for (;;)
		__asm__ volatile("wfe");
}

/* gcc may emit calls for struct copies; keep the image self-contained. */
void *memset(void *d, int c, unsigned long n)
{
	unsigned char *q = d;
	while (n--)
		*q++ = (unsigned char)c;
	return d;
}

void *memcpy(void *d, const void *s, unsigned long n)
{
	unsigned char *q = d;
	const unsigned char *r = s;
	while (n--)
		*q++ = *r++;
	return d;
}
