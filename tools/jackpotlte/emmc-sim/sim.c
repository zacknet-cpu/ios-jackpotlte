/*
 * User-space model of the Exynos dw_mmc host + eMMC card, enough to run
 * a5xelte_emmc.c's one-sector and multi-block paths, the self-tests and the
 * strategy routine, with fault injection.
 */
#include "sim_kernel.h"
#include <zlib.h>

#define CARD_SECTORS 100000u
#define UD_FIRST 4096u
#define UD_COUNT 65536u
#define SYS_FIRST 80000u
#define SYS_COUNT 9000u

static uint8_t *card;            /* backing store */
static uint32_t regs[0x400 / 4];
static uint32_t rint;
static uint64_t fifo[64];
static unsigned fifo_head, fifo_n;
static bool half_read, half_written;
static uint32_t half_lo;
static int ctrl_reset_ticks;
static uint64_t ticks;

/* transfer state */
static bool xfer_active, xfer_write, xfer_multi, xfer_stop;
static uint64_t xfer_pos, xfer_end;   /* byte offsets in card */
static uint32_t xfer_moved;           /* bytes moved card side */
static int cmd_done_in, acd_in, busy_ticks;
static bool dto_set;
static uint32_t fifo_stall;           /* ticks FIFO full (read) / empty (write) */

/* fault injection */
int f_crc_every;          /* set DCRC midway through every Nth multi command */
int f_corrupt_reads;      /* swap halves of an entry in multi reads */
int f_no_acd;             /* never set ACD */
int f_drop_last_block;    /* multi writes lose their last block */
int f_hto_every;          /* raise HTO midway through every Nth multi command */
static int multi_count;
static bool this_crc, this_hto;

uint64_t n_cmd17, n_cmd18, n_cmd24, n_cmd25, n_cmd12;
int sim_preempt;

static void
fifo_push(uint64_t v)
{
	fifo[(fifo_head + fifo_n) % 64] = v;
	fifo_n++;
}

static uint64_t
fifo_pop(void)
{
	uint64_t v = fifo[fifo_head];
	fifo_head = (fifo_head + 1) % 64;
	fifo_n--;
	return v;
}

static void
tick(void)
{
	ticks++;
	if (ctrl_reset_ticks > 0 && --ctrl_reset_ticks == 0) {
		regs[0] &= ~2u;
		fifo_n = 0;
		fifo_head = 0;
		half_read = half_written = false;
	}
	if (cmd_done_in > 0 && --cmd_done_in == 0) {
		rint |= 1u << 2;
		regs[0x2c / 4] &= ~(1u << 31);
	}
	if (busy_ticks > 0) {
		busy_ticks--;
	}
	if (acd_in > 0 && --acd_in == 0 && !f_no_acd) {
		rint |= 1u << 14;
	}
	if (!xfer_active) {
		return;
	}
	if (cmd_done_in > 0) {
		return;
	}
	uint32_t total = (uint32_t)(xfer_end - xfer_pos);
	if (xfer_multi && (this_crc || this_hto) && xfer_moved >= total / 2) {
		rint |= this_crc ? (1u << 7) : (1u << 10);
		this_crc = this_hto = false;
		xfer_active = false; /* the data machine stops */
		return;
	}
	for (int k = 0; k < 2; k++) {
		if (!xfer_write) {
			if (xfer_moved < total && fifo_n < 64) {
				uint64_t v;
				memcpy(&v, card + xfer_pos + xfer_moved, 8);
				if (f_corrupt_reads && xfer_multi && xfer_moved == 4096) {
					v = (v >> 32) | (v << 32);
				}
				fifo_push(v);
				xfer_moved += 8;
				if (fifo_n > 16) {
					rint |= 1u << 5; /* RXDR above watermark */
				}
			}
		} else if (xfer_moved < total && fifo_n > 0) {
			uint64_t v = fifo_pop();
			bool drop = f_drop_last_block && xfer_multi && xfer_moved >= total - 512;
			if (!drop) {
				memcpy(card + xfer_pos + xfer_moved, &v, 8);
			}
			xfer_moved += 8;
			if (fifo_n < 48) {
				rint |= 1u << 4; /* TXDR */
			}
		}
	}
	if (xfer_moved >= total && !dto_set) {
		dto_set = true;
		rint |= 1u << 3;
		xfer_active = xfer_write ? false : true;
		if (xfer_stop) {
			acd_in = 5;
		}
		if (xfer_write) {
			busy_ticks = 50;
		}
	}
	if (!xfer_write && dto_set && fifo_n == 0) {
		xfer_active = false;
	}
}

static uint32_t
status(void)
{
	uint32_t s = (fifo_n & 0x1fffu) << 17;
	if (fifo_n == 0) {
		s |= 1u << 2;
	}
	if (fifo_n == 64) {
		s |= 1u << 3;
	}
	if (busy_ticks) {
		s |= 1u << 9;
	}
	if (xfer_active) {
		s |= 1u << 10;
	}
	return s | (1u << 8);
}

uint32_t
sim_rd(uint32_t off)
{
	tick();
	switch (off) {
	case 0x044:
		return rint;
	case 0x048:
		return status();
	case 0x05c:
		return xfer_moved;
	case 0x200:
		if (!fifo_n) {
			fprintf(stderr, "SIM: read of empty FIFO\n");
			exit(3);
		}
		half_read = true;
		return (uint32_t)fifo[fifo_head];
	case 0x204: {
		if (!half_read || !fifo_n) {
			fprintf(stderr, "SIM: unpaired FIFO read\n");
			exit(3);
		}
		half_read = false;
		uint64_t v = fifo_pop();
		return (uint32_t)(v >> 32);
	}
	default:
		return regs[off / 4];
	}
}

static void
start_cmd(uint32_t v)
{
	uint32_t idx = v & 0x3f;
	bool data = v & (1u << 9);
	if (!(v & (1u << 31))) {
		return;
	}
	if (idx == 12) {
		n_cmd12++;
		xfer_active = false;
		cmd_done_in = 3;
		busy_ticks = 10;
		return;
	}
	if (!data) {
		cmd_done_in = 3;
		return;
	}
	if (xfer_active) {
		fprintf(stderr, "SIM: data command while a transfer is active\n");
		exit(3);
	}
	uint32_t bytcnt = regs[0x20 / 4], arg = regs[0x28 / 4];
	if (regs[0x1c / 4] != 512 || bytcnt % 512 || bytcnt == 0) {
		fprintf(stderr, "SIM: bad BLKSIZ/BYTCNT %u/%u\n", regs[0x1c / 4], bytcnt);
		exit(3);
	}
	if ((uint64_t)arg + bytcnt / 512 > CARD_SECTORS) {
		fprintf(stderr, "SIM: transfer past the card end lba=%u n=%u\n", arg, bytcnt / 512);
		exit(3);
	}
	xfer_write = v & (1u << 10);
	xfer_multi = idx == 18 || idx == 25;
	xfer_stop = v & (1u << 12);
	if ((idx == 17 || idx == 24) && bytcnt != 512) {
		fprintf(stderr, "SIM: CMD%u with %u bytes\n", idx, bytcnt);
		exit(3);
	}
	if (xfer_multi && !xfer_stop) {
		fprintf(stderr, "SIM: open-ended multi-block command without auto stop\n");
		exit(3);
	}
	if (xfer_write && (arg < UD_FIRST || arg + bytcnt / 512 > UD_FIRST + UD_COUNT) &&
	    (arg < SYS_FIRST || arg + bytcnt / 512 > SYS_FIRST + 8193)) {
		fprintf(stderr, "SIM: write outside USERDATA and the log area lba=%u\n", arg);
		exit(3);
	}
	switch (idx) {
	case 17: n_cmd17++; break;
	case 18: n_cmd18++; break;
	case 24: n_cmd24++; break;
	case 25: n_cmd25++; break;
	default:
		fprintf(stderr, "SIM: unexpected data CMD%u\n", idx);
		exit(3);
	}
	if (xfer_multi) {
		multi_count++;
		this_crc = f_crc_every && multi_count % f_crc_every == 0;
		this_hto = f_hto_every && multi_count % f_hto_every == 0;
	}
	xfer_pos = (uint64_t)arg * 512u;
	xfer_end = xfer_pos + bytcnt;
	xfer_moved = 0;
	dto_set = false;
	xfer_active = true;
	cmd_done_in = 4;
	fifo_stall = 0;
}

void
sim_wr(uint32_t off, uint32_t v)
{
	tick();
	switch (off) {
	case 0x000:
		regs[0] = v;
		if (v & 2u) {
			ctrl_reset_ticks = 2;
		}
		return;
	case 0x02c:
		regs[off / 4] = v;
		start_cmd(v);
		return;
	case 0x044:
		rint &= ~v;
		return;
	case 0x200:
		if (half_written) {
			fprintf(stderr, "SIM: unpaired FIFO write\n");
			exit(3);
		}
		half_written = true;
		half_lo = v;
		return;
	case 0x204:
		if (!half_written || fifo_n >= 64) {
			fprintf(stderr, "SIM: bad FIFO write (paired %d, fill %u)\n", half_written, fifo_n);
			exit(3);
		}
		half_written = false;
		fifo_push((uint64_t)half_lo | ((uint64_t)v << 32));
		return;
	default:
		regs[off / 4] = v;
	}
}

/* ---- kernel stand-ins ---- */
int hz = 100;
static char bootargs_root[16] = "USERDATA";
static int ba_write = 1, ba_log = 1;
static uint32_t ba_multi = 7;
int eno_dump(void) { return 0; }
int eno_stop(void) { return 0; }
int eno_reset(void) { return 0; }
int eno_select(void) { return 0; }
int eno_mmap(void) { return 0; }
int eno_strat(void) { return 0; }
int eno_getc(void) { return 0; }
int eno_putc(void) { return 0; }
int physio(void (*s)(buf_t), void *b, dev_t d, int f, void *m, struct uio *u, int bs) { return 0; }
void minphys(void) {}
int bdevsw_add(int i, const struct bdevsw *b) { return 1; }
int cdevsw_add_with_bdev(int i, const struct cdevsw *c, int b) { return 2; }
void *devfs_make_node(dev_t d, int c, int u, int g, int p, const char *f, ...) { return NULL; }
uint32_t buf_count(buf_t b) { return b->count; }
void buf_setresid(buf_t b, uint32_t r) { b->resid = r; }
int64_t buf_blkno(buf_t b) { return b->blkno; }
int buf_flags(buf_t b) { return b->flags; }
int buf_map(buf_t b, caddr_t *v) { *v = b->data; return 0; }
int buf_unmap(buf_t b) { return 0; }
void buf_seterror(buf_t b, int e) { b->error = e; }
void buf_biodone(buf_t b) { b->done = 1; }
static int lock_depth;
void sim_lock(int *m) { if (lock_depth++) { fprintf(stderr, "SIM: recursive lock\n"); exit(3); } }
void sim_unlock(int *m) { lock_depth--; }
int tsleep(void *c, int p, const char *w, int t) { return 0; }
int
PE_parse_boot_argn(const char *n, void *v, int size)
{
	if (!strcmp(n, "emmcroot")) { strncpy(v, bootargs_root, size); return 1; }
	if (!strcmp(n, "emmcwrite")) { *(int *)v = ba_write; return 1; }
	if (!strcmp(n, "a5log")) { *(int *)v = ba_log; return 1; }
	if (!strcmp(n, "emmcmulti")) { *(uint32_t *)v = ba_multi; return 1; }
	return 0;
}
static uintptr_t dt_reg[2] = { 0x13500000u, 0x10000u };
int SecureDTLookupEntry(void *p, const char *n, DTEntry *e) { *e = dt_reg; return 0; }
int
SecureDTGetProperty(DTEntry e, const char *n, void const **v, unsigned int *s)
{
	*v = dt_reg;
	*s = sizeof(dt_reg);
	return 0;
}
vm_offset_t ml_io_map(vm_offset_t pa, vm_size_t sz) { return 0x1000; }
kern_return_t kernel_thread_start(thread_continue_t c, void *a, thread_t *t) { *t = NULL; return 0; }
void thread_deallocate(thread_t t) {}
uint64_t
mach_absolute_time(void)
{
	if (lock_depth == 0 && sim_preempt) {
		fprintf(stderr, "SIM: preemption left disabled\n");
		exit(3);
	}
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
void clock_interval_to_deadline(uint32_t i, uint64_t s, uint64_t *d) { *d = mach_absolute_time() + i * s; }
void absolutetime_to_nanoseconds(uint64_t a, uint64_t *n) { *n = a; }
void nanoseconds_to_absolutetime(uint64_t n, uint64_t *a) { *a = n; }

#include "driver.c"

/* ---- harness ---- */
static void
put_le32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> (8 * i); }
static void
put_le64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = v >> (8 * i); }

static void
make_card(void)
{
	card = malloc((size_t)CARD_SECTORS * 512);
	srand(1);
	for (size_t i = 0; i < (size_t)CARD_SECTORS * 512; i++) {
		card[i] = rand();
	}
	uint8_t *h = card + 512, *e = card + 1024;
	memset(h, 0, 512);
	memset(e, 0, 512);
	const char *names[2] = { "USERDATA", "SYSTEM" };
	uint64_t first[2] = { UD_FIRST, SYS_FIRST }, count[2] = { UD_COUNT, SYS_COUNT };
	for (int k = 0; k < 2; k++) {
		uint8_t *p = e + k * 128;
		p[0] = 1; /* type GUID non-zero */
		put_le64(p + 32, first[k]);
		put_le64(p + 40, first[k] + count[k] - 1);
		for (size_t c = 0; c < strlen(names[k]); c++) {
			p[56 + c * 2] = names[k][c];
		}
	}
	memcpy(h, "EFI PART", 8);
	put_le32(h + 8, 0x10000);
	put_le32(h + 12, 92);
	put_le64(h + 24, 1);
	put_le64(h + 40, 34);
	put_le64(h + 48, CARD_SECTORS - 34);
	put_le64(h + 72, 2);
	put_le32(h + 80, 4);
	put_le32(h + 84, 128);
	put_le32(h + 88, (uint32_t)crc32(0, e, 512));
	put_le32(h + 16, (uint32_t)crc32(0, h, 92));
	regs[0x004 / 4] = 1;
	regs[0x010 / 4] = 1;
	regs[0x06c / 4] = 0x5342270au;
	regs[0x070 / 4] = 0x08e42541u;
	regs[0x0a8 / 4] = 0x00010000u;
	regs[0x000 / 4] = 0x10;
}

static int
do_io(bool rd, int64_t blkno, uint32_t count, uint8_t *data)
{
	struct sim_buf b = { .count = count, .flags = rd ? B_READ : B_WRITE,
		             .blkno = blkno, .data = (char *)data };
	emmc_strategy(&b);
	if (!b.done || lock_depth || sim_preempt) {
		fprintf(stderr, "SIM: strategy did not complete cleanly\n");
		exit(3);
	}
	if (!b.error && b.resid) {
		fprintf(stderr, "SIM: resid %u without an error\n", b.resid);
		exit(3);
	}
	return b.error;
}

static int failures;

static void
random_io(int rounds, uint32_t secsize)
{
	uint8_t *raw = malloc(2u << 20), *ref = malloc(2u << 20);
	for (int r = 0; r < rounds; r++) {
		bool rd = rand() % 3 != 0;
		uint32_t blocks = 1 + rand() % ((1u << 20) / secsize);
		if (rand() % 4 == 0) {
			blocks = 1 + rand() % 3;
		}
		uint32_t count = blocks * secsize;
		if (rand() % 10 == 0 && secsize == 512) {
			count = 1 + rand() % 3000; /* partial sectors */
		}
		uint64_t maxblk = (uint64_t)UD_COUNT * 512 / secsize - (count + secsize - 1) / secsize;
		int64_t blkno = rand() % maxblk;
		uint8_t *data = raw + (rand() % 8); /* unaligned buffers too */
		uint64_t off = (uint64_t)UD_FIRST * 512 + (uint64_t)blkno * secsize;
		if (rd) {
			memset(data, 0xee, count);
			int e = do_io(true, blkno, count, data);
			if (e || memcmp(data, card + off, count)) {
				printf("FAIL: read blk %lld count %u error %d\n", (long long)blkno, count, e);
				failures++;
			}
		} else {
			for (uint32_t i = 0; i < count; i++) {
				data[i] = rand();
			}
			memcpy(ref, card + off - 512, count + 1024);
			int e = do_io(false, blkno, count, data);
			if (e || memcmp(card + off, data, count) ||
			    memcmp(card + off - 512, ref, 512) ||
			    memcmp(card + off + count, ref + 512 + count, 512)) {
				printf("FAIL: write blk %lld count %u error %d\n", (long long)blkno, count, e);
				failures++;
			}
		}
	}
	free(raw);
	free(ref);
}

static void
reset_driver(uint32_t multi)
{
	emmc_ready = false;
	emmc_regs = 0;
	emmc_log_enabled = false;
	emmc_multi = 0;
	emmc_multi_errors = 0;
	emmc_secsize = 512;
	memset(&emmc_stats, 0, sizeof(emmc_stats));
	ba_multi = multi;
	n_cmd17 = n_cmd18 = n_cmd24 = n_cmd25 = n_cmd12 = 0;
	dev_t dev;
	if (a5xelte_emmc_rootdev(&dev)) {
		printf("FAIL: rootdev\n");
		exit(1);
	}
}

int
main(int argc, char **argv)
{
	make_card();
	uint8_t *log_before = malloc(64 * 512);
	memcpy(log_before, card + (size_t)(SYS_FIRST + 8193 - 64) * 512, 64 * 512);

	printf("== emmcmulti=0 (old path)\n");
	reset_driver(0);
	random_io(150, 512);
	printf("   cmd17 %llu cmd18 %llu cmd24 %llu cmd25 %llu\n", n_cmd17, n_cmd18, n_cmd24, n_cmd25);
	if (n_cmd18 || n_cmd25) { printf("FAIL: multi-block used with emmcmulti=0\n"); failures++; }

	printf("== emmcmulti=7\n");
	reset_driver(7);
	if (memcmp(log_before, card + (size_t)(SYS_FIRST + 8193 - 64) * 512, 64 * 512)) {
		printf("FAIL: write self-test did not restore the log sectors\n");
		failures++;
	}
	if (emmc_multi != 7) { printf("FAIL: mode %u after self-test\n", emmc_multi); failures++; }
	random_io(400, 512);
	emmc_secsize = 4096;
	random_io(200, 4096);
	emmc_secsize = 512;
	printf("   cmd17 %llu cmd18 %llu cmd24 %llu cmd25 %llu cmd12 %llu errors %llu\n",
	    n_cmd17, n_cmd18, n_cmd24, n_cmd25, n_cmd12, emmc_stats.errors);

	printf("== emmcmulti=3, a DCRC every 7th multi-block command\n");
	f_crc_every = 7;
	reset_driver(3);
	random_io(300, 512);
	printf("   mode now %u, errors %u, cmd12 %llu\n", emmc_multi, emmc_multi_errors, n_cmd12);
	f_crc_every = 0;

	printf("== emmcmulti=3, HTO every 5th multi-block command\n");
	f_hto_every = 5;
	reset_driver(3);
	random_io(100, 512);
	printf("   mode now %u, errors %u\n", emmc_multi, emmc_multi_errors);
	f_hto_every = 0;

	printf("== emmcmulti=7, corrupted multi-block reads (self-test must refuse)\n");
	f_corrupt_reads = 1;
	reset_driver(7);
	if (emmc_multi != 0) { printf("FAIL: mode %u with corrupt reads\n", emmc_multi); failures++; }
	random_io(100, 512);
	f_corrupt_reads = 0;

	printf("== emmcmulti=7, multi-block writes lose a block (self-test must refuse)\n");
	f_drop_last_block = 1;
	reset_driver(7);
	if (emmc_multi & EMMC_MULTI_WRITE) { printf("FAIL: writes kept with dropped blocks\n"); failures++; }
	random_io(150, 512);
	printf("   mode now %u\n", emmc_multi);
	f_drop_last_block = 0;

	printf("== emmcmulti=3, then blocks get lost after the self-test (verify off: expected loss)\n");
	reset_driver(3);
	f_drop_last_block = 1;
	int before = failures;
	random_io(50, 512);
	printf("   %d expected write failures without read-back\n", failures - before);
	failures = before;
	f_drop_last_block = 0;

	printf("== emmcmulti=7, blocks get lost after the self-test (read-back must repair)\n");
	reset_driver(7);
	f_drop_last_block = 1;
	random_io(150, 512);
	printf("   mode now %u\n", emmc_multi);
	f_drop_last_block = 0;

	printf("== emmcmulti=3, no ACD from the host (self-test must refuse)\n");
	f_no_acd = 1;
	reset_driver(3);
	if (emmc_multi) { printf("FAIL: mode %u without ACD\n", emmc_multi); failures++; }
	random_io(20, 512);
	f_no_acd = 0;

	printf("== emmcmulti=7, one HTO during the read self-test (one retry allowed)\n");
	multi_count = 0;
	f_hto_every = 1000000;
	multi_count = 1000000 - 1;
	reset_driver(7);
	f_hto_every = 0;
	if (emmc_multi != 7) { printf("FAIL: mode %u after one retried error\n", emmc_multi); failures++; }
	random_io(50, 512);

	printf("%s (%d failures)\n", failures ? "FAILED" : "ALL OK", failures);
	return failures != 0;
}
