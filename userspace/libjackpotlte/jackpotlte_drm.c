/* SPDX-License-Identifier: MIT */
/*
 * jackpotlte_drm: Linux's DRM requests over our kernel's GPU user client.
 *
 * See jackpotlte_drm.h for what this is.  The shape of it is the reference's
 * (hakios/mtk tools/mesa/redwood-drm): one connection in the process, a request
 * number's low byte as the call's selector, and sync objects kept here because
 * a Darwin client has none.  What is different is that the structures are
 * translated instead of passed through, because our kernel reads exactly the
 * 40-byte structures in 0154 and refuses a request larger than that.
 */
#include "jackpotlte_drm.h"
#include "jackpotlte_gpu.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/*
 * Two of Linux's panfrost requests that our kernel does not answer (0154 has
 * no counters).  They are named here so the refusal is explicit and costs no
 * round trip.
 */
#define JT_PERFCNT_ENABLE       0x46
#define JT_PERFCNT_DUMP         0x47

/* drmCommandWrite(index): Linux's DRM_COMMAND_BASE, which libdrm adds. */
#define JT_DRM_COMMAND_BASE     0x40

/* Buffers and sync objects by handle.  Both are per-connection state, and a
 * handle at or past the end simply is not remembered (0154's handles are small;
 * MALI_BUFFERS is 4096). */
#define JT_MAPS                 4096
#define JT_SYNCOBJS             1024

#define JT_REQUEST_SIZE(request) ((size_t) (((request) >> 16) & 0x3ffful))

/*
 * The one GPU and the fd Mesa holds for it.  `device_fd` is /dev/null: the fd
 * means nothing here, but Mesa stores it and hands it back, and closing it must
 * not close the connection, so it is opened once and dup()ed per open.
 */
static struct jt_gpu *gpu;
static int device_fd = -1;
static pthread_mutex_t gpu_lock = PTHREAD_MUTEX_INITIALIZER;

/* A buffer's mapping, kept until the buffer is given back. */
static struct { void *address; uint64_t size; } maps[JT_MAPS];

/*
 * Sync objects, kept in this process over the kernel's job numbers.  A job is
 * submitted synchronously (0154: "every job has finished before its submission
 * returned"), so a sync object is a number to wait for and nothing else: what
 * panfrost needs is that its out_sync means its job, and that waiting on it
 * waits for that job.
 */
static pthread_mutex_t sync_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t syncobj_seq[JT_SYNCOBJS];
static uint64_t syncfile_seq[JT_SYNCOBJS];
static uint64_t last_seq;
static uint32_t next_syncobj = 1;

/*
 * Which job last used each buffer, as the kernel keeps it (0154:527-540), so a
 * WAIT_BO for a buffer whose last job is known to be done is answered here.
 * panfrost asks with a non-blocking WAIT_BO for every cached buffer it reuses
 * (pan_bo.c's cache fetch, mostly transient pool memory), and that round trip
 * was 9% of backboardd's render thread while scrolling Settings (11-25
 * profile, a8gl-prof "2").  done_seq is the highest job number a kernel wait
 * said was finished; jobs finish in order (one queue, 0153), so every buffer
 * whose last job is at or below it is idle.  BO_SEQ_BUSY marks a buffer while
 * a submission naming it is on its way, so nothing says idle for it then.
 * Only "idle" is ever answered here; anything else goes to the kernel.
 */
#define BO_SEQ_BUSY             UINT64_MAX
static uint64_t bo_seq[JT_MAPS];
static uint64_t done_seq;
/* 11-26: the surface a handle came from (JT_SURFACE), 0 for none.  The
 * kernel gives a surface that is imported again the handle it already has
 * (0154 surfaceBuffer), so an import first takes back a queued close of
 * that surface (close_cancel_surface). */
static uint32_t bo_surface[JT_MAPS];

static void
note_done(uint64_t seq)
{
	uint64_t old = __atomic_load_n(&done_seq, __ATOMIC_RELAXED);

	while (seq > old && !__atomic_compare_exchange_n(&done_seq, &old, seq, false,
	    __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
	}
}

static void
bo_seq_set(uint32_t handle, uint64_t seq)
{
	if (handle < JT_MAPS) {
		__atomic_store_n(&bo_seq[handle], seq, __ATOMIC_RELEASE);
	}
}

/*
 * xnu 0225: the kernel's last finished job, live, from the done page
 * (jt_done_page), so "idle" needs no call and, for a look (timeout 0),
 * neither does "busy": jobs finish in order, and bo_seq[] is the same
 * record of each buffer's last job the kernel keeps (0154).  Without the
 * page (an older kernel, or /private/var/mobile/Library/a8gl-donepage "0")
 * done_seq is what kernel waits reported, and only "idle" is answered here.
 */
static int donepage_mode = -1;                  /* -1 not read yet, 0 off, 1 on */

static const volatile uint64_t *
done_page(struct jt_gpu *g)
{
	if (donepage_mode < 0) {
		char c = '1';
		int fd = open("/private/var/mobile/Library/a8gl-donepage", O_RDONLY | O_CLOEXEC);

		if (fd >= 0) {
			if (read(fd, &c, 1) != 1) {
				c = '1';
			}
			close(fd);
		}
		donepage_mode = c == '0' ? 0 : 1;
	}
	return donepage_mode > 0 ? jt_done_page(g) : NULL;
}

/* The highest job known to be finished; *live: it came from the page. */
static uint64_t
done_now(struct jt_gpu *g, bool *live)
{
	const volatile uint64_t *page = done_page(g);
	uint64_t done = __atomic_load_n(&done_seq, __ATOMIC_ACQUIRE);

	*live = page != NULL;
	if (page != NULL) {
		uint64_t now = __atomic_load_n(&page[0], __ATOMIC_ACQUIRE);

		if (now > done) {
			note_done(now);
			done = now;
		}
	}
	return done;
}

/* A kernel wait for job `seq`; a success says every job up to it is done. */
static void held_flush(struct jt_gpu *g);     /* 0218 */
static void held_new_kernel(void);              /* 0230 */

static int
wait_seq_noted(struct jt_gpu *g, uint64_t seq, int64_t timeout_ns)
{
	int ret;

	{
		bool live;

		if (seq <= done_now(g, &live)) {
			return 0;               /* done: no call (0225 page, or an earlier wait) */
		}
		if (live && timeout_ns == 0) {
			return -ETIMEDOUT;      /* a look, and the page says not yet */
		}
	}
	held_flush(g);                  /* 0218/0230: never wait behind held submits */
	ret = jt_wait_seq(g, seq, timeout_ns);

	if (ret == 0) {
		note_done(seq);
	}
	return ret;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * The connection.
 */
struct jt_gpu *
jackpotlte_drm_gpu(void)
{
	struct jt_gpu *g;

	pthread_mutex_lock(&gpu_lock);
	if (gpu == NULL) {
#ifdef JACKPOTLTE_DRM_IOS
		struct jt_gpu *opened = NULL;

		if (jt_iokit_open(&opened) == 0) {
			gpu = opened;
		}
#endif
	}
	g = gpu;
	pthread_mutex_unlock(&gpu_lock);
	return g;
}

void
jackpotlte_drm_attach(struct jt_gpu *g)
{
	struct jt_gpu *old;

	held_flush(gpu);                /* 0230: the old connection's held submits go to it */
	held_new_kernel();
	jackpotlte_drm_close_drain();   /* the queued closes belong to the old one */
	pthread_mutex_lock(&gpu_lock);
	old = gpu;
	gpu = g;
	for (uint32_t i = 0; i < JT_MAPS; i++) {
		if (maps[i].address == NULL) {
			continue;
		}
		/* The previous connection's mappings go with it. */
		if (old != NULL && old != g) {
			(void)jt_bo_unmap(old, i, maps[i].address, maps[i].size);
		}
		maps[i].address = NULL;
		maps[i].size = 0;
	}
	pthread_mutex_unlock(&gpu_lock);

	pthread_mutex_lock(&sync_lock);
	memset(syncobj_seq, 0, sizeof(syncobj_seq));
	memset(syncfile_seq, 0, sizeof(syncfile_seq));
	last_seq = 0;
	next_syncobj = 1;
	/* A new connection numbers its jobs from 1 again. */
	for (uint32_t i = 0; i < JT_MAPS; i++) {
		bo_seq[i] = 0;
		bo_surface[i] = 0;
	}
	__atomic_store_n(&done_seq, 0, __ATOMIC_RELEASE);
	pthread_mutex_unlock(&sync_lock);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * The mappings.  A buffer's memory is mapped by its handle (0154's
 * clientMemoryForType), and unmapped only when the buffer goes, so panfrost's
 * map/unmap of every use of its cache does not call the kernel each time.
 */
static void
maps_forget(uint32_t handle, void **address, uint64_t *size)
{
	*address = NULL;
	*size = 0;
	if (handle >= JT_MAPS) {
		return;
	}
	pthread_mutex_lock(&gpu_lock);
	*address = maps[handle].address;
	*size = maps[handle].size;
	maps[handle].address = NULL;
	maps[handle].size = 0;
	pthread_mutex_unlock(&gpu_lock);
}

void *
jackpotlte_drm_map(int fd, uint32_t handle, uint64_t size)
{
	struct jt_gpu *g = jackpotlte_drm_gpu();
	void *address = NULL;
	uint64_t mapped = 0;
	int ret;

	(void)fd;
	if (g == NULL) {
		errno = ENODEV;
		return MAP_FAILED;
	}
	pthread_mutex_lock(&gpu_lock);
	if (handle < JT_MAPS && maps[handle].address != NULL && maps[handle].size >= size) {
		address = maps[handle].address;
		pthread_mutex_unlock(&gpu_lock);
		return address;
	}
	if (handle < JT_MAPS && maps[handle].address != NULL) {
		/* A smaller mapping than is wanted: give it back and map again. */
		(void)jt_bo_unmap(g, handle, maps[handle].address, maps[handle].size);
		maps[handle].address = NULL;
		maps[handle].size = 0;
	}
	ret = jt_bo_map(g, handle, &address, &mapped);
	if (ret == 0 && handle < JT_MAPS) {
		maps[handle].address = address;
		maps[handle].size = mapped;
	}
	pthread_mutex_unlock(&gpu_lock);
	if (ret != 0) {
		errno = -ret;
		return MAP_FAILED;
	}
	return address;
}

void
jackpotlte_drm_unmap(int fd, uint32_t handle, void *address)
{
	struct jt_gpu *g;

	(void)fd;
	if (handle < JT_MAPS) {
		return;         /* kept for the next map: maps_forget at GEM_CLOSE */
	}
	g = jackpotlte_drm_gpu();
	if (g != NULL && address != NULL) {
		(void)jt_bo_unmap(g, handle, address, 0);
	}
}

/*
 * 11-26 (P3c): buffers given back on a thread of their own.  The kernel's
 * GEM_CLOSE (0154 freeBuffer) first waits until the buffer's last job is
 * done, then takes its GPU mapping out and frees its memory, and the CPU
 * unmap before it is another kernel call.  Mesa gives buffers back in
 * panfrost_batch_cleanup, right after their batch is submitted, so the
 * render thread waited for the GPU there: 15% of backboardd's render thread
 * while opening and closing an app (11-26 profile, jt_bo_release).  Here the
 * bookkeeping stays on the caller's thread and the two kernel calls go to a
 * queue served in order by one thread, which first waits for the buffer
 * with WAIT_BO (no lock held in the kernel) so that GEM_CLOSE does not wait
 * under the user client's lock.  The kernel keeps the handle taken until the
 * close really happens, so nothing can be given the same handle before,
 * except a surface imported again: the import takes the queued close back
 * first (close_cancel_surface) or waits for the one being done.
 * /private/var/mobile/Library/a8gl-closer "0": closes as before.
 */
#define JT_CLOSE_QUEUE          2048
static struct {
	struct jt_gpu *g;
	uint32_t handle;
	uint32_t surface;               /* the surface it came from, 0 for none */
	void *address;
	uint64_t size;
	uint64_t seq;                   /* its bo_seq[] before the close, put back if taken back */
} close_q[JT_CLOSE_QUEUE];
static unsigned close_head, close_count, close_busy;
static uint32_t close_busy_surface;
static pthread_mutex_t close_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t close_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t close_done = PTHREAD_COND_INITIALIZER;
static int closer_state;                /* 0 unknown, 1 running, -1 off */

static void *
closer_main(void *arg)
{
	(void)arg;
	for (;;) {
		struct jt_gpu *g;
		uint32_t handle;
		void *address;
		uint64_t size;

		pthread_mutex_lock(&close_lock);
		while (close_count == 0) {
			pthread_cond_wait(&close_cond, &close_lock);
		}
		g = close_q[close_head].g;
		handle = close_q[close_head].handle;
		address = close_q[close_head].address;
		size = close_q[close_head].size;
		close_busy_surface = close_q[close_head].surface;
		close_head = (close_head + 1) % JT_CLOSE_QUEUE;
		close_count--;
		close_busy = 1;
		pthread_mutex_unlock(&close_lock);
		/* The kernel's GEM_CLOSE waits for the buffer's last job with the
		 * user client's lock held (0154 freeBuffer), which would hold the
		 * render thread's next submit too; WAIT_BO waits without it. */
		(void)jt_wait_bo(g, handle, 2000000000ll);
		if (address != NULL) {
			(void)jt_bo_unmap(g, handle, address, size);
		}
		(void)jt_bo_release(g, handle);
		pthread_mutex_lock(&close_lock);
		close_busy = 0;
		close_busy_surface = 0;
		pthread_cond_broadcast(&close_done);
		pthread_mutex_unlock(&close_lock);
	}
	return NULL;
}

/* Every queued close done (a new connection, and the host test). */
void
jackpotlte_drm_close_drain(void)
{
	pthread_mutex_lock(&close_lock);
	while (close_count != 0 || close_busy) {
		pthread_cond_wait(&close_done, &close_lock);
	}
	pthread_mutex_unlock(&close_lock);
}

/* true when the close was queued (the caller is done) */
static bool
close_later(struct jt_gpu *g, uint32_t handle, uint32_t surface, void *address, uint64_t size,
    uint64_t seq)
{
	bool queued = false;

	pthread_mutex_lock(&close_lock);
	if (closer_state == 0) {
		FILE *f = fopen("/private/var/mobile/Library/a8gl-closer", "r");
		int c = f != NULL ? fgetc(f) : EOF;
		pthread_t thread;

		if (f != NULL) {
			fclose(f);
		}
		closer_state = -1;
		if (c != '0' && pthread_create(&thread, NULL, closer_main, NULL) == 0) {
			pthread_detach(thread);
			closer_state = 1;
		}
	}
	if (closer_state == 1 && close_count < JT_CLOSE_QUEUE) {
		unsigned at = (close_head + close_count) % JT_CLOSE_QUEUE;

		close_q[at].g = g;
		close_q[at].handle = handle;
		close_q[at].surface = surface;
		close_q[at].address = address;
		close_q[at].size = size;
		close_q[at].seq = seq;
		close_count++;
		queued = true;
		pthread_cond_signal(&close_cond);
	}
	pthread_mutex_unlock(&close_lock);
	return queued;
}

/* Give a buffer back: its mapping first, then the buffer and its address. */
static int
buffer_close(struct jt_gpu *g, uint32_t handle)
{
	void *address = NULL;
	uint64_t size = 0;
	uint64_t last;

	if (handle == 0) {
		return -EINVAL;
	}
	last = handle < JT_MAPS ? __atomic_load_n(&bo_seq[handle], __ATOMIC_ACQUIRE) : BO_SEQ_BUSY;

	maps_forget(handle, &address, &size);
	bo_seq_set(handle, 0);          /* a new buffer under this handle: no job yet */
	if (close_later(g, handle, handle < JT_MAPS ? __atomic_exchange_n(&bo_surface[handle], 0, __ATOMIC_ACQ_REL) : 0,
	    address, size, last)) {
		return 0;
	}
	if (address != NULL) {
		(void)jt_bo_unmap(g, handle, address, size);
	}
	return jt_bo_release(g, handle);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * The requests, one function each.  Each reads the fields the kernel reads and
 * writes back the ones it answers; 0154 is the authority for both.
 */
static int
gem_close(struct jt_gpu *g, void *arg, size_t size)
{
	struct jt_gem_close *request = arg;

	if (arg == NULL || size < sizeof(*request)) {
		return -EINVAL;
	}
	return buffer_close(g, request->handle);
}

/*
 * 0218 (kernel 0218, selector 0x7f): one kernel call per render pass.  Mesa
 * submits a batch's vertex/tiler chain with no sync object when a fragment
 * chain follows (pan_jm.c, jm_submit_batch), then the fragment chain right
 * after; each was an IOKit round trip (Settings ~22 passes a frame, Control
 * Center ~39).
 *
 * 0230 (kernel 0230, selector 0x7c): several passes in one call.  Submits are
 * held, each with a copy of its buffer list (Mesa frees its own once the
 * ioctl returns), and go to the kernel together, in order, when
 *   - the held passes (fragment submits) reach a8gl-batch's number (default
 *     4; "1" = one pass a call, as 0218; up to 8), or the list is full;
 *   - a submit with input sync objects comes (it goes after them);
 *   - something must see them run: a real wait (WAIT_BO or a job wait that
 *     will block), a GEM_CLOSE, a surface request, a sync-object call while a
 *     held submit signals one, and above all the end of every panfrost_flush
 *     (jackpotlte_drm_flush_end, tools/mesa-jackpotlte/0007): a glFlush that
 *     ends a frame is followed by the flip with no fence to wait on.
 * Until panfrost_flush has called jackpotlte_drm_flush_end once (a Mesa
 * without 0007), nothing is held beyond one pass and every request other than
 * a submit sends what is held first: 0218, unchanged.
 * Every buffer of a held submit is marked busy (BO_SEQ_BUSY) from the moment
 * it is held, so a WAIT_BO look (timeout 0) at one answers "busy" without a
 * call, and a real wait sends the held submits first: no buffer a held job
 * uses is ever reported idle, reused or freed under it.  MADVISE, CREATE_BO,
 * MMAP_BO, GET_PARAM and GET_BO_OFFSET cannot touch a held job's buffers and
 * leave the list alone.  A kernel older than 0230 refuses the selector: pairs
 * (0218) or single submits from then on.  a8gl-pair "0": nothing is held.
 */
#define JT_HELD_MAX             JT_SUBMITN_MAX  /* = the kernel's JACKPOTLTE_SUBMITN_MAX */
static pthread_mutex_t held_lock = PTHREAD_MUTEX_INITIALIZER;
static struct jt_submit_req held_req[JT_HELD_MAX];
static uint32_t *held_handles[JT_HELD_MAX];
static uint32_t held_cap[JT_HELD_MAX];
static unsigned held_n;                         /* submits held (atomic reads outside the lock) */
static unsigned held_passes;                    /* fragment submits among them */
static bool held_syncs;                         /* one of them signals a sync object */
static int pair_mode = -1;                      /* -1 not read yet, 0 off, 1 on */
static int batch_passes = -1;                   /* 0230: passes a call; -1 not read yet */
static bool submitn_ok = true;                  /* 0230: false once the kernel refuses 0x7c */
static bool submit2_ok = true;                  /* 0218: false once the kernel refuses 0x7f */
static uint64_t pairs_sent, batches_sent, batch_jobs_sent;
static bool flush_end_seen;                     /* 0230: panfrost_flush calls jackpotlte_drm_flush_end */

static bool
pair_on(void)
{
	if (pair_mode < 0) {
		char c = '1';
		int fd = open("/private/var/mobile/Library/a8gl-pair", O_RDONLY | O_CLOEXEC);

		if (fd >= 0) {
			if (read(fd, &c, 1) != 1) {
				c = '1';
			}
			close(fd);
		}
		pair_mode = c == '0' ? 0 : 1;
	}
	return pair_mode > 0;
}

/* 0230: a8gl-batch, a digit 1-8: passes sent in one call (default 4) */
static unsigned
batch_on(void)
{
	if (batch_passes < 0) {
		char c = '4';
		int fd = open("/private/var/mobile/Library/a8gl-batch", O_RDONLY | O_CLOEXEC);

		if (fd >= 0) {
			if (read(fd, &c, 1) != 1) {
				c = '4';
			}
			close(fd);
		}
		batch_passes = (c >= '1' && c <= '8') ? c - '0' : 4;
	}
	return (unsigned) batch_passes;
}

/* The bookkeeping of a submit the kernel took (seq) or did not (0). */
static void
submit_done(const struct jt_submit_req *request, const uint32_t *handles, uint64_t seq)
{
	if (seq == 0) {
		/* Not queued: the buffers keep a busy mark, and their WAIT_BOs go to
		 * the kernel, which knows what really ran. */
		return;
	}
	for (uint32_t i = 0; handles != NULL && i < request->bo_handle_count; i++) {
		bo_seq_set(handles[i], seq);
	}
	__atomic_store_n(&last_seq, seq, __ATOMIC_RELEASE);
	if (request->out_sync != 0) {
		pthread_mutex_lock(&sync_lock);
		syncobj_seq[request->out_sync % JT_SYNCOBJS] = seq;
		pthread_mutex_unlock(&sync_lock);
	}
}

/* Queue one request now; its job's number, or 0 with *error set. */
static uint64_t
submit_now(struct jt_gpu *g, const struct jt_submit_req *request, const uint32_t *handles, int *error)
{
	/* The kernel copyin()s the handle array itself, so only its address
	 * travels (0154:527-540).  It reads just the fragment bit of
	 * `requirements`. */
	return jt_submit(g, request->jc, (request->requirements & JT_JD_REQ_FS) != 0,
	    request->bo_handle_count != 0 ? handles : NULL, request->bo_handle_count, error);
}

/* 0230: a new connection may take what an old one refused */
static void
held_new_kernel(void)
{
	pthread_mutex_lock(&held_lock);
	submitn_ok = true;
	submit2_ok = true;
	pthread_mutex_unlock(&held_lock);
}

/* Send the held submits, in order (held_lock held). */
static void
held_flush_locked(struct jt_gpu *g)
{
	unsigned n = held_n, k = 0;

	if (n == 0) {
		return;
	}
	if (n >= 3 && submitn_ok) {
		int error = jt_submitn(g, held_req, n);

		if (error == 0) {
			/* each request's jc is now its job's number (0: not queued) */
			for (k = 0; k < n; k++) {
				submit_done(&held_req[k], held_handles[k], held_req[k].jc);
			}
			batches_sent++;
			batch_jobs_sent += n;
			k = n;
		} else if (error == -ENOTTY || error == -EINVAL) {
			/* a kernel older than 0230 (unknown selector, or a structure
			 * larger than its own): pairs from now on */
			submitn_ok = false;
		}
		/* any other refusal (not ready): this time as pairs or singles */
	}
	while (k < n) {
		bool pair = k + 1 < n && submit2_ok &&
		    (held_req[k].requirements & JT_JD_REQ_FS) == 0 &&
		    (held_req[k + 1].requirements & JT_JD_REQ_FS) != 0;

		if (pair) {
			struct jt_submit_req two[2];

			two[0] = held_req[k];
			two[1] = held_req[k + 1];
			int error = jt_submit2(g, two);

			if (error == 0) {
				submit_done(&held_req[k], held_handles[k], two[0].jc);
				submit_done(&held_req[k + 1], held_handles[k + 1], two[1].jc);
				pairs_sent++;
				k += 2;
				continue;
			}
			if (error == -ENOTTY || error == -EINVAL) {
				submit2_ok = false;     /* older than 0218: one at a time */
			}
		}
		{
			int error = 0;
			uint64_t seq = submit_now(g, &held_req[k], held_handles[k], &error);

			submit_done(&held_req[k], held_handles[k], seq);
			k++;
		}
	}
	held_passes = 0;
	held_syncs = false;
	__atomic_store_n(&held_n, 0, __ATOMIC_RELEASE);
}

static void
held_flush(struct jt_gpu *g)
{
	if (__atomic_load_n(&held_n, __ATOMIC_ACQUIRE) == 0 || g == NULL) {
		return;
	}
	pthread_mutex_lock(&held_lock);
	held_flush_locked(g);
	pthread_mutex_unlock(&held_lock);
}

/* 0230: a sync-object call reads or writes what a held submit will signal:
 * the held submits go first, so the order stays the one Mesa asked for */
static void
held_flush_syncs(void)
{
	if (__atomic_load_n(&held_n, __ATOMIC_ACQUIRE) == 0) {
		return;
	}
	pthread_mutex_lock(&held_lock);
	if (held_syncs) {
		held_flush_locked(jackpotlte_drm_gpu());
	}
	pthread_mutex_unlock(&held_lock);
}

/* Hold one submit with a copy of its buffer list; false: no room (held_lock held) */
static bool
held_add_locked(const struct jt_submit_req *request, const uint32_t *handles)
{
	unsigned k = held_n;
	uint32_t n = request->bo_handle_count;

	if (k >= JT_HELD_MAX || (n != 0 && handles == NULL)) {
		return false;
	}
	if (n > held_cap[k]) {
		uint32_t *grown = realloc(held_handles[k], (size_t) n * sizeof(uint32_t));

		if (grown == NULL) {
			return false;
		}
		held_handles[k] = grown;
		held_cap[k] = n;
	}
	if (n != 0) {
		memcpy(held_handles[k], handles, (size_t) n * sizeof(uint32_t));
	}
	held_req[k] = *request;
	held_req[k].bo_handles = (uint64_t) (uintptr_t) held_handles[k];
	if ((request->requirements & JT_JD_REQ_FS) != 0) {
		held_passes++;
	}
	if (request->out_sync != 0) {
		held_syncs = true;
	}
	__atomic_store_n(&held_n, k + 1, __ATOMIC_RELEASE);
	return true;
}

static int
submit(struct jt_gpu *g, void *arg, size_t size)
{
	struct jt_submit_req *request = arg;    /* Linux's struct's first 40 bytes */
	const uint32_t *handles;
	uint64_t seq;
	int error = 0;

	if (arg == NULL || size < sizeof(*request)) {
		return -EINVAL;
	}
	handles = (const uint32_t *) (uintptr_t) request->bo_handles;
	if (handles != NULL) {
		for (uint32_t i = 0; i < request->bo_handle_count; i++) {
			bo_seq_set(handles[i], BO_SEQ_BUSY);
		}
	}
	pthread_mutex_lock(&held_lock);
	{
		bool fragment = (request->requirements & JT_JD_REQ_FS) != 0;
		bool batching = __atomic_load_n(&flush_end_seen, __ATOMIC_ACQUIRE);
		/* without 0007's flush end, 0218: hold a vertex/tiler submit with no
		 * sync object, and its fragment submit only to send the pair */
		bool holdable = batching ? true :
		    (fragment ? held_n != 0 : request->out_sync == 0);

		if (request->jc != 0 && request->in_sync_count == 0 && pair_on() && holdable &&
		    held_add_locked(request, handles)) {
			/* a pass is complete with its fragment submit: send when enough are */
			if (held_passes >= (batching ? batch_on() : 1u) || held_n + 2 > JT_HELD_MAX) {
				held_flush_locked(g);
			}
			pthread_mutex_unlock(&held_lock);
			return 0;       /* queued with the others */
		}
	}
	held_flush_locked(g);           /* whatever is held goes first, in order */
	pthread_mutex_unlock(&held_lock);
	seq = submit_now(g, request, handles, &error);
	submit_done(request, handles, seq);
	if (seq == 0) {
		return error != 0 ? error : -EIO;
	}
	request->jc = seq;
	return 0;
}

static int
wait_bo(struct jt_gpu *g, void *arg, size_t size)
{
	struct jt_wait_bo_req *request = arg;
	uint64_t seq = BO_SEQ_BUSY;
	int ret;

	if (arg == NULL || size < sizeof(*request)) {
		return -EINVAL;
	}
	if (request->handle < JT_MAPS) {
		seq = __atomic_load_n(&bo_seq[request->handle], __ATOMIC_ACQUIRE);
		if (seq != BO_SEQ_BUSY) {
			bool live;

			if (seq <= done_now(g, &live)) {
				return 0;       /* its last job is done: idle, no round trip */
			}
			if (live && request->timeout_ns == 0) {
				return -ETIMEDOUT;      /* 0225: a look, and its job is not done yet */
			}
		} else if (request->timeout_ns == 0 &&
		    __atomic_load_n(&flush_end_seen, __ATOMIC_ACQUIRE) &&
		    __atomic_load_n(&held_n, __ATOMIC_ACQUIRE) != 0) {
			/* 0230: marked busy and submits are held (it may be in one):
			 * busy, without sending them or a call */
			return -ETIMEDOUT;
		}
	}
	held_flush(g);                  /* 0230: what the kernel answers covers every held submit */
	ret = jt_wait_bo(g, request->handle, request->timeout_ns);
	if (ret == 0 && seq != BO_SEQ_BUSY) {
		note_done(seq);
	}
	return ret;
}

static int
create_bo(struct jt_gpu *g, void *arg, size_t size)
{
	struct jt_create_bo_req *request = arg;
	uint32_t handle = 0;
	uint64_t address = 0;
	int ret;

	if (arg == NULL || size < sizeof(*request)) {
		return -EINVAL;
	}
	ret = jt_bo_create(g, request->size, &handle, &address);
	if (ret != 0) {
		return ret;
	}
	request->handle = handle;
	request->offset = address;
	return 0;
}

static int
mmap_bo(void *arg, size_t size)
{
	struct jt_mmap_bo_req *request = arg;

	if (arg == NULL || size < sizeof(*request)) {
		return -EINVAL;
	}
	/* Darwin maps a buffer by its handle, not by an offset.  0154 answers
	 * Linux's offset anyway - the handle shifted 12 (0154:505-508) - and
	 * nothing here hands it to mmap(): jackpotlte_drm_map does the mapping. */
	request->offset = (uint64_t) request->handle << 12;
	return 0;
}

static int
get_param(struct jt_gpu *g, void *arg, size_t size)
{
	struct jt_get_param_req *request = arg;
	uint64_t value = 0;
	int ret;

	if (arg == NULL || size < sizeof(*request)) {
		return -EINVAL;
	}
	ret = jt_get_param(g, request->param, &value);
	if (ret != 0) {
		return ret;
	}
	request->value = value;
	return 0;
}

static int
get_bo_offset(struct jt_gpu *g, void *arg, size_t size)
{
	struct jt_get_bo_offset_req *request = arg;
	uint64_t address = 0;
	int ret;

	if (arg == NULL || size < sizeof(*request)) {
		return -EINVAL;
	}
	ret = jt_bo_offset(g, request->handle, &address);
	if (ret != 0) {
		return ret;
	}
	request->offset = address;
	return 0;
}

static int
bo_madvise(struct jt_gpu *g, void *arg, size_t size)
{
	struct jt_madvise_req *request = arg;

	if (arg == NULL || size < sizeof(*request)) {
		return -EINVAL;
	}
	/* The kernel keeps every buffer and answers retained = 1 to every call
	 * (0154:551), so the answer is given here without the round trip: it was
	 * 5% of backboardd's render thread while scrolling (11-24 profile; the
	 * shim's glca17b patched the same thing into the binary). */
	(void)g;
	request->retained = 1;
	return 0;
}

static int
wait_seq(struct jt_gpu *g, void *arg, size_t size)
{
	struct jt_wait_req *request = arg;

	if (arg == NULL || size < sizeof(*request)) {
		return -EINVAL;
	}
	return wait_seq_noted(g, request->seq, request->timeout_ns);
}

/*
 * Before a surface is imported: the kernel would hand this import the
 * handle of a buffer of that surface whose close is still queued, so such a
 * close is taken back (the buffer stays, and its CPU mapping goes back into
 * maps[] for the next map), and one the closer thread is doing now is waited
 * for (after it the kernel makes a new buffer).
 */
static void
close_cancel_surface(uint32_t surface)
{
	unsigned kept = 0;

	if (surface == 0) {
		return;
	}
	pthread_mutex_lock(&close_lock);
	while (close_busy && close_busy_surface == surface) {
		pthread_cond_wait(&close_done, &close_lock);
	}
	for (unsigned i = 0; i < close_count; i++) {
		unsigned at = (close_head + i) % JT_CLOSE_QUEUE;

		if (close_q[at].surface == surface) {
			uint32_t handle = close_q[at].handle;

			/* the buffer stays, with the job it was last in (the
			 * kernel's record still names it): not "idle" until then */
			bo_seq_set(handle, close_q[at].seq);
			if (close_q[at].address != NULL && handle < JT_MAPS) {
				pthread_mutex_lock(&gpu_lock);
				if (maps[handle].address == NULL) {
					maps[handle].address = close_q[at].address;
					maps[handle].size = close_q[at].size;
				}
				pthread_mutex_unlock(&gpu_lock);
			}
			continue;
		}
		if (kept != i) {
			close_q[(close_head + kept) % JT_CLOSE_QUEUE] = close_q[at];
		}
		kept++;
	}
	close_count = kept;
	pthread_mutex_unlock(&close_lock);
}

static int
surface(struct jt_gpu *g, void *arg, size_t size)
{
	struct jackpotlte_drm_surface *request = arg;
	struct jt_surface_req info;
	uint32_t handle = 0;
	int ret;

	if (arg == NULL || size < sizeof(*request)) {
		return -EINVAL;
	}
	handle = request->handle;
	if (request->handle != JACKPOTLTE_DRM_DESCRIBE) {
		close_cancel_surface(request->surface);
	}
	ret = jt_surface_buffer(g, request->surface,
	    request->handle == JACKPOTLTE_DRM_DESCRIBE ? NULL : &handle, &info);
	if (ret != 0) {
		return ret;
	}
	request->handle = info.handle;
	if (info.handle != 0 && info.handle < JT_MAPS) {
		__atomic_store_n(&bo_surface[info.handle], request->surface, __ATOMIC_RELEASE);
	}
	request->size = info.size;
	request->offset = info.offset;
	request->row_bytes = info.rowBytes;
	request->width = info.width;
	request->height = info.height;
	request->pixel_format = info.pixelFormat;
	request->planes = info.planes;
	return 0;
}

int
jackpotlte_drm_ioctl(struct jt_gpu *g, unsigned long request, void *arg, size_t size)
{
	unsigned int number = (unsigned int) (request & 0xff);

	if (g == NULL) {
		return -ENODEV;
	}
	switch (number) {
	case JT_PANFROST_SUBMIT:        /* holds or sends, in order */
		break;
	case JT_PANFROST_WAIT_BO:       /* sends them itself when it really waits */
	case JT_PANFROST_CREATE_BO:     /* 0230: none of these touches a held job's buffers */
	case JT_PANFROST_MMAP_BO:
	case JT_PANFROST_GET_PARAM:
	case JT_PANFROST_GET_BO_OFFSET:
	case JT_PANFROST_MADVISE:
		if (__atomic_load_n(&flush_end_seen, __ATOMIC_ACQUIRE)) {
			break;
		}
		held_flush(g);          /* 0218 (no flush end seen): held submits go first */
		break;
	default:
		held_flush(g);          /* 0218: GEM_CLOSE, waits, surfaces: held submits go first */
		break;
	}
	switch (number) {
	case JT_DRM_GEM_CLOSE:          return gem_close(g, arg, size);
	case JT_PANFROST_SUBMIT:        return submit(g, arg, size);
	case JT_PANFROST_WAIT_BO:       return wait_bo(g, arg, size);
	case JT_PANFROST_CREATE_BO:     return create_bo(g, arg, size);
	case JT_PANFROST_MMAP_BO:       return mmap_bo(arg, size);
	case JT_PANFROST_GET_PARAM:     return get_param(g, arg, size);
	case JT_PANFROST_GET_BO_OFFSET: return get_bo_offset(g, arg, size);
	case JT_PANFROST_MADVISE:       return bo_madvise(g, arg, size);
	case JT_SURFACE:                return surface(g, arg, size);
	case JT_WAIT:                   return wait_seq(g, arg, size);
	case JT_PERFCNT_ENABLE:         /* our driver does not count, */
	case JT_PERFCNT_DUMP:           /* and has nothing to dump */
	case 0x0a:                      /* JM_CTX_CREATE: the kernel has one context */
	case 0x0b:                      /* JM_CTX_DESTROY: and it lives as long as the client */
		return -ENOTTY;
	default:
		return -ENOTTY;
	}
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * What Mesa calls.
 */
int
drmOpenWithType(const char *name, const char *busid, int type)
{
	int fd;

	(void)busid;
	(void)type;
	if (name == NULL || strcmp(name, "panfrost") != 0) {
		/* One GPU, and it is a Mali. */
		errno = ENODEV;
		return -1;
	}
	if (jackpotlte_drm_gpu() == NULL) {
		errno = ENODEV;         /* no service: the GPU did not come up */
		return -1;
	}
	pthread_mutex_lock(&gpu_lock);
	if (device_fd < 0) {
		device_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
	}
	fd = device_fd >= 0 ? dup(device_fd) : -1;
	pthread_mutex_unlock(&gpu_lock);
	return fd;
}

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	int error;

	(void)fd;
	error = jackpotlte_drm_ioctl(jackpotlte_drm_gpu(), request, arg,
	    JT_REQUEST_SIZE(request));
	if (error != 0) {
		errno = -error;
		return -1;
	}
	return 0;
}

/*
 * libdrm's drmCommandWrite(index): `index` is the command's number without
 * DRM_COMMAND_BASE, and the size is the structure's.  Returns -errno, as
 * libdrm does (not -1).
 */
static int
command(unsigned long index, void *data, unsigned long size)
{
	return jackpotlte_drm_ioctl(jackpotlte_drm_gpu(),
	    JACKPOTLTE_DRM_REQUEST(JT_DRM_COMMAND_BASE + index, size), data, (size_t) size);
}

int
drmCommandWrite(int fd, unsigned long index, const void *data, unsigned long size)
{
	(void)fd;
	return command(index, (void *) (uintptr_t) data, size);
}

int
drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
	(void)fd;
	return command(index, data, size);
}

/*
 * The driver calls itself what Linux's is called, and is of the version whose
 * interface it has: 1.2, buffers at addresses the kernel chooses and tells
 * ("softpin"), nothing newer.
 */
drmVersionPtr
drmGetVersion(int fd)
{
	drmVersionPtr version = calloc(1, sizeof(*version));

	(void)fd;
	if (version == NULL) {
		return NULL;
	}
	version->version_major = 1;
	version->version_minor = 2;
	version->name = strdup("panfrost");
	version->date = strdup("");
	version->desc = strdup("Mali on Darwin");
	if (version->name == NULL || version->date == NULL || version->desc == NULL) {
		drmFreeVersion(version);
		return NULL;
	}
	version->name_len = (int) strlen(version->name);
	version->date_len = (int) strlen(version->date);
	version->desc_len = (int) strlen(version->desc);
	return version;
}

void
drmFreeVersion(drmVersionPtr version)
{
	if (version == NULL) {
		return;
	}
	free(version->name);
	free(version->date);
	free(version->desc);
	free(version);
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	(void)fd;
	(void)capability;
	if (value != NULL) {
		*value = 0;
	}
	return 0;
}

/* A SweetSurface's pixels as a buffer, or only what they would be like. */
int
jackpotlte_drm_surface(int fd, struct jackpotlte_drm_surface *surface)
{
	int error;

	(void)fd;
	if (surface == NULL) {
		return -EINVAL;
	}
	error = jackpotlte_drm_ioctl(jackpotlte_drm_gpu(),
	    JACKPOTLTE_DRM_REQUEST(JT_SURFACE, sizeof(*surface)), surface, sizeof(*surface));
	if (error != 0) {
		errno = -error;
		return -1;
	}
	return 0;
}

/* panfrost imports a surface as a dmabuf: its ID is the "descriptor". */
int
drmPrimeFDToHandle(int fd, int surface, uint32_t *handle)
{
	struct jackpotlte_drm_surface request;
	int error;

	(void)fd;
	if (handle == NULL) {
		return -EINVAL;
	}
	memset(&request, 0, sizeof(request));
	request.surface = (uint32_t) surface;
	error = jackpotlte_drm_ioctl(jackpotlte_drm_gpu(),
	    JACKPOTLTE_DRM_REQUEST(JT_SURFACE, sizeof(request)), &request, sizeof(request));
	if (error != 0) {
		errno = -error;
		return -1;
	}
	*handle = request.handle;
	return 0;
}

/* Buffers as descriptors, for handing one to another program or driver: none. */
int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
	(void)fd;
	(void)handle;
	(void)flags;
	(void)prime_fd;
	errno = ENOSYS;
	return -1;
}

int
drmCloseBufferHandle(int fd, uint32_t handle)
{
	struct jt_gem_close request;

	(void)fd;
	memset(&request, 0, sizeof(request));
	request.handle = handle;
	return jackpotlte_drm_ioctl(jackpotlte_drm_gpu(),
	    JACKPOTLTE_DRM_REQUEST(JT_DRM_GEM_CLOSE, sizeof(request)), &request, sizeof(request));
}

void
jackpotlte_drm_flush_end(void)
{
	__atomic_store_n(&flush_end_seen, true, __ATOMIC_RELEASE);
	held_flush(jackpotlte_drm_gpu());
}

uint64_t
jackpotlte_drm_last_seq(void)
{
	held_flush(jackpotlte_drm_gpu());       /* 0218: the frame's last job, really sent */
	return __atomic_load_n(&last_seq, __ATOMIC_ACQUIRE);
}

int
jackpotlte_drm_wait_seq(uint64_t seq, int64_t relative_ns)
{
	struct jt_gpu *g;

	if (seq == 0) {
		return 0;               /* nothing was submitted: nothing to wait for */
	}
	g = jackpotlte_drm_gpu();
	if (g == NULL) {
		return -ENODEV;
	}
	return wait_seq_noted(g, seq, relative_ns);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Sync objects.  A job finishes before its submission returns, so every one of
 * them is signalled by the time it can be asked about; each is the number of
 * the job it was signalled with, and waiting is the kernel's wait for it.
 */
static int
syncobj_none(void)
{
	errno = ENOSYS;
	return -ENOSYS;
}

int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	(void)fd;
	(void)flags;
	if (handle == NULL) {
		return -EINVAL;
	}
	pthread_mutex_lock(&sync_lock);
	*handle = next_syncobj;
	if (++next_syncobj == 0) {
		next_syncobj = 1;       /* 0 means "not a sync object" */
	}
	syncobj_seq[*handle % JT_SYNCOBJS] = 0;
	pthread_mutex_unlock(&sync_lock);
	return 0;
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
	(void)fd;
	held_flush_syncs();             /* 0230: after the held submits */
	pthread_mutex_lock(&sync_lock);
	syncobj_seq[handle % JT_SYNCOBJS] = 0;
	pthread_mutex_unlock(&sync_lock);
	return 0;
}

int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count)
{
	(void)fd;
	held_flush_syncs();             /* 0230: after the held submits */
	if (handles == NULL) {
		return -EINVAL;
	}
	pthread_mutex_lock(&sync_lock);
	for (uint32_t i = 0; i < count; i++) {
		syncobj_seq[handles[i] % JT_SYNCOBJS] = 0;
	}
	pthread_mutex_unlock(&sync_lock);
	return 0;
}

int
drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points, uint32_t count)
{
	(void)fd;
	held_flush_syncs();             /* 0230: after the held submits */
	(void)points;
	if (handles == NULL) {
		return -EINVAL;
	}
	pthread_mutex_lock(&sync_lock);
	for (uint32_t i = 0; i < count; i++) {
		/* Every point is reached: the jobs are done.  0 is "nothing to
		 * wait for", which is what a signalled sync object is here. */
		syncobj_seq[handles[i] % JT_SYNCOBJS] = 0;
	}
	pthread_mutex_unlock(&sync_lock);
	return 0;
}

int
drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t count)
{
	return drmSyncobjTimelineSignal(fd, handles, NULL, count);
}

int
drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned count,
    int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
	struct jt_gpu *g;

	(void)fd;
	(void)points;                   /* one point per sync object here */
	(void)flags;                    /* WAIT_ALL and WAIT_FOR_SUBMIT: all are submitted */
	if (handles == NULL) {
		return -EINVAL;
	}
	held_flush_syncs();             /* 0230: their numbers are known once sent */
	if (first_signaled != NULL) {
		*first_signaled = 0;
	}
	g = jackpotlte_drm_gpu();
	for (unsigned i = 0; i < count; i++) {
		uint64_t seq;

		pthread_mutex_lock(&sync_lock);
		seq = syncobj_seq[handles[i] % JT_SYNCOBJS];
		pthread_mutex_unlock(&sync_lock);
		if (seq == 0) {
			continue;       /* nothing was submitted with it: nothing to wait for */
		}
		if (g == NULL) {
			return -ENODEV;
		}
		int ret = wait_seq_noted(g, seq, timeout_nsec);

		if (ret != 0) {
			return ret;
		}
	}
	return 0;
}

int
drmSyncobjWait(int fd, uint32_t *handles, unsigned count, int64_t timeout_nsec,
    unsigned flags, uint32_t *first_signaled)
{
	return drmSyncobjTimelineWait(fd, handles, NULL, count, timeout_nsec, flags,
	    first_signaled);
}

int
drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points, uint32_t count, uint32_t flags)
{
	(void)fd;
	(void)flags;
	if (handles == NULL || points == NULL) {
		return -EINVAL;
	}
	for (uint32_t i = 0; i < count; i++) {
		/* A binary sync object's signalled point is 1; here every one
		 * of them is signalled. */
		points[i] = 1;
	}
	return 0;
}

int
drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t count)
{
	return drmSyncobjQuery2(fd, handles, points, count, 0);
}

int
drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle,
    uint64_t src_point, uint32_t flags)
{
	(void)fd;
	held_flush_syncs();             /* 0230: after the held submits */
	(void)dst_point;
	(void)src_point;
	(void)flags;
	pthread_mutex_lock(&sync_lock);
	syncobj_seq[dst_handle % JT_SYNCOBJS] = syncobj_seq[src_handle % JT_SYNCOBJS];
	pthread_mutex_unlock(&sync_lock);
	return 0;
}

/*
 * A sync object as a sync file, and back.  panfrost makes one of every flush
 * (panfrost_fence_create) and polls it; a job is done before the submission
 * returns, so a fence that is always ready can be any descriptor - /dev/null's.
 * Without this every frame of the window server had no fence at all.
 */
int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
	int exported;

	(void)fd;
	if (sync_file_fd == NULL) {
		return -EINVAL;
	}
	held_flush_syncs();             /* 0230: the number it carries is known once sent */
	exported = open("/dev/null", O_RDONLY | O_CLOEXEC);
	if (exported < 0) {
		return -errno;
	}
	pthread_mutex_lock(&sync_lock);
	syncfile_seq[exported % JT_SYNCOBJS] = syncobj_seq[handle % JT_SYNCOBJS];
	pthread_mutex_unlock(&sync_lock);
	*sync_file_fd = exported;
	return 0;
}

int
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
	(void)fd;
	if (sync_file_fd < 0) {
		return -EINVAL;
	}
	held_flush_syncs();             /* 0230: after the held submits, as asked */
	pthread_mutex_lock(&sync_lock);
	syncobj_seq[handle % JT_SYNCOBJS] = syncfile_seq[sync_file_fd % JT_SYNCOBJS];
	pthread_mutex_unlock(&sync_lock);
	return 0;
}

int
drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
	(void)fd;
	(void)obj_fd;
	(void)handle;
	return syncobj_none();
}

int
drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{
	(void)fd;
	(void)handle;
	(void)obj_fd;
	return syncobj_none();
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Linux's device nodes and device list, which the Vulkan runtime walks: one
 * GPU, a render node, and nothing that has to be enumerated.
 */
int
drmGetNodeTypeFromFd(int fd)
{
	(void)fd;
	return DRM_NODE_RENDER;
}

int
drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{
	(void)flags;
	(void)devices;
	(void)max_devices;
	return 0;
}

void
drmFreeDevices(drmDevicePtr devices[], int count)
{
	(void)devices;
	(void)count;
}

int
drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
	(void)fd;
	(void)flags;
	if (device != NULL) {
		*device = NULL;
	}
	return -ENODEV;
}

void
drmFreeDevice(drmDevicePtr *device)
{
	(void)device;
}

/* The structures are the kernel's, field for field (0154:82-95). */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(offsetof(struct jackpotlte_drm_surface, row_bytes) ==
    offsetof(struct jt_surface_req, rowBytes), "0154: surface.rowBytes");
_Static_assert(offsetof(struct jackpotlte_drm_surface, pixel_format) ==
    offsetof(struct jt_surface_req, pixelFormat), "0154: surface.pixelFormat");
#endif
