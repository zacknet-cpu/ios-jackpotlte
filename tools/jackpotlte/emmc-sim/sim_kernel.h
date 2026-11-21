#pragma once
/* Minimal user-space stand-ins for the XNU interfaces a5xelte_emmc.c uses. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <limits.h>
#include <time.h>

#define ARM64_BOARD_CONFIG_A5XELTE 1
#define JACKPOTLTE 1
#define __unused __attribute__((unused))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define NSEC_PER_SEC 1000000000ull
#define NSEC_PER_MSEC 1000000ull
#define NSEC_PER_USEC 1000ull
#define USEC_PER_SEC 1000000ull
#define PZERO 22
#define FWRITE 2
#define B_WRITE 0
#define B_READ 1
#define D_DISK 2
#define DEVFS_BLOCK 0
#define DEVFS_CHAR 1
#define UID_ROOT 0
#define GID_OPERATOR 5
#define kSuccess 0

typedef uintptr_t vm_offset_t;
typedef uintptr_t vm_size_t;
typedef int kern_return_t;
#define KERN_SUCCESS 0
typedef void *thread_t;
typedef int wait_result_t;
typedef void (*thread_continue_t)(void *, wait_result_t);
typedef int sim_dev_t;
#define dev_t sim_dev_t
typedef char *caddr_t;
typedef unsigned long u_long;
typedef void *DTEntry;
struct proc;
struct uio;
struct sim_buf {
	uint32_t count, resid, flags;
	int64_t blkno;
	int error;
	char *data;
	int done;
};
typedef struct sim_buf *buf_t;

enum {
	DKIOCGETBLOCKSIZE = 1, DKIOCSETBLOCKSIZE, DKIOCGETPHYSICALBLOCKSIZE,
	DKIOCGETBLOCKCOUNT, DKIOCISWRITABLE, DKIOCGETMAXBLOCKCOUNTREAD,
	DKIOCGETMAXBLOCKCOUNTWRITE, DKIOCGETMAXSEGMENTCOUNTREAD,
	DKIOCGETMAXSEGMENTCOUNTWRITE, DKIOCSYNCHRONIZE, DKIOCSYNCHRONIZECACHE,
};

struct bdevsw {
	void *d_open, *d_close, *d_strategy, *d_ioctl, *d_dump, *d_psize;
	int d_type;
};
struct cdevsw {
	void *d_open, *d_close, *d_read, *d_write, *d_ioctl, *d_stop, *d_reset,
	    *d_ttys, *d_select, *d_mmap, *d_strategy, *d_reserved_1, *d_reserved_2;
	int d_type;
};
extern int eno_dump(void), eno_stop(void), eno_reset(void), eno_select(void),
    eno_mmap(void), eno_strat(void), eno_getc(void), eno_putc(void);
int physio(void (*)(buf_t), void *, dev_t, int, void *, struct uio *, int);
void minphys(void);
int bdevsw_add(int, const struct bdevsw *);
int cdevsw_add_with_bdev(int, const struct cdevsw *, int);
void *devfs_make_node(dev_t, int, int, int, int, const char *, ...);
#define makedev(a, b) (((a) << 24) | (b))

uint32_t buf_count(buf_t);
void buf_setresid(buf_t, uint32_t);
int64_t buf_blkno(buf_t);
int buf_flags(buf_t);
int buf_map(buf_t, caddr_t *);
int buf_unmap(buf_t);
void buf_seterror(buf_t, int);
void buf_biodone(buf_t);

#define LCK_GRP_DECLARE(n, s) int n
#define LCK_MTX_DECLARE(n, g) int n
void sim_lock(int *);
void sim_unlock(int *);
#define lck_mtx_lock(m) sim_lock(m)
#define lck_mtx_unlock(m) sim_unlock(m)
int tsleep(void *, int, const char *, int);

int PE_parse_boot_argn(const char *, void *, int);
int SecureDTLookupEntry(void *, const char *, DTEntry *);
int SecureDTGetProperty(DTEntry, const char *, void const **, unsigned int *);

uint64_t mach_absolute_time(void);
void clock_interval_to_deadline(uint32_t, uint64_t, uint64_t *);
void absolutetime_to_nanoseconds(uint64_t, uint64_t *);
void nanoseconds_to_absolutetime(uint64_t, uint64_t *);
extern int sim_preempt;
#define disable_preemption() (sim_preempt++)
#define enable_preemption() (sim_preempt--)

#define os_atomic_add_orig(p, v, o) __atomic_fetch_add((p), (v), __ATOMIC_RELAXED)
#define os_atomic_load(p, o) __atomic_load_n((p), __ATOMIC_RELAXED)

uint32_t sim_rd(uint32_t off);
void sim_wr(uint32_t off, uint32_t v);
