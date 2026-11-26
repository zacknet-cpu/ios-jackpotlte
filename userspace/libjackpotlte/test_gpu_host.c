/* SPDX-License-Identifier: MIT */
/*
 * test_gpu_host: check the request layer against the ABI, on a build machine.
 *
 * There is no GPU here and no IOKit: a fake backend records the selector, the
 * size and the bytes that jt_gpu hands it, and answers the way 0154 does.  So
 * this checks the half of M4 that can be checked without the phone - that the
 * right request carries the right structure with the right fields - and it
 * runs on Linux, so it also runs in the userspace CI job.
 *
 * It is not a test of the kernel.  "Green" here means the layer asks correctly,
 * nothing about whether the GPU answers; that needs the phone.
 */
#include "jackpotlte_drm.h"
#include "jackpotlte_gpu.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static unsigned failures;

#define CHECK(cond, ...) do {                                   \
	if (!(cond)) {                                          \
		failures++;                                     \
		printf("FAIL %s:%d: ", __func__, __LINE__);     \
		printf(__VA_ARGS__);                            \
		printf("\n");                                   \
	}                                                       \
} while (0)

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * The fake IOKit.
 */
struct fake {
	unsigned calls;
	uint32_t selector;
	size_t   size;
	unsigned char sent[64];         /* what the layer sent, before any reply */
	uint32_t kr;                    /* what call() answers with */
	uint64_t job_seq;               /* what the "kernel" gives the next job */

	unsigned maps, unmaps, closes;
	uint32_t unmap_handle;          /* the type the unmap came back with */
	void    *map_address;
	uint64_t map_size;
	uint32_t map_kr;

	/* Fill in the kernel's answer for this selector, and only this one: a
	 * reply armed for another selector would write its own structure's
	 * bytes into a request of a different size. */
	void (*reply)(struct fake *f, unsigned char *data, size_t size);
	uint32_t reply_selector;

	uint32_t refuse;                /* 0230: a selector answered "unsupported" */
	uint32_t log[64];               /* 0230: the selectors called, in order */
	unsigned nlog;
	uint64_t log_jc[64];            /* the first request's chain of each call */
};

static uint32_t
fake_call(void *ctx, uint32_t selector, void *data, size_t size)
{
	struct fake *f = ctx;

	f->calls++;
	if (f->nlog < 64) {
		f->log_jc[f->nlog] = size >= sizeof(struct jt_submit_req) ?
		    ((struct jt_submit_req *)data)->jc : 0;
		f->log[f->nlog++] = selector;
	}
	if (f->refuse != 0 && selector == f->refuse) {
		return 0xe00002c7;      /* kIOReturnUnsupported */
	}
	f->selector = selector;
	f->size = size;
	if (size <= sizeof(f->sent)) {
		memcpy(f->sent, data, size);
	}
	if (f->kr == JT_IORETURN_SUCCESS) {
		if (f->reply != NULL && f->selector == f->reply_selector) {
			f->reply(f, data, size);
		}
		/* A submit always comes back with the job's number in `jc`, 0 when
		 * the kernel refused it (0154:541) - so the layer can read 0 as
		 * "not queued" rather than the chain it sent. */
		if (f->selector == JT_PANFROST_SUBMIT && size >= sizeof(struct jt_submit_req)) {
			((struct jt_submit_req *)data)->jc = f->job_seq;
		}
		/* 0218/0230: several in one call, numbered in order */
		if ((f->selector == JT_SUBMIT2 || f->selector == JT_SUBMITN) &&
		    size % sizeof(struct jt_submit_req) == 0) {
			for (size_t k = 0; k < size / sizeof(struct jt_submit_req); k++) {
				((struct jt_submit_req *)data)[k].jc = f->job_seq++;
			}
		}
	}
	return f->kr;
}

static uint32_t
fake_map(void *ctx, uint32_t handle, void **address, uint64_t *size)
{
	struct fake *f = ctx;

	f->maps++;
	f->selector = handle;
	if (f->map_kr != JT_IORETURN_SUCCESS) {
		return f->map_kr;
	}
	*address = f->map_address;
	*size = f->map_size;
	return JT_IORETURN_SUCCESS;
}

static void
fake_unmap(void *ctx, uint32_t handle, void *address, uint64_t size)
{
	struct fake *f = ctx;

	(void)address;
	(void)size;
	f->unmaps++;
	f->unmap_handle = handle;
}

static void
fake_close(void *ctx)
{
	struct fake *f = ctx;

	f->closes++;
}

static const struct jt_backend fake_backend = {
	fake_call, fake_map, fake_unmap, fake_close,
};

/* One fake at a time - the tests run one after another, and the layer holds
 * the pointer, so it has to outlive any open. */
static struct fake G;

/* Arm the fake's answer for one selector. */
static void
arm(struct fake *f, uint32_t selector,
    void (*reply)(struct fake *, unsigned char *, size_t))
{
	f->reply = reply;
	f->reply_selector = selector;
}

static struct jt_gpu *
open_with(struct fake **fp)
{
	struct jt_gpu *g = NULL;

	memset(&G, 0, sizeof(G));
	G.kr = JT_IORETURN_SUCCESS;
	G.map_kr = JT_IORETURN_SUCCESS;
	CHECK(jt_gpu_open(&g, &fake_backend, &G) == 0, "open failed");
	/* the open's own look for the done page (xnu 0225) is not what the
	 * tests count */
	G.maps = 0;
	G.unmaps = 0;
	G.selector = 0;
	G.unmap_handle = 0;
	*fp = &G;
	return g;
}

/* xnu 0225: the done page is mapped at open, kept only with its magic, and
 * let go at close; an older kernel's refusal leaves none. */
static void
test_done_page(void)
{
	static uint64_t page[4] = { 5, JT_DONE_PAGE_MAGIC, 9, 0 };
	struct jt_gpu *g = NULL;

	memset(&G, 0, sizeof(G));
	G.kr = JT_IORETURN_SUCCESS;
	G.map_address = page;
	G.map_size = sizeof(page);
	CHECK(jt_gpu_open(&g, &fake_backend, &G) == 0, "open failed");
	CHECK(G.maps == 1 && G.selector == JT_DONE_PAGE_TYPE, "the done page was not asked for");
	CHECK(jt_done_page(g) == page && jt_done_page(g)[0] == 5, "the done page was not kept");
	jt_gpu_close(g);
	CHECK(G.unmaps == 1 && G.unmap_handle == JT_DONE_PAGE_TYPE, "the done page was not let go");

	page[1] = 0;
	memset(&G, 0, sizeof(G));
	G.map_address = page;
	G.map_size = sizeof(page);
	CHECK(jt_gpu_open(&g, &fake_backend, &G) == 0, "open failed");
	CHECK(jt_done_page(g) == NULL && G.unmaps == 1, "a page without the magic was kept");
	jt_gpu_close(g);
	page[1] = JT_DONE_PAGE_MAGIC;

	memset(&G, 0, sizeof(G));
	G.map_kr = JT_IORETURN_BAD_ARGUMENT;
	CHECK(jt_gpu_open(&g, &fake_backend, &G) == 0, "open failed");
	CHECK(jt_done_page(g) == NULL && G.unmaps == 0, "an older kernel's refusal left a page");
	jt_gpu_close(g);
	CHECK(G.unmaps == 0, "close let go of a page that was never there");
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * The kernel's answers.
 */
static void
reply_create(struct fake *f, unsigned char *data, size_t size)
{
	struct jt_create_bo_req *r = (struct jt_create_bo_req *)data;

	(void)f;
	(void)size;
	r->handle = 7;
	r->offset = 0x01000000;         /* JT_FIRST_JOB_GPU: where 0153 starts */
}

static void
reply_param(struct fake *f, unsigned char *data, size_t size)
{
	struct jt_get_param_req *r = (struct jt_get_param_req *)data;

	(void)f;
	(void)size;
	r->value = 0x7010;              /* a plausible id */
}

static void
reply_offset(struct fake *f, unsigned char *data, size_t size)
{
	struct jt_get_bo_offset_req *r = (struct jt_get_bo_offset_req *)data;

	(void)f;
	(void)size;
	r->offset = 0x02000000;
}

/* 0154:551: the kernel keeps every buffer and says so. */
static void
reply_madvise(struct fake *f, unsigned char *data, size_t size)
{
	struct jt_madvise_req *r = (struct jt_madvise_req *)data;

	(void)f;
	(void)size;
	r->retained = 1;
}

/* A SweetSurface as the kernel describes it (0154:328-351). */
static void
reply_surface(struct fake *f, unsigned char *data, size_t size)
{
	struct jt_surface_req *r = (struct jt_surface_req *)data;

	(void)f;
	(void)size;
	r->handle = 9;
	r->size = 0x400000;
	r->offset = 0;
	r->rowBytes = 1080 * 4;
	r->width = 1080;
	r->height = 2220;
	r->pixelFormat = 0x4;
	r->planes = 1;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

static void
test_open(void)
{
	struct jt_gpu *g = NULL;
	struct fake *f = NULL;
	struct jt_backend broken = fake_backend;

	CHECK(jt_gpu_open(&g, NULL, NULL) == -EINVAL, "a NULL backend was accepted");
	broken.map = NULL;
	CHECK(jt_gpu_open(&g, &broken, NULL) == -EINVAL, "a backend without map was accepted");

	g = open_with(&f);
	CHECK(g != NULL, "open_with returned nothing");
	CHECK(f->closes == 0, "the connection was closed before it was used");
	if (g != NULL) {
		jt_gpu_close(g);
		CHECK(f->closes == 1, "close did not reach the backend");
	}
}

static void
test_create_bo(void)
{
	struct fake *f;
	struct jt_gpu *g = open_with(&f);
	struct jt_create_bo_req sent;
	uint32_t handle = 0;
	uint64_t address = 0;

	arm(f, JT_PANFROST_CREATE_BO, reply_create);
	CHECK(jt_bo_create(g, 16384, &handle, &address) == 0, "create_bo failed");
	memcpy(&sent, f->sent, sizeof(sent));
	CHECK(f->selector == JT_PANFROST_CREATE_BO, "selector %#x, not create_bo", f->selector);
	CHECK(f->size == sizeof(struct jt_create_bo_req), "sent %zu bytes", f->size);
	CHECK(sent.size == 16384, "asked for %u bytes", sent.size);
	CHECK(handle == 7, "handle %u", handle);
	CHECK(address == 0x01000000, "address %#llx", (unsigned long long)address);

	/* A buffer with no address is not usable and must not look like success. */
	f->reply = NULL;
	CHECK(jt_bo_create(g, 4096, &handle, &address) == -EIO,
	    "a handle of 0 was accepted");
	CHECK(jt_bo_create(g, 0, &handle, &address) == -EINVAL, "size 0 was accepted");

	/* The kernel's refusals arrive as errno. */
	f->kr = JT_IORETURN_NOT_READY;
	CHECK(jt_bo_create(g, 4096, &handle, &address) == -ENODEV,
	    "not-ready did not become -ENODEV");
	jt_gpu_close(g);
}

static void
test_map(void)
{
	struct fake *f;
	struct jt_gpu *g = open_with(&f);
	static int memory[4];
	void *address = NULL;
	uint64_t size = 0;

	f->map_address = memory;
	f->map_size = sizeof(memory);
	CHECK(jt_bo_map(g, 3, &address, &size) == 0, "map failed");
	CHECK(f->maps == 1, "%u maps", f->maps);
	CHECK(f->selector == 3, "the client-memory type was %u, not the handle", f->selector);
	CHECK(address == memory && size == sizeof(memory), "address/size wrong");
	CHECK(jt_bo_unmap(g, 3, address, size) == 0, "unmap failed");
	CHECK(f->unmaps == 1, "%u unmaps", f->unmaps);
	CHECK(f->unmap_handle == 3, "the unmap carried type %u, not the handle", f->unmap_handle);

	f->map_kr = JT_IORETURN_NOT_FOUND;
	CHECK(jt_bo_map(g, 4, &address, &size) == -ENOENT,
	    "a missing buffer did not become -ENOENT");
	jt_gpu_close(g);
}

static void
test_submit(void)
{
	struct fake *f;
	struct jt_gpu *g = open_with(&f);
	uint32_t handles[2] = { 7, 8 };
	struct jt_submit_req sent;
	uint64_t seq;
	int error = 0;

	f->job_seq = 42;                        /* the job the "kernel" queues */
	seq = jt_submit(g, 0x01000000, true, handles, 2, &error);
	memcpy(&sent, f->sent, sizeof(sent));
	CHECK(error == 0, "submit reported %d", error);
	CHECK(seq == 42, "job number %llu", (unsigned long long)seq);
	CHECK(f->selector == JT_PANFROST_SUBMIT, "selector %#x", f->selector);
	CHECK(f->size == sizeof(struct jt_submit_req), "sent %zu bytes", f->size);
	CHECK(sent.jc == 0x01000000, "chain %#llx", (unsigned long long)sent.jc);
	CHECK(sent.bo_handles == (uint64_t)(uintptr_t)handles,
	    "the handle array did not travel by address");
	CHECK(sent.bo_handle_count == 2, "count %u", sent.bo_handle_count);
	CHECK((sent.requirements & JT_JD_REQ_FS) != 0, "the fragment bit is missing");

	/* A vertex job must not carry the fragment bit. */
	(void)jt_submit(g, 0x01000000, false, NULL, 0, &error);
	memcpy(&sent, f->sent, sizeof(sent));
	CHECK((sent.requirements & JT_JD_REQ_FS) == 0, "a vertex job asked to be a fragment job");
	CHECK(sent.bo_handle_count == 0 && sent.bo_handles == 0, "no-handle submit is not empty");

	/* The kernel answering "no job number" is a failure, not job 0. */
	f->job_seq = 0;
	seq = jt_submit(g, 0x01000000, false, NULL, 0, &error);
	CHECK(seq == 0 && error == -EIO, "an answer of 0 was taken as a job (error %d, seq %llu)",
	    error, (unsigned long long)seq);

	f->kr = JT_IORETURN_UNSUPPORTED;
	(void)jt_submit(g, 0x01000000, false, NULL, 0, &error);
	CHECK(error == -ENOTTY, "an unsupported submit became %d", error);
	jt_gpu_close(g);
}

static void
test_waits(void)
{
	struct fake *f;
	struct jt_gpu *g = open_with(&f);
	struct jt_wait_bo_req bo;
	struct jt_wait_req seqreq;

	CHECK(jt_wait_bo(g, 5, 1000000000) == 0, "wait_bo failed");
	memcpy(&bo, f->sent, sizeof(bo));
	CHECK(f->selector == JT_PANFROST_WAIT_BO, "selector %#x", f->selector);
	CHECK(bo.handle == 5 && bo.timeout_ns == 1000000000, "wait_bo fields wrong");

	CHECK(jt_wait_seq(g, 42, 2000000000) == 0, "wait_seq failed");
	memcpy(&seqreq, f->sent, sizeof(seqreq));
	CHECK(f->selector == JT_WAIT, "selector %#x", f->selector);
	CHECK(seqreq.seq == 42 && seqreq.timeout_ns == 2000000000, "wait_seq fields wrong");

	f->kr = JT_IORETURN_TIMEOUT;
	CHECK(jt_wait_seq(g, 42, 1) == -ETIMEDOUT, "a timeout did not become -ETIMEDOUT");
	jt_gpu_close(g);
}

static void
test_param_and_release(void)
{
	struct fake *f;
	struct jt_gpu *g = open_with(&f);
	uint64_t value = 0;
	struct jt_get_param_req param;
	struct jt_gem_close close_req;

	arm(f, JT_PANFROST_GET_PARAM, reply_param);
	CHECK(jt_get_param(g, JT_PARAM_GPU_ID_HI, &value) == 0, "get_param failed");
	memcpy(&param, f->sent, sizeof(param));
	CHECK(param.param == JT_PARAM_GPU_ID_HI, "param %u", param.param);
	CHECK(value == 0x7010, "value %#llx", (unsigned long long)value);

	arm(f, JT_PANFROST_GET_BO_OFFSET, reply_offset);
	CHECK(jt_bo_offset(g, 7, &value) == 0 && value == 0x02000000, "get_bo_offset wrong");

	CHECK(jt_bo_release(g, 7) == 0, "release failed");
	memcpy(&close_req, f->sent, sizeof(close_req));
	CHECK(f->selector == JT_DRM_GEM_CLOSE, "selector %#x", f->selector);
	CHECK(close_req.handle == 7, "closed handle %u", close_req.handle);

	f->kr = JT_IORETURN_NOT_FOUND;
	CHECK(jt_bo_release(g, 9) == -ENOENT, "a missing handle did not become -ENOENT");
	jt_gpu_close(g);
}

static void
test_madvise(void)
{
	struct fake *f;
	struct jt_gpu *g = open_with(&f);
	struct jt_madvise_req sent;
	uint32_t retained = 0;

	arm(f, JT_PANFROST_MADVISE, reply_madvise);
	CHECK(jt_bo_madvise(g, 7, 1 /* PANFROST_MADV_DONTNEED */, &retained) == 0,
	    "madvise failed");
	memcpy(&sent, f->sent, sizeof(sent));
	CHECK(f->selector == JT_PANFROST_MADVISE, "selector %#x", f->selector);
	CHECK(f->size == sizeof(struct jt_madvise_req), "sent %zu bytes", f->size);
	CHECK(sent.handle == 7 && sent.madv == 1, "madvise fields wrong");
	CHECK(retained == 1, "retained %u, not the kernel's answer", retained);

	CHECK(jt_bo_madvise(g, 0, 0, &retained) == -EINVAL, "handle 0 was accepted");
	jt_gpu_close(g);
}

static void
test_surface(void)
{
	struct fake *f;
	struct jt_gpu *g = open_with(&f);
	struct jt_surface_req info;
	struct jt_surface_req sent;
	uint32_t handle = JT_SURFACE_DESCRIBE;

	/* Describe only: no buffer is wanted, so the describe value travels. */
	f->reply = NULL;
	CHECK(jt_surface_buffer(g, 12, &handle, &info) == 0, "surface describe failed");
	memcpy(&sent, f->sent, sizeof(sent));
	CHECK(f->selector == JT_SURFACE, "selector %#x", f->selector);
	CHECK(sent.surface == 12 && sent.handle == JT_SURFACE_DESCRIBE,
	    "surface %u handle %#x", sent.surface, sent.handle);
	jt_gpu_close(g);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * The libdrm face (jackpotlte_drm.c): the same fake, reached through the
 * request codes Mesa uses.  What is checked is the translation - that a DRM
 * request becomes the kernel's structure, with the right fields, and that the
 * kernel's answer comes back in the caller's structure.
 */
static void
test_drm_ioctl(void)
{
	struct fake *f;
	struct jt_gpu *g = open_with(&f);
	struct jt_create_bo_req create;
	struct jt_get_param_req param;
	struct jt_mmap_bo_req mmap_bo;
	struct jt_madvise_req madvise;
	struct jt_submit_req submit;
	struct jt_wait_req waited;
	uint32_t handles[2] = { 7, 8 };
	unsigned char dummy[16] = { 0 };

	jackpotlte_drm_attach(g);

	/* The code's size field is the structure's size: drmIoctl() has only
	 * the code, so that is all it can tell the kernel. */
	memset(&create, 0, sizeof(create));
	create.size = 16384;
	arm(f, JT_PANFROST_CREATE_BO, reply_create);
	CHECK(drmIoctl(-1, JACKPOTLTE_DRM_REQUEST(JT_PANFROST_CREATE_BO, sizeof(create)),
	    &create) == 0, "create_bo through drmIoctl failed");
	CHECK(f->selector == JT_PANFROST_CREATE_BO, "selector %#x", f->selector);
	CHECK(f->size == sizeof(struct jt_create_bo_req), "sent %zu bytes", f->size);
	CHECK(create.handle == 7 && create.offset == 0x01000000, "the answer was not written back");

	/* A request unknown to the kernel is refused here, without a call. */
	CHECK(jackpotlte_drm_ioctl(g, JACKPOTLTE_DRM_REQUEST(0x46, 8), dummy, 8) == -ENOTTY,
	    "the performance counter request was not -ENOTTY");
	CHECK(jackpotlte_drm_ioctl(g, JACKPOTLTE_DRM_REQUEST(0x0a, 8), dummy, 8) == -ENOTTY,
	    "a JM context request was not -ENOTTY");
	CHECK(jackpotlte_drm_ioctl(g, JACKPOTLTE_DRM_REQUEST(0x40, 16), dummy, 16) == -EINVAL,
	    "a short submit structure was accepted");

	memset(&param, 0, sizeof(param));
	param.param = JT_PARAM_GPU_ID_HI;
	arm(f, JT_PANFROST_GET_PARAM, reply_param);
	CHECK(jackpotlte_drm_ioctl(g, JACKPOTLTE_DRM_REQUEST(JT_PANFROST_GET_PARAM, sizeof(param)),
	    &param, sizeof(param)) == 0, "get_param failed");
	CHECK(param.value == 0x7010, "value %#llx", (unsigned long long)param.value);

	/* MMAP_BO is answered here: the offset is bookkeeping (the mapping is by
	 * handle), and the kernel is not asked. */
	memset(&mmap_bo, 0, sizeof(mmap_bo));
	mmap_bo.handle = 5;
	CHECK(jackpotlte_drm_ioctl(g, JACKPOTLTE_DRM_REQUEST(JT_PANFROST_MMAP_BO, sizeof(mmap_bo)),
	    &mmap_bo, sizeof(mmap_bo)) == 0, "mmap_bo failed");
	CHECK(mmap_bo.offset == (uint64_t)5 << 12, "offset %#llx", (unsigned long long)mmap_bo.offset);

	memset(&madvise, 0, sizeof(madvise));
	madvise.handle = 7;
	madvise.madv = 1;
	arm(f, JT_PANFROST_MADVISE, reply_madvise);
	CHECK(jackpotlte_drm_ioctl(g, JACKPOTLTE_DRM_REQUEST(JT_PANFROST_MADVISE, sizeof(madvise)),
	    &madvise, sizeof(madvise)) == 0, "madvise failed");
	CHECK(madvise.retained == 1, "retained was not written back");

	/* The submit: the kernel's number comes back in `jc`, and the sync
	 * object the caller named is given that number.  (Before panfrost_flush
	 * has called jackpotlte_drm_flush_end, 0230 behaves as 0218: a lone
	 * fragment submit goes at once.) */
	memset(&submit, 0, sizeof(submit));
	submit.jc = 0x01000000;
	submit.out_sync = 3;
	submit.bo_handles = (uint64_t)(uintptr_t)handles;
	submit.bo_handle_count = 2;
	submit.requirements = JT_JD_REQ_FS;
	f->job_seq = 42;
	CHECK(jackpotlte_drm_ioctl(g, JACKPOTLTE_DRM_REQUEST(JT_PANFROST_SUBMIT, sizeof(submit)),
	    &submit, sizeof(submit)) == 0, "submit failed");
	CHECK(f->selector == JT_PANFROST_SUBMIT, "selector %#x", f->selector);
	CHECK(f->size == 40, "the kernel was sent %zu bytes, not its 40-byte submit", f->size);
	CHECK(submit.jc == 42, "the job's number was not written back in jc");
	CHECK(jackpotlte_drm_last_seq() == 42, "last_seq %llu",
	    (unsigned long long)jackpotlte_drm_last_seq());

	CHECK(drmSyncobjWait(-1, &submit.out_sync, 1, 0, 0, NULL) == 0,
	    "the sync object did not wait for the job");
	memcpy(&waited, f->sent, sizeof(waited));
	CHECK(f->selector == JT_WAIT, "selector %#x", f->selector);
	CHECK(waited.seq == 42, "waited for job %llu", (unsigned long long)waited.seq);

	/* A WAIT_BO for a buffer whose last job is known done (job 42, just
	 * waited for) or that no job used is answered without the kernel; one
	 * for a buffer a later job names goes to the kernel. */
	{
		struct jt_wait_bo_req bo;
		uint32_t again[1] = { 7 };

		memset(&bo, 0, sizeof(bo));
		bo.handle = 7;
		f->selector = 0;
		CHECK(jackpotlte_drm_ioctl(g, JACKPOTLTE_DRM_REQUEST(JT_PANFROST_WAIT_BO, sizeof(bo)),
		    &bo, sizeof(bo)) == 0, "wait_bo for a done buffer failed");
		CHECK(f->selector == 0, "a done buffer's wait_bo went to the kernel (%#x)", f->selector);
		bo.handle = 9;
		CHECK(jackpotlte_drm_ioctl(g, JACKPOTLTE_DRM_REQUEST(JT_PANFROST_WAIT_BO, sizeof(bo)),
		    &bo, sizeof(bo)) == 0, "wait_bo for an unused buffer failed");
		CHECK(f->selector == 0, "an unused buffer's wait_bo went to the kernel (%#x)", f->selector);

		memset(&submit, 0, sizeof(submit));
		submit.jc = 0x01000000;
		submit.bo_handles = (uint64_t)(uintptr_t)again;
		submit.bo_handle_count = 1;
		f->job_seq = 43;
		CHECK(jackpotlte_drm_ioctl(g, JACKPOTLTE_DRM_REQUEST(JT_PANFROST_SUBMIT, sizeof(submit)),
		    &submit, sizeof(submit)) == 0, "second submit failed");
		bo.handle = 7;
		f->selector = 0;
		CHECK(jackpotlte_drm_ioctl(g, JACKPOTLTE_DRM_REQUEST(JT_PANFROST_WAIT_BO, sizeof(bo)),
		    &bo, sizeof(bo)) == 0, "wait_bo for a busy buffer failed");
		CHECK(f->selector == JT_PANFROST_WAIT_BO, "a busy buffer's wait_bo did not reach the kernel");
	}

	/* A surface: described, and then made.  The field names differ from the
	 * kernel's; the structure that travels is the kernel's. */
	{
		struct jackpotlte_drm_surface described;
		struct jackpotlte_drm_surface made;
		struct jt_surface_req sent;

		memset(&described, 0, sizeof(described));
		described.surface = 12;
		described.handle = JACKPOTLTE_DRM_DESCRIBE;
		CHECK(jackpotlte_drm_surface(-1, &described) == 0, "surface describe failed");
		memcpy(&sent, f->sent, sizeof(sent));
		CHECK(sent.surface == 12 && sent.handle == JT_SURFACE_DESCRIBE,
		    "surface %u handle %#x", sent.surface, sent.handle);

		memset(&made, 0, sizeof(made));
		made.surface = 12;
		made.handle = 0;
		arm(f, JT_SURFACE, reply_surface);
		CHECK(jackpotlte_drm_surface(-1, &made) == 0, "surface describe-and-make failed");
		CHECK(made.handle == 9 && made.size == 0x400000, "the buffer was not written back");
		CHECK(made.row_bytes == 1080 * 4 && made.width == 1080 && made.height == 2220,
		    "row_bytes/width/height were not translated");
		CHECK(made.pixel_format == 0x4 && made.planes == 1,
		    "pixel_format/planes were not translated");
	}

	/* A buffer given back takes its mapping with it. */
	{
		static int memory[2];

		f->map_address = memory;
		f->map_size = sizeof(memory);
		CHECK(jackpotlte_drm_map(-1, 5, 8) == memory, "map failed");
		CHECK(jackpotlte_drm_map(-1, 5, 8) == memory, "the second map failed");
		CHECK(f->maps == 1, "the second map called the kernel again (%u maps)", f->maps);
		jackpotlte_drm_unmap(-1, 5, memory);
		CHECK(f->unmaps == 0, "unmap did not keep the mapping");
		CHECK(drmCloseBufferHandle(-1, 5) == 0, "close failed");
		jackpotlte_drm_close_drain();   /* 11-26: closes run on a thread of their own */
		CHECK(f->unmaps == 1 && f->unmap_handle == 5, "the mapping was not unmapped");
		CHECK(f->selector == JT_DRM_GEM_CLOSE, "selector %#x", f->selector);
	}

	jackpotlte_drm_attach(NULL);
	jt_gpu_close(g);
}

static void
test_drm_misc(void)
{
	drmVersionPtr version;
	drmDevicePtr device = NULL;
	uint64_t capability = 1;
	uint32_t a = 0, b = 0;
	int file = -1;

	jackpotlte_drm_attach(NULL);
	CHECK(drmOpenWithType("panfrost", NULL, DRM_NODE_RENDER) == -1,
	    "a device was opened without a GPU");
	CHECK(drmOpenWithType("msm", NULL, DRM_NODE_RENDER) == -1, "an msm device was opened");

	version = drmGetVersion(-1);
	CHECK(version != NULL, "no version");
	if (version != NULL) {
		CHECK(version->version_major == 1 && version->version_minor == 2,
		    "version %d.%d", version->version_major, version->version_minor);
		CHECK(version->name != NULL && strcmp(version->name, "panfrost") == 0,
		    "name %s", version->name == NULL ? "(null)" : version->name);
		drmFreeVersion(version);
	}

	CHECK(drmGetCap(-1, 0, &capability) == 0 && capability == 0, "get_cap");
	CHECK(drmGetNodeTypeFromFd(-1) == DRM_NODE_RENDER, "node type");
	CHECK(drmGetDevices2(0, NULL, 0) == 0, "device enumeration is not empty");
	CHECK(drmGetDevice2(-1, 0, &device) == -ENODEV, "a device was found");
	CHECK(drmPrimeHandleToFD(-1, 1, 0, &file) == -1 && errno == ENOSYS,
	    "a buffer was exported as a descriptor");

	CHECK(drmSyncobjCreate(-1, 0, &a) == 0 && a != 0, "syncobj create");
	CHECK(drmSyncobjCreate(-1, 0, &b) == 0 && b != a, "the second syncobj is not new");
	CHECK(drmSyncobjExportSyncFile(-1, a, &file) == 0 && file >= 0, "sync file export");
	if (file >= 0) {
		CHECK(drmSyncobjImportSyncFile(-1, b, file) == 0, "sync file import");
		close(file);
	}
	CHECK(drmSyncobjTransfer(-1, b, 0, a, 0, 0) == 0, "syncobj transfer");
	CHECK(drmSyncobjWait(-1, &b, 1, 0, 0, NULL) == 0, "an unsubmitted sync object did not wait");
	CHECK(drmSyncobjReset(-1, &a, 1) == 0, "syncobj reset");
	CHECK(drmSyncobjDestroy(-1, a) == 0, "syncobj destroy");
	CHECK(drmSyncobjFDToHandle(-1, -1, &a) == -ENOSYS, "syncobj fd import is not ENOSYS");
}

static void
test_errno_table(void)
{
	CHECK(jt_errno(JT_IORETURN_SUCCESS) == 0, "success");
	CHECK(jt_errno(JT_IORETURN_BAD_ARGUMENT) == -EINVAL, "bad argument");
	CHECK(jt_errno(JT_IORETURN_UNSUPPORTED) == -ENOTTY, "unsupported");
	CHECK(jt_errno(JT_IORETURN_NOT_READY) == -ENODEV, "not ready");
	CHECK(jt_errno(JT_IORETURN_TIMEOUT) == -ETIMEDOUT, "timeout");
}

/* 0230: whole passes are held and sent together, in order, when there are
 * a8gl-batch's number of them (4 here: no switch file on a build machine) or
 * something must see them run; a kernel that refuses 0x7c gets pairs. */
static void
submit_pass(struct jt_gpu *g, uint64_t chain, uint32_t *handle, uint32_t out_sync)
{
	struct jt_submit_req r;

	memset(&r, 0, sizeof(r));
	r.jc = chain;                   /* vertex/tiler */
	r.bo_handles = (uint64_t)(uintptr_t)handle;
	r.bo_handle_count = 1;
	CHECK(jackpotlte_drm_ioctl(g, JACKPOTLTE_DRM_REQUEST(JT_PANFROST_SUBMIT, sizeof(r)),
	    &r, sizeof(r)) == 0, "vertex submit failed");
	r.jc = chain + 1;               /* fragment */
	r.requirements = JT_JD_REQ_FS;
	r.out_sync = out_sync;
	CHECK(jackpotlte_drm_ioctl(g, JACKPOTLTE_DRM_REQUEST(JT_PANFROST_SUBMIT, sizeof(r)),
	    &r, sizeof(r)) == 0, "fragment submit failed");
}

static void
test_batch(void)
{
	struct fake *f;
	struct jt_gpu *g = open_with(&f);
	uint32_t h[8] = { 11, 12, 13, 14, 15, 16, 17, 18 };
	struct jt_wait_bo_req bo;
	uint32_t sync = 0;

	jackpotlte_drm_attach(g);
	f->job_seq = 100;
	f->nlog = 0;

	/* before panfrost_flush's end was seen: 0218, one call a pass */
	submit_pass(g, 0x0f00, &h[7], 0);
	CHECK(f->nlog == 1 && f->log[0] == JT_SUBMIT2, "without the flush end a pass was not one "
	    "pair (%u calls, first %#x)", f->nlog, f->nlog ? f->log[0] : 0);
	jackpotlte_drm_flush_end();     /* what tools/mesa-jackpotlte/0007 calls */
	CHECK(f->nlog == 1, "the flush end sent something with nothing held");
	f->job_seq = 100;
	f->nlog = 0;

	/* four passes: one call, eight submits, in order */
	for (unsigned p = 0; p < 3; p++) {
		submit_pass(g, 0x1000 + 0x10 * p, &h[p], 0);
	}
	CHECK(f->nlog == 0, "three passes were sent (%u calls)", f->nlog);
	submit_pass(g, 0x1030, &h[3], 0);
	CHECK(f->nlog == 1 && f->log[0] == JT_SUBMITN, "four passes: %u calls, first %#x", f->nlog,
	    f->nlog ? f->log[0] : 0);
	CHECK(f->size == 8 * sizeof(struct jt_submit_req), "the batch was %zu bytes", f->size);
	CHECK(f->log_jc[0] == 0x1000, "the batch did not start with the first pass's vertex chain");
	CHECK(jackpotlte_drm_last_seq() == 107, "last_seq %llu after the batch",
	    (unsigned long long)jackpotlte_drm_last_seq());
	CHECK(f->nlog == 1, "the frame-end look sent something with nothing held");

	/* a panfrost_flush that ends a frame with two passes held: sent at its end */
	submit_pass(g, 0x1800, &h[6], 0);
	submit_pass(g, 0x1810, &h[7], 0);
	CHECK(f->nlog == 1, "two passes were sent before the flush end");
	jackpotlte_drm_flush_end();
	CHECK(f->nlog == 2 && f->log[1] == JT_SUBMITN && f->log_jc[1] == 0x1800,
	    "the flush end did not send the two held passes in one call (%u calls)", f->nlog);
	f->job_seq = 108;

	/* each buffer has its own pass's number: the fourth pass's (106, 107) */
	memset(&bo, 0, sizeof(bo));
	bo.handle = 14;
	bo.timeout_ns = 1000000;
	f->nlog = 0;
	CHECK(jackpotlte_drm_ioctl(g, JACKPOTLTE_DRM_REQUEST(JT_PANFROST_WAIT_BO, sizeof(bo)),
	    &bo, sizeof(bo)) == 0, "wait_bo after the batch failed");
	CHECK(f->nlog == 1 && f->log[0] == JT_PANFROST_WAIT_BO, "the wait was not one WAIT_BO");

	/* a held pass: a look is busy without a call; GEM_CLOSE sends it first */
	submit_pass(g, 0x2000, &h[4], 0);
	f->nlog = 0;
	bo.handle = 15;
	bo.timeout_ns = 0;
	CHECK(jackpotlte_drm_ioctl(g, JACKPOTLTE_DRM_REQUEST(JT_PANFROST_WAIT_BO, sizeof(bo)),
	    &bo, sizeof(bo)) == -ETIMEDOUT, "a look at a held pass's buffer was not busy");
	CHECK(f->nlog == 0, "a look at a held pass's buffer called the kernel");
	{
		struct jt_madvise_req m;

		memset(&m, 0, sizeof(m));
		m.handle = 15;
		(void)jackpotlte_drm_ioctl(g, JACKPOTLTE_DRM_REQUEST(JT_PANFROST_MADVISE, sizeof(m)),
		    &m, sizeof(m));
		CHECK(f->nlog == 0, "MADVISE (answered here) sent the held pass (%u calls)", f->nlog);
	}
	{
		struct jt_gem_close c;

		memset(&c, 0, sizeof(c));
		c.handle = 99;
		f->nlog = 0;
		(void)jackpotlte_drm_ioctl(g, JACKPOTLTE_DRM_REQUEST(JT_DRM_GEM_CLOSE, sizeof(c)),
		    &c, sizeof(c));
		jackpotlte_drm_close_drain();
		CHECK(f->nlog >= 1 && f->log[0] == JT_SUBMIT2, "GEM_CLOSE did not send the held pass first "
		    "(%u calls, first %#x)", f->nlog, f->nlog ? f->log[0] : 0);
	}

	/* a held pass that signals a sync object: waiting on it sends it first,
	 * and the wait is for its fragment job */
	CHECK(drmSyncobjCreate(-1, 0, &sync) == 0, "syncobj create failed");
	f->job_seq = 200;
	submit_pass(g, 0x3000, &h[5], sync);
	f->nlog = 0;
	CHECK(drmSyncobjWait(-1, &sync, 1, 1000000, 0, NULL) == 0, "syncobj wait failed");
	CHECK(f->nlog == 2 && f->log[0] == JT_SUBMIT2 && f->log[1] == JT_WAIT,
	    "syncobj wait: %u calls (%#x, %#x)", f->nlog, f->log[0], f->log[1]);
	{
		struct jt_wait_req w;

		memcpy(&w, f->sent, sizeof(w));
		CHECK(w.seq == 201, "the sync object waited for job %llu, not the fragment's 201",
		    (unsigned long long)w.seq);
	}

	/* a kernel older than 0230: the four passes go as four pairs, in order */
	f->refuse = JT_SUBMITN;
	f->nlog = 0;
	f->job_seq = 300;
	for (unsigned p = 0; p < 4; p++) {
		submit_pass(g, 0x4000 + 0x10 * p, &h[p], 0);
	}
	CHECK(f->nlog == 5 && f->log[0] == JT_SUBMITN && f->log[1] == JT_SUBMIT2 &&
	    f->log[4] == JT_SUBMIT2 && f->log_jc[1] == 0x4000 && f->log_jc[4] == 0x4030,
	    "refused batch: %u calls", f->nlog);
	f->nlog = 0;
	for (unsigned p = 0; p < 4; p++) {
		submit_pass(g, 0x5000 + 0x10 * p, &h[p], 0);
	}
	CHECK(f->nlog == 4 && f->log[0] == JT_SUBMIT2, "after a refusal 0x7c was asked again "
	    "(%u calls, first %#x)", f->nlog, f->nlog ? f->log[0] : 0);
	f->refuse = 0;
	jackpotlte_drm_attach(NULL);
	jt_gpu_close(g);
}

int
main(void)
{
	test_open();
	test_create_bo();
	test_map();
	test_done_page();
	test_submit();
	test_waits();
	test_param_and_release();
	test_madvise();
	test_surface();
	test_drm_ioctl();
	test_drm_misc();
	test_batch();
	test_errno_table();

	if (failures != 0) {
		printf("FAIL: %u check%s failed\n", failures, failures == 1 ? "" : "s");
		return 1;
	}
	printf("PASS: the request layer asks 0154 what 0154 expects\n");
	return 0;
}
