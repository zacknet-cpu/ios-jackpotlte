/*
 * Host-side check of bsd/dev/a8_acpm.c: the real file, compiled for the PC
 * against stand-in kernel headers, run against the words peeked out of the
 * phone's ACPM SRAM (serial.log), with a toy APM core answering the IPC.
 *
 *   harness SERIAL.LOG FVP.BIN  (run.sh builds and runs it)
 *
 * It checks parsing (initdata, plug-ins, channels, fvmap), the queue
 * protocol in both directions, the plug-in load, and the refusals.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SRAM_PA   0x02052000u
#define SRAM_SIZE 0x1b000u

static uint32_t sram[SRAM_SIZE / 4];
static uint32_t mbox[0x1000 / 4];
static uint32_t tmu_cpu[0x1000 / 4], tmu_g3d[0x1000 / 4], pmu[0x10000 / 4], cmu_g3d[0x1000 / 4];
static uint64_t now_ns;
static const char *fvp_path;

/* what the test wants from the stand-ins */
uint32_t test_a8dvfs = 1;
uint32_t test_a8dvfstemp = 0;
bool test_tmu_refuses;
static bool apm_fvp_attached;
static bool apm_ignores_dvfs;           /* a plug-in that does not answer */
static uint32_t apm_rate[5] = { 1794000, 533000, 1352000, 1014000, 343000 };
static void (*watch_fn)(void *, int);
static int watch_blocks;

/* ---- kernel stand-ins ---- */
uintptr_t
ml_io_map(uintptr_t pa, uintptr_t size)
{
	(void)size;
	switch (pa) {
	case SRAM_PA: return (uintptr_t)sram;
	case 0x11c40000u: return (uintptr_t)mbox;
	case 0x10070000u: return (uintptr_t)tmu_cpu;
	case 0x10074000u: return (uintptr_t)tmu_g3d;
	case 0x11c80000u: return (uintptr_t)pmu;
	case 0x11400000u: return (uintptr_t)cmu_g3d;
	}
	printf("TEST: unexpected ml_io_map(0x%lx)\n", (unsigned long)pa);
	exit(2);
}

bool
a8_probe_read32(uintptr_t va, uint32_t *value, uint32_t *esr)
{
	if (test_tmu_refuses && va >= (uintptr_t)tmu_cpu && va < (uintptr_t)(tmu_cpu + 0x400)) {
		*esr = 0x96000010u;
		return false;
	}
	*value = *(volatile uint32_t *)va;
	*esr = 0;
	return true;
}

static uint32_t
crc32_of(const uint8_t *p, size_t n)
{
	uint32_t crc = ~0u;
	while (n--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++) {
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
		}
	}
	return ~crc;
}

int
a8f_read_whole(const char *path, uint8_t *buf, uint32_t max, uint32_t *got, uint32_t *crc)
{
	FILE *f = fopen(strcmp(path, "/private/var/root/fvp.fw") == 0 ? fvp_path : path, "rb");
	long size;

	*got = 0;
	*crc = 0;
	if (f == NULL) {
		return 2;
	}
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size <= 0 || (uint32_t)size > max) {
		fclose(f);
		return 27;
	}
	if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
		fclose(f);
		return 5;
	}
	fclose(f);
	*got = (uint32_t)size;
	*crc = crc32_of(buf, (size_t)size);
	return 0;
}

int
PE_parse_boot_argn(const char *name, void *out, int size)
{
	(void)size;
	if (strcmp(name, "a8dvfs") == 0) {
		*(uint32_t *)out = test_a8dvfs;
		return 1;
	}
	if (strcmp(name, "a8dvfstemp") == 0 && test_a8dvfstemp != 0) {
		*(uint32_t *)out = test_a8dvfstemp;
		return 1;
	}
	return 0;
}

uint64_t
mach_absolute_time(void)
{
	now_ns += 1000;
	return now_ns;
}

void
clock_interval_to_deadline(uint32_t interval, uint32_t scale, uint64_t *deadline)
{
	*deadline = now_ns + (uint64_t)interval * scale;
}

/* ---- the toy APM: takes commands when the AP is waiting ---- */
static uint32_t *
chan_rec(unsigned ch)
{
	return &sram[(sram[(0x4b80 + 8) / 4] + ch * 72) / 4];
}

static void
apm_answer(unsigned ch, const uint32_t cmd[4])
{
	uint32_t *c = chan_rec(ch);
	uint32_t front = sram[c[11] / 4], len = c[13];

	memcpy(&sram[(c[12] + front * 16) / 4], cmd, 16);
	sram[c[11] / 4] = (front + 1) % len;
	mbox[0x28 / 4] |= 1u << ch;
}

static void
apm_step(void)
{
	uint32_t raised = mbox[0x08 / 4] >> 16;

	/* INTCR1 written by the AP clears status bits */
	if (mbox[0x20 / 4] != 0) {
		mbox[0x28 / 4] &= ~mbox[0x20 / 4];
		mbox[0x20 / 4] = 0;
	}
	for (unsigned ch = 0; ch < 8; ch++) {
		uint32_t *c = chan_rec(ch);
		uint32_t rear, front, cmd[4];

		if (!(raised & (1u << ch)) || c[3] != 1) {
			continue;
		}
	again:
		rear = sram[c[4] / 4];
		front = sram[c[5] / 4];
		if (rear == front) {
			continue;
		}
		if (ch == 0 && (!apm_fvp_attached || apm_ignores_dvfs)) {
			continue;       /* nobody home: the command stays queued */
		}
		/* a full answer queue stalls the firmware */
		if ((sram[c[11] / 4] + 1) % c[13] == sram[c[10] / 4]) {
			printf("TEST: APM->AP queue of channel %u is full\n", ch);
			continue;
		}
		memcpy(cmd, &sram[(c[6] + rear * 16) / 4], 16);
		sram[c[4] / 4] = (rear + 1) % c[13];
		if (ch == 4 && (cmd[0] & (1u << 25))) {
			unsigned id = (cmd[0] >> 26) & 7;
			uint32_t *p = &sram[(sram[0x4b80 / 4] + id * 44) / 4];

			/* the code must be there before the attach */
			if (sram[0x7800 / 4] != 0x219c2201u) {
				printf("TEST: FAIL: attach before the firmware was copied\n");
				exit(1);
			}
			p[7] = 1;
			p[3] = 0x8001;
			apm_fvp_attached = true;
			apm_answer(ch, cmd);
		} else if (ch == 0) {
			unsigned dom = cmd[0] & 0xffff;

			if (dom < 5 && cmd[2] == 1) {
				cmd[1] = apm_rate[dom];
			} else if (dom < 5 && cmd[2] == 0) {
				apm_rate[dom] = cmd[1];
				if (dom == 4) {
					/* 26 MHz * M / 13 / 2 */
					cmu_g3d[0x120 / 4] = 0xa0000d01u | ((cmd[1] / 1000) << 16);
				}
			}
			apm_answer(ch, cmd);
		} else {
			apm_answer(ch, cmd);
		}
		goto again;             /* the firmware empties its queue */
	}
	mbox[0x08 / 4] = 0;
}

void
delay(int usec)
{
	now_ns += (uint64_t)usec * 1000u;
	apm_step();
}

int
kernel_thread_start(void (*fn)(void *, int), void *arg, void **thread)
{
	(void)arg;
	watch_fn = fn;
	*thread = NULL;
	return 0;
}

void
thread_deallocate(void *thread)
{
	(void)thread;
}

int
assert_wait_deadline(void *chan, int interruptible, uint64_t deadline)
{
	(void)chan;
	(void)interruptible;
	now_ns = deadline;
	return 0;
}

/* The watch thread never returns: let it run a set number of rounds. */
#include <setjmp.h>
static jmp_buf watch_out;

int
thread_block(void *continuation)
{
	(void)continuation;
	if (watch_blocks-- <= 0) {
		longjmp(watch_out, 1);
	}
	return 0;
}

static void
run_watch(int rounds)
{
	if (watch_fn == NULL) {
		printf("TEST: no watch thread was started\n");
		return;
	}
	watch_blocks = rounds;
	if (setjmp(watch_out) == 0) {
		watch_fn(NULL, 0);
	}
}

/* ---- the SRAM words the phone printed ---- */
static int
load_sram(const char *log)
{
	FILE *f = fopen(log, "rb");
	char line[512];
	int words = 0;

	if (f == NULL) {
		return -1;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		unsigned long addr;
		unsigned w[4];
		int n = sscanf(line, "A8SH: %lx: %x %x %x %x", &addr, &w[0], &w[1], &w[2], &w[3]);

		if (n < 2 || addr < SRAM_PA || addr >= SRAM_PA + SRAM_SIZE) {
			continue;
		}
		for (int i = 0; i < n - 1; i++) {
			sram[(addr - SRAM_PA) / 4 + i] = w[i];
			words++;
		}
	}
	fclose(f);
	return words;
}

void a8_acpm_command(const char *args);
void a8_dvfs_command(const char *args);
void a8_tmu_command(const char *args);

static void
run(const char *what, void (*fn)(const char *), const char *args)
{
	printf("\n> %s %s\n", what, args);
	fn(args);
}

static void
set_temp(uint32_t *block, int celsius)
{
	/* one-point: code = temp - 25 + error1, error1 = 50 */
	block[0x00 / 4] = 50;
	block[0x20 / 4] = 1;
	block[0x40 / 4] = (uint32_t)(celsius - 25 + 50);
}

int
main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "harness SERIAL.LOG FVP.BIN\n");
		return 2;
	}
	fvp_path = argv[2];
	printf("TEST: %d SRAM words from %s\n", load_sram(argv[1]), argv[1]);
	pmu[0x40a4 / 4] = 0xf;
	cmu_g3d[0x120 / 4] = 0xa1570d11u;
	set_temp(tmu_cpu, 41);
	set_temp(tmu_g3d, 39);

	run("acpm", a8_acpm_command, "");
	run("dvfs", a8_dvfs_command, "table g3d");
	run("dvfs", a8_dvfs_command, "table big");
	run("dvfs", a8_dvfs_command, "table little");
	run("dvfs", a8_dvfs_command, "");
	run("tmu", a8_tmu_command, "");
	run("dvfs", a8_dvfs_command, "g3d 450000");             /* refused: no FVP */
	run("acpm", a8_acpm_command, "send 0 4 0 1 0");         /* times out: no FVP */
	run("acpm", a8_acpm_command, "fvp /nonexistent");
	run("acpm", a8_acpm_command, "fvp /private/var/root/fvp.fw");
	run("acpm", a8_acpm_command, "fvp /private/var/root/fvp.fw");   /* already attached */
	run("acpm", a8_acpm_command, "");
	run("dvfs", a8_dvfs_command, "");
	run("dvfs", a8_dvfs_command, "g3d 460000");             /* not a level */
	run("dvfs", a8_dvfs_command, "g3d 1300000");            /* above the stock limit */
	run("dvfs", a8_dvfs_command, "mif 1794000");            /* not ours */
	run("dvfs", a8_dvfs_command, "g3d 450000");
	run("dvfs", a8_dvfs_command, "g3d 676000");
	run("dvfs", a8_dvfs_command, "big 1768000");
	for (int i = 0; i < 40; i++) {                          /* sequence numbers wrap at 64 */
		a8_acpm_command("send 0 3 0 1 0");
	}
	run_watch(16);                                          /* 32 s: one status line */
	set_temp(tmu_g3d, 83);
	run_watch(2);                                           /* over the limit: back to boot */
	run("dvfs", a8_dvfs_command, "g3d 676000");             /* still hot: refused */
	set_temp(tmu_g3d, 45);
	run_watch(2);
	run("dvfs", a8_dvfs_command, "g3d 676000");
	test_tmu_refuses = true;
	tmu_g3d[0x40 / 4] = 0;                                  /* no reading at all */
	run("tmu", a8_tmu_command, "");
	run_watch(4);                                           /* three misses: back to boot */
	run("dvfs", a8_dvfs_command, "g3d 545000");             /* no temperature: refused */
	run("dvfs", a8_dvfs_command, "boot");
	run("acpm", a8_acpm_command, "log");
	apm_ignores_dvfs = true;
	run("dvfs", a8_dvfs_command, "");                       /* every FREQ_GET times out */
	printf("\nTEST: end; APM rates mif %u int %u big %u little %u g3d %u\n", apm_rate[0],
	    apm_rate[1], apm_rate[2], apm_rate[3], apm_rate[4]);
	return 0;
}
