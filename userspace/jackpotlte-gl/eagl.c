/* SPDX-License-Identifier: MIT */
/*
 * libjackpotlte-gl.dylib: an OpenGL ES implementation behind iOS's EAGLContext,
 * for the machine CoreAnimation draws on when it has no Metal.
 *
 * CoreAnimation's render server draws with Metal, or else with OpenGL ES, or
 * else with its own software rasteriser (CA::WindowServer::EAGLServer::renderer()).
 * With no Metal plug-in on this phone (docs/jackpotlte-mali.md section 6), it
 * reaches for OpenGL ES, and asks an EAGLContext for a handful of things.  All
 * of them are replaced here, so the context it gets is one of the library
 * behind this file (jackpotlte_gl_osmesa.c: Mesa's OSMesa with the phone's
 * Mali) rather than Apple's driver:
 *
 *   -initWithAPI:properties:   makes a context of the OpenGL ES version asked for
 *                              (and the -sharegroup and -sharedWithCompute:
 *                              forms of the same initialiser)
 *   -API                       that version
 *   -sharegroup                nil: nothing here shares a context (CoreImage asks;
 *                              its own accessor reads ivars of an object this
 *                              shim never initialised, so it must be answered)
 *   -setParameter:to:          noted; the value is not read
 *   -getParameter:to:          the same, in the other direction
 *   -getMacroContextPrivate    the table of functions the caller invokes as
 *                              entry(table[0], arguments...), at the offsets of
 *                              the build's own OpenGLES (eagl_slots.txt); entry
 *                              0 is this context, the rest are gl_slots.c's
 *   -texImageIOSurface:...     the texture bound to the target is to show what
 *                              the IOSurface holds (both forms the build has,
 *                              with and without `invert`)
 *   -attachImage:toCoreSurface:invertedRender:
 *                              the texture bound to the target is to be the
 *                              surface's memory (CoreAnimation then draws into
 *                              it), or to show it once
 *   -renderbufferStorage:fromDrawable:
 *                              noted, YES: whether this build takes that path
 *   -presentRenderbuffer:      (and its atTime: and afterMinimumDuration: forms)
 *                              the frame the caller has drawn is handed to the
 *                              GPU - see below
 *   +setCurrentContext:        noted; every GL entry already carries its context
 *   +currentContext            what they noted
 *   -dealloc                   the context goes
 *
 * Both surface methods go through the same path: the surface's ID is handed to
 * glEGLImageTargetTexture2DOES, which 0003's OSMesa frontend hook turns into a
 * buffer of that surface (0154's 0x7e) - no copy, the display's memory itself.
 * A surface that cannot be one (not one plane of 32-bit BGRA pixels) is logged
 * and refused rather than copied, except -attachImage: of a 4-byte one-plane
 * surface (Control Center masks), which is copied as BGRA: a
 * different bug.
 *
 * The present methods are what makes a drawn frame exist.  Apple's context
 * presents by handing its renderbuffer to the window system; there is no
 * renderbuffer here, and the surface CoreAnimation drew into is composited by
 * the kernel's own display path (0046), so the part left to do is what Apple's
 * driver does first: get the batch to the GPU.  Mesa submits on a flush, and a
 * GL client that only draws submits nothing - which is what the jt60 log shows,
 * ending at glDrawElements with no job behind it.
 *
 * Nothing here is called by Apple's code paths that this shim does not replace:
 * the class's ivars are never filled in (there is no call to super's -init), so
 * every method CoreAnimation reaches must be one of the ones above.
 */
#include "jackpotlte_gl.h"

#include <fcntl.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach_time.h>
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

void trace_install(void *table[JACKPOTLTE_GL_SLOTS]);      /* trace.c */
void trace_frame(void);                                     /* trace.c */

/*
 * IOSurface's accessors, declared here: the iPhoneOS SDK ships no header for
 * them (the framework is there, the header is not), the same arrangement the
 * iOS backend uses for IOKit (userspace/libjackpotlte/jackpotlte_iokit.c).  The
 * names are the framework's and the process that loads this library has it.
 */
typedef struct __IOSurface *IOSurfaceRef;
uint32_t IOSurfaceGetID(IOSurfaceRef surface);
uint32_t IOSurfaceGetPixelFormat(IOSurfaceRef surface);
size_t IOSurfaceGetPlaneCount(IOSurfaceRef surface);
size_t IOSurfaceGetBytesPerElement(IOSurfaceRef surface);
size_t IOSurfaceGetBytesPerRow(IOSurfaceRef surface);
size_t IOSurfaceGetAllocSize(IOSurfaceRef surface);
size_t IOSurfaceGetWidth(IOSurfaceRef surface);
size_t IOSurfaceGetHeight(IOSurfaceRef surface);
void *IOSurfaceGetBaseAddress(IOSurfaceRef surface);
int IOSurfaceLock(IOSurfaceRef surface, uint32_t options, uint32_t *seed);
int IOSurfaceUnlock(IOSurfaceRef surface, uint32_t options, uint32_t *seed);
uint32_t IOSurfaceGetSeed(IOSurfaceRef surface);
const void *CFRetain(const void *object);
void CFRelease(const void *object);

/* The GL names this file itself uses. */
#define GL_TEXTURE_2D             0x0de1
#define GL_TEXTURE_BINDING_2D     0x8069
#define GL_VENDOR                 0x1f00
#define GL_RENDERER               0x1f01
#define GL_VERSION                0x1f02

/* glTexImage2D's formats CoreAnimation hands -texImageIOSurface: (the surface's
 * own pixel format is not part of that call: these are). */
#define GL_ALPHA                  0x1906
#define GL_RGB                    0x1907
#define GL_RGBA                   0x1908
#define GL_BGRA                   0x80e1
#define GL_LUMINANCE              0x1909
#define GL_LUMINANCE_ALPHA        0x190a
#define GL_UNSIGNED_BYTE          0x1401
#define GL_UNSIGNED_SHORT_5_6_5   0x8363
#define GL_UNSIGNED_SHORT_4_4_4_4 0x8033
#define GL_UNSIGNED_SHORT_5_5_5_1 0x8034
#define GL_FLOAT                  0x1406
#define GL_HALF_FLOAT             0x140b
#define GL_RGBA8                  0x8058
#define GL_RGBA16F                0x881a
#define GL_RGBA32F                0x8814
#define GL_TEXTURE_MIN_FILTER     0x2801
#define GL_TEXTURE_MAG_FILTER     0x2800
#define GL_LINEAR                 0x2601

/* kIOSurfaceLockReadOnly. */
#define SURFACE_LOCK_READ_ONLY    0x1

/* As many contexts as the display server has been seen to make: the renderer's
 * own and CoreImage's.  A table this small keeps the lookup in context_of() a
 * handful of compares, and a full one is logged instead of growing. */
#define CONTEXTS 8

struct jackpotlte_eagl {
   void *table[JACKPOTLTE_GL_SLOTS];   /* [0] is this: what every entry is handed */
   id object;                          /* the EAGLContext this stands for */
   struct jackpotlte_gl *gl;
   unsigned long api;
};

static struct jackpotlte_eagl *contexts[CONTEXTS];

/* The library's functions this file calls itself (never by name: see
 * jackpotlte_gl_osmesa.c). */
static struct {
   void (*get_integerv)(unsigned pname, int *params);
   const unsigned char *(*get_string)(unsigned name);
} helpers;

static void
say(const char *format, ...)
{
   char line[512];
   va_list arguments;

   va_start(arguments, format);
   (void)vsnprintf(line, sizeof(line), format, arguments);
   va_end(arguments);
   fprintf(stderr, "jackpotlte-gl: %s\n", line);
   jackpotlte_gl_log(line);
}

/*
 * The log, doubled onto the kernel console when the boot-arg jtgllog=1 is
 * there: this library is loaded into the display server, whose stderr is not
 * anywhere a bring-up can read, while the kernel console is (it is what draws
 * a panic, and it is what the persistent ring keeps).  The switch is read once,
 * out of kern.bootargs; with it absent, nothing is opened and nothing extra is
 * written.  The kernel's own exec injector logs to the same switch.
 */
void
jackpotlte_gl_log(const char *line)
{
   static int looked, console = -1;

   if (!looked) {
      char args[1024];
      size_t size = sizeof(args);
      const char *p;

      looked = 1;
      if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && size > 0) {
         args[size < sizeof(args) ? size : sizeof(args) - 1] = '\0';
         for (p = strstr(args, "jtgllog="); p != NULL; p = strstr(p + 8, "jtgllog=")) {
            if (p[8] == '1' && (p[9] == '\0' || p[9] == ' ')) {
               console = open("/dev/console", O_WRONLY | O_APPEND);
               break;
            }
         }
      }
   }
   if (console >= 0) {
      char out[600];
      int n = snprintf(out, sizeof(out), "jackpotlte-gl: %s\n", line);

      if (n > 0) {
         (void)write(console, out,
             (size_t)(n < (int)sizeof(out) ? n : (int)sizeof(out) - 1));
      }
   }
}

/* A slot the build exports no _gl* name for was called (gl_slots.c). */
void
jackpotlte_gl_note_unsupported(unsigned offset)
{
   static unsigned noted;

   if (noted < 32) {
      say("slot %#06x has no name in this build: a call to it did nothing", offset);
      noted++;
   }
}

/*
 * Every entry of the table calls this before it passes its call on: the entry's
 * first argument is the context the caller means, and a GL call runs on the
 * thread where its context is current.  CoreAnimation's renderer is not always
 * the thread that made the context.
 */
void
jackpotlte_gl_use(void *context)
{
   struct jackpotlte_eagl *e = context;

   if (e != NULL && e->gl != NULL)
      jackpotlte_gl_make_current(e->gl);
}

/*
 * The dispatch table's drawing entries call this after the call is passed on.
 * Apple's driver submits a draw to the GPU as it is made; the library behind
 * this table keeps it in a batch until a flush, so a renderer that never
 * flushes (the display server, jt60: a full first frame drawn, no job to the
 * GPU) would leave its frame in the library's command stream forever.  One
 * flush per draw is expensive and is exactly what a bring-up wants: a drawn
 * frame becomes a real one, and the driver's own job log says so.
 */
/*
 * a8-gpu-cache: the Mali-G71 here is not cache-coherent with the CPUs, so a
 * surface that another process drew with the CPU can still sit in the CPU
 * caches when the GPU reads it as a "memory shared" texture: the GPU then
 * samples stale RAM.  The Galaxy S7 port found the same thing in its shim
 * (egemengunel/ios-a5xelte HEROLTE_PROGRESS.md: "0 textures refilled") and
 * cleaned the IOSurface's cache when its seed changed and every 30 frames.
 *
 * Here every surface that becomes a shared texture or a render target is
 * remembered, and before each flush (when Mesa hands the batch to the GPU) a
 * surface whose seed moved, or every SYNC_EVERY draws any surface, is written
 * back and invalidated with sys_dcache_flush (DC CIVAC by address).
 *
 * The other direction matters as much: the kernel's framebuffer copies a
 * render target to the panel with the CPU as soon as it is swapped, without
 * waiting for the GPU and without touching the caches (this kernel has no
 * HakiosMaliDrawn() like the S9+ port's, danstronics/ios-s9plus-exynos).  So
 * at the end of each frame the GPU is waited for (glFinish) and the render
 * targets are cleaned and invalidated, as the S7 and S9+ shims do in their
 * frame_end() (boot-arg glwait there).
 *
 * Boot-arg jtglsync: 0 = off (the old behaviour), 1 = textures cleaned before
 * the GPU runs, the GPU waited for at the end of each frame and the render
 * targets cleaned after it (default), 2 = that after every single draw too.
 * 2 is very slow; it is a test of whether the frames are right at all.
 */
#define SHARED_MAX 32
#define SYNC_EVERY 120
static struct {
   IOSurfaceRef surface;               /* retained while it is in here */
   void *owner;                        /* the context whose texture it is */
   int texture;                        /* that texture's name */
   uint32_t id, seed;
   int target;                         /* drawn into (-attachImage:) */
} shared[SHARED_MAX];
static unsigned shared_next;

/*
 * glca9: textures made as a copy of a surface (surface_copy_texture: pixel
 * format 0 masks, alpha and luminance-alpha surfaces). A copy is a snapshot,
 * and CoreAnimation binds the texture again without asking for the surface
 * again, so a surface its writer filled after the first copy stayed empty on
 * screen (apps' SF Symbols: the back chevron, search glass, slider suns, a
 * 33x57 RGBA8 texture whose alpha was 0 everywhere in a trace). The copy is
 * remembered here and made again when the texture is bound and the surface's
 * seed has moved.
 */
#define COPIED_MAX 96
static struct {
   IOSurfaceRef surface;               /* retained while it is in here */
   void *owner;
   int texture;
   uint32_t seed;
   int width, height;
   unsigned long internal, format, type;
   BOOL invert;
   int target;                         /* made by -attachImage: (drawn into) */
   int told;                           /* glca9c: its first draw was said */
   int empty;                          /* glca9d: the copy had alpha 0 everywhere */
   unsigned tries;                     /* copies made again while it stayed empty */
   uint64_t next_try;
} copied[COPIED_MAX];
static int last_copy_alpha = -1;       /* alpha_max of the last surface copy */
static unsigned copied_next, copied_used;
static unsigned long recopies, reordered;
static unsigned long draws, cleans, finishes, frames, forgotten;
static int frame_drawn;                /* a draw since the last frame_end() */

static int
boot_arg_int(const char *name, int fallback)
{
   char args[1024], key[64];
   size_t size = sizeof(args);
   const char *p;

   if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0 || size == 0)
      return fallback;
   args[size < sizeof(args) ? size : sizeof(args) - 1] = '\0';
   (void)snprintf(key, sizeof(key), "%s=", name);
   for (p = strstr(args, key); p != NULL; p = strstr(p + 1, key)) {
      if (p == args || p[-1] == ' ')
         return atoi(p + strlen(key));
   }
   return fallback;
}

static int
sync_mode(void)
{
   static int mode = -1;

   if (mode < 0) {
      mode = boot_arg_int("jtglsync", 1);
      say("cache: jtglsync=%d (0 off, 1 clean before the GPU and wait at the "
          "end of a frame, 2 wait after every draw)", mode);
   }
   return mode;
}

/*
 * a8-gpu2: one batch a frame.  The flush after every draw (below) is how the
 * bring-up got a frame to the GPU at all, but it makes each draw its own
 * fragment job, and a fragment job loads and stores the whole 1080x2220 render
 * target: the lock screen's first frame (a8log-2, frame 10: 57 draws) was 57
 * full-screen passes.  CoreAnimation ends every frame with glFenceSync, which
 * submits the batch, and frame_end() waits for the GPU there (jtglsync >= 1),
 * so the frame still reaches the GPU before the swap.
 *
 * Boot-arg jtglflush: 1 = flush after every draw (default, the old behaviour),
 * 0 = no flush after a draw: one batch a render target a frame.
 */
static int
flush_mode(void)
{
   static int mode = -1;

   if (mode < 0) {
      /* glca3: one batch a frame unless asked otherwise.  The boot-args lost
       * jtglflush=0 with kernel 0196 (room for a8fix=1), and the old default
       * then flushed every draw as a full-screen GPU pass (23-46 ms a frame
       * with Control Center up).  The file a8gl-flush1 brings that back. */
      mode = boot_arg_int("jtglflush", -1);
      if (mode < 0)
         mode = access("/private/var/mobile/Library/a8gl-flush1", F_OK) == 0 ? 1 : 0;
      say("flush: jtglflush=%d (1 a flush after every draw, 0 one batch a "
          "frame)", mode);
   }
   return mode;
}

/*
 * a8-gpu2: what a frame costs, from the display server's side.  Every 10 s in
 * which frames were drawn: how many, the gaps between them (a gap during an
 * animation is the frame time), and the time spent waiting for the GPU
 * (frame_end's glFinish), in the per-draw flushes and in cache maintenance.
 * Boot-arg jtglperf=0 turns it off.
 */
static mach_timebase_info_data_t timebase;
static struct {
   uint64_t start, last;               /* mach time: window start, last frame */
   unsigned long frames, draws;
   unsigned long gaps[5];              /* <=17, <=34, <=50, <=100, >100 ms */
   uint64_t wait, wait_max, flush, clean;      /* mach ticks */
   uint64_t build, between;            /* glca6: first draw -> frame end, frame end -> next first draw */
   uint64_t frame_start, frame_done;   /* mach time of this frame's first draw, of the last frame's end */
   unsigned long on_big, on_cpu;       /* glca8: draws made on the big cores (cpu 6, 7), of all sampled */
   unsigned long on[8];                /* draws made on each cpu */
} perf;

/* the CPU this thread runs on (XNU keeps it in TPIDRRO_EL0's low bits) */
static unsigned
cpu_now(void)
{
#if defined(__arm64__) || defined(__aarch64__)
   uint64_t v;

   __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(v));
   return (unsigned)(v & 7);
#else
   return 0;                           /* (the host test build) */
#endif
}
static unsigned long perf_lines;

static uint64_t
ticks_us(uint64_t ticks)
{
   if (timebase.denom == 0)
      (void)mach_timebase_info(&timebase);
   return timebase.denom == 0 ? 0 : ticks * timebase.numer / timebase.denom / 1000;
}

static int
perf_on(void)
{
   static int on = -1;

   if (on < 0)
      on = boot_arg_int("jtglperf", 1);
   return on;
}

/* CoreAnimation's glFenceSync: a frame is about to be swapped. */
static void
perf_frame(void)
{
   uint64_t now, us, gap;
   unsigned long n;

   if (!perf_on())
      return;
   now = mach_absolute_time();
   if (perf.start == 0)
      perf.start = now;
   if (perf.last != 0) {
      gap = ticks_us(now - perf.last) / 1000;
      perf.gaps[gap <= 17 ? 0 : gap <= 34 ? 1 : gap <= 50 ? 2 : gap <= 100 ? 3 : 4]++;
   }
   perf.last = now;
   perf.frames++;
   us = ticks_us(now - perf.start);
   if (us < 10000000ull)
      return;
   n = perf.frames;
   if (perf_lines++ < 3000)
      say("perf: %llu ms %lu fr %lu dr/fr; gaps 17/34/50/100+ %lu/%lu/%lu/%lu/%lu; "
          "gpu %llu us (max %llu) build %llu btw %llu flush %llu cache %llu; big %lu%% cpus %lu %lu %lu %lu %lu %lu %lu %lu; f%d s%d",
          (unsigned long long)(us / 1000), n, perf.draws / n,
          perf.gaps[0], perf.gaps[1], perf.gaps[2], perf.gaps[3], perf.gaps[4],
          (unsigned long long)(ticks_us(perf.wait) / n),
          (unsigned long long)ticks_us(perf.wait_max),
          (unsigned long long)(ticks_us(perf.build) / n),
          (unsigned long long)(ticks_us(perf.between) / n),
          (unsigned long long)(ticks_us(perf.flush) / n),
          (unsigned long long)(ticks_us(perf.clean) / n),
          perf.on_cpu ? perf.on_big * 100 / perf.on_cpu : 0,
          perf.on[0], perf.on[1], perf.on[2], perf.on[3], perf.on[4], perf.on[5], perf.on[6], perf.on[7],
          flush_mode(), sync_mode());
   {
      uint64_t done = perf.frame_done;

      memset(&perf, 0, sizeof(perf));
      perf.frame_done = done;
   }
   perf.start = now;
   perf.last = now;
}

static void
clean_surface(unsigned i, const char *why)
{
   uint32_t seed = 0;
   void *base;
   size_t length;
   uint64_t t0;

   /*
    * DC CIVAC writes the line back.  A read-only lock is not enough: CoreAnimation
    * client images are often mapped prot 1/1 (tag 51), and the writeback is a
    * SIGBUS (libjackpotlte-gl+0x78ec on the upload path that just noted them).
    * Take a writable lock, or skip the flush.
    */
   if (IOSurfaceLock(shared[i].surface, 0, &seed) != 0) {
      if (cleans < 40)
         say("cache: surface %u not writable, skip dcache (%s)", shared[i].id, why);
      return;
   }
   base = IOSurfaceGetBaseAddress(shared[i].surface);
   length = IOSurfaceGetAllocSize(shared[i].surface);
   t0 = mach_absolute_time();
   if (base != NULL && length != 0)
      sys_dcache_flush(base, length);
   perf.clean += mach_absolute_time() - t0;
   (void)IOSurfaceUnlock(shared[i].surface, 0, &seed);
   /*
    * glca5: the writable unlock above moves the surface's seed itself.  Not
    * noting that made shared_sync() (after every draw) see a "seed changed"
    * on every surface it had just cleaned and clean it again: both render
    * targets after every draw, about 145 lock/clean/unlock rounds a frame
    * (~290 kernel calls, a quarter of each frame in IOSurfaceClientUnlock in
    * a backboardd sample).  Only another writer's change counts now.
    */
   shared[i].seed = IOSurfaceGetSeed(shared[i].surface);
   cleans++;
   if (cleans <= 40 || cleans % 500 == 0)
      say("cache: cleaned surface %u, %zu bytes at %p (%s, %s), %lu cleans",
          shared[i].id, length, base, why,
          shared[i].target ? "render target" : "texture", cleans);
}

static unsigned
shared_count(void)
{
   unsigned i, n = 0;

   for (i = 0; i < SHARED_MAX; i++)
      n += shared[i].surface != NULL;
   return n;
}

/* A texture of `owner`'s now shows `surface` (in place, memory shared). */
static void
shared_note(void *owner, int texture, IOSurfaceRef surface, int target)
{
   uint32_t id;
   unsigned i, slot = SHARED_MAX;

   if (sync_mode() == 0 || surface == NULL)
      return;
   id = (uint32_t)IOSurfaceGetID(surface);
   for (i = 0; i < SHARED_MAX; i++) {
      if (shared[i].surface != NULL && shared[i].owner == owner &&
          shared[i].texture == texture) {
         if (shared[i].id == id) {
            shared[i].target |= target;
            return;
         }
         slot = i;                     /* the texture shows another surface now */
         break;
      }
   }
   for (i = 0; slot == SHARED_MAX && i < SHARED_MAX; i++) {
      if (shared[i].surface == NULL)
         slot = i;
   }
   if (slot == SHARED_MAX)
      slot = shared_next++ % SHARED_MAX;   /* full: an old one goes */
   if (shared[slot].surface != NULL)
      CFRelease(shared[slot].surface);
   shared[slot].surface = (IOSurfaceRef)CFRetain(surface);
   shared[slot].owner = owner;
   shared[slot].texture = texture;
   shared[slot].id = id;
   shared[slot].seed = IOSurfaceGetSeed(surface);
   shared[slot].target = target;
   clean_surface(slot, target ? "new render target" : "new texture");
}

/* The texture is deleted: let its surface go (backboardd must not keep them). */
static void
shared_forget(void *owner, int texture)
{
   unsigned i;

   for (i = 0; i < SHARED_MAX; i++) {
      if (shared[i].surface != NULL && shared[i].owner == owner &&
          shared[i].texture == texture) {
         CFRelease(shared[i].surface);
         shared[i].surface = NULL;
         forgotten++;
      }
   }
   for (i = 0; copied_used != 0 && i < COPIED_MAX; i++) {
      if (copied[i].surface != NULL && copied[i].owner == owner &&
          copied[i].texture == texture) {
         CFRelease(copied[i].surface);
         copied[i].surface = NULL;
         copied_used--;
      }
   }
}

static void
shared_sync(void)
{
   static unsigned long calls;
   unsigned i;
   /* the safety net: every surface now and then (every 120 draws, or every
    * 30 frames when this runs once a frame) */
   int every = ++calls % (flush_mode() == 0 ? 30 : SYNC_EVERY) == 0;

   for (i = 0; i < SHARED_MAX; i++) {
      uint32_t seed;

      if (shared[i].surface == NULL)
         continue;
      seed = IOSurfaceGetSeed(shared[i].surface);
      if (seed != shared[i].seed) {
         shared[i].seed = seed;
         clean_surface(i, "seed changed");
      } else if (every) {
         clean_surface(i, "periodic");
      }
   }
}

/*
 * The end of a frame: wait for the GPU (unless the caller just did), then
 * clean and invalidate the render targets so the kernel's present copy reads
 * what the GPU wrote and not lines the CPU fetched before.  Nothing writes a
 * render target with the CPU between frames, so the clean writes nothing back.
 */
static void
frame_end(const char *why, int finished)
{
   unsigned i, targets = 0;

   if (sync_mode() == 0 || !frame_drawn)
      return;
   frame_drawn = 0;
   if (perf.frame_start != 0)
      perf.build += mach_absolute_time() - perf.frame_start;
   if (flush_mode() == 0)
      shared_sync();               /* before the batch goes to the GPU (glFinish below) */
   if (!finished) {
      void (*finish)(void) = (void (*)(void))jackpotlte_gl_proc("glFinish");

      if (finish != NULL) {
         uint64_t t0 = mach_absolute_time(), t;

         finish();
         finishes++;
         t = mach_absolute_time() - t0;
         perf.wait += t;
         if (t > perf.wait_max)
            perf.wait_max = t;
      }
   }
   for (i = 0; i < SHARED_MAX; i++) {
      if (shared[i].surface != NULL && shared[i].target) {
         clean_surface(i, "frame drawn");
         targets++;
      }
   }
   frames++;
   perf.frame_done = mach_absolute_time();
   if (frames <= 30 || frames % 300 == 0)
      say("frame %lu ended by %s: %lu draws, %u render targets cleaned, "
          "%lu GPU waits, %u surfaces kept, %lu let go, %u copies, %lu made again",
          frames, why, draws, targets, finishes, shared_count(), forgotten,
          copied_used, recopies);
}

/*
 * The dispatch table's drawing entries call this after the call is passed on.
 * Apple's driver submits a draw to the GPU as it is made; the library behind
 * this table keeps it in a batch until a flush, so a renderer that never
 * flushes (the display server, jt60: a full first frame drawn, no job to the
 * GPU) would leave its frame in the library's command stream forever.  One
 * flush per draw is expensive and is exactly what a bring-up wants: a drawn
 * frame becomes a real one, and the driver's own job log says so.
 *
 * a8-gpu-cache: the caches are cleaned first, because the GPU reads the
 * surfaces only once the flush has submitted the batch.  A draw also marks the
 * frame as drawn, for frame_end().
 */
unsigned ca_fixes_function(void);      /* ca_fixes.c */
static int resolve_helpers(void);

/*
 * glca9c: a surface CoreAnimation attaches to draw into, but that is not BGRA,
 * is a copy here, and apps' SF Symbols came out of such copies with nothing in
 * them (33x57, alpha 0 everywhere).  Say, once for each, what draws into one.
 */
static void
copy_target_draw(void)
{
   static void (*attachment)(unsigned, unsigned, unsigned, int *);
   static unsigned said;
   int framebuffer = 0, kind = 0, name = 0, program = 0;
   unsigned i, function;

   if (copied_used == 0 || said >= 60 || !resolve_helpers())
      return;
   if (attachment == NULL) {
      attachment = (void (*)(unsigned, unsigned, unsigned, int *))
         jackpotlte_gl_proc("glGetFramebufferAttachmentParameteriv");
      if (attachment == NULL)
         return;
   }
   helpers.get_integerv(0x8ca6 /* GL_FRAMEBUFFER_BINDING */, &framebuffer);
   if (framebuffer == 0)
      return;
   attachment(0x8d40 /* GL_FRAMEBUFFER */, 0x8ce0 /* COLOR_ATTACHMENT0 */,
              0x8cd0 /* ATTACHMENT_OBJECT_TYPE */, &kind);
   if (kind != 0x1702 /* GL_TEXTURE */)
      return;
   attachment(0x8d40, 0x8ce0, 0x8cd1 /* ATTACHMENT_OBJECT_NAME */, &name);
   for (i = 0; i < COPIED_MAX; i++) {
      if (copied[i].surface == NULL || copied[i].texture != name || copied[i].told)
         continue;
      copied[i].told = 1;
      said++;
      helpers.get_integerv(0x8b8d /* GL_CURRENT_PROGRAM */, &program);
      function = ca_fixes_function();
      say("copy: a draw into texture %d (copy of surface %u, %zux%zu, %s): CA function %u, "
          "program %d", name, (unsigned)IOSurfaceGetID(copied[i].surface),
          IOSurfaceGetWidth(copied[i].surface), IOSurfaceGetHeight(copied[i].surface),
          copied[i].target ? "attached" : "sampled", function, program);
      break;
   }
}

void
jackpotlte_gl_after_draw(void)
{
   copy_target_draw();

   void (*flush)(void) = (void (*)(void))jackpotlte_gl_proc("glFlush");
   int mode = sync_mode();

   draws++;
   perf.draws++;
   perf.on_cpu++;
   {
      unsigned c = cpu_now();

      perf.on_big += c >= 6;
      perf.on[c & 7]++;
   }
   if (!frame_drawn) {
      perf.frame_start = mach_absolute_time();
      if (perf.frame_done != 0)
         perf.between += perf.frame_start - perf.frame_done;
   }
   frame_drawn = 1;
   /*
    * glca7: with one batch a frame (jtglflush=0) nothing reaches the GPU
    * before frame_end(), so the textures are synced there, once a frame,
    * instead of after every draw (32 surfaces' seeds read 80 times a frame).
    */
   if (mode != 0 && flush_mode() != 0)
      shared_sync();
   if (flush != NULL && flush_mode() != 0) {
      uint64_t t0 = mach_absolute_time();

      flush();
      perf.flush += mach_absolute_time() - t0;
   }
   if (mode == 2)
      frame_end("a draw (jtglsync=2)", 0);
   if (draws <= 20 || draws % 600 == 0)
      say("draws: %lu, %u surfaces kept, %lu cleans, %lu GPU waits, %lu frames",
          draws, shared_count(), cleans, finishes, frames);
}

/* ---- entries CoreAnimation calls that the library answered wrongly ------ */

/*
 * glFlush, glFinish and glFenceSync end a frame: CoreAnimation makes a fence
 * right before each swap (the a5log of the first GPU boot: glFenceSync, then
 * "SWEET FB: swap 1").  They go through frame_end() before they are passed on.
 */
static void
my_glFlush(void *context)
{
   static void (*real)(void);

   jackpotlte_gl_use(context);
   if (real == NULL)
      real = (void (*)(void))jackpotlte_gl_proc("glFlush");
   if (sync_mode() != 0)
      shared_sync();
   if (real != NULL)
      real();
   frame_end("glFlush", 0);
}

static void
my_glFinish(void *context)
{
   static void (*real)(void);

   jackpotlte_gl_use(context);
   if (real == NULL)
      real = (void (*)(void))jackpotlte_gl_proc("glFinish");
   if (sync_mode() != 0)
      shared_sync();
   if (real != NULL) {
      real();
      finishes++;
   }
   frame_end("glFinish", 1);
}

static GLsync
my_glFenceSync(void *context, GLenum condition, GLbitfield flags)
{
   static GLsync (*real)(GLenum, GLbitfield);

   jackpotlte_gl_use(context);
   if (real == NULL)
      real = (GLsync (*)(GLenum, GLbitfield))jackpotlte_gl_proc("glFenceSync");
   frame_end("glFenceSync", 0);
   perf_frame();
   trace_frame();
   return real != NULL ? real(condition, flags) : NULL;
}

/*
 * GL_APPLE_fence and the object waits.  Mesa has none of them, so the table's
 * entries found no function: every test answered 0 ("the GPU is still busy")
 * and every wait did nothing.  iOS 16's CoreAnimation calls
 * glFinishObjectAPPLE before it deletes a render target (the Galaxy S7 port,
 * wardevsa/ios-a5xelte gpu/eagl/src/gl_resolve.c, which answers them the
 * same way); the first GPU boot's log has it too.  Everything is done after
 * glFinish, so a wait finishes the GPU and a test answers yes.
 */
static void
my_glGenFencesAPPLE(void *context, GLsizei n, GLuint *fences)
{
   static GLuint next = 1;
   GLsizei i;

   (void)context;
   for (i = 0; fences != NULL && i < n; i++)
      fences[i] = next++;
}

static void
my_glDeleteFencesAPPLE(void *context, GLsizei n, const GLuint *fences)
{
   (void)context;
   (void)n;
   (void)fences;
}

static void
my_glSetFenceAPPLE(void *context, GLuint fence)
{
   (void)fence;
   my_glFlush(context);
}

static GLboolean
my_glIsFenceAPPLE(void *context, GLuint fence)
{
   (void)context;
   return fence != 0;
}

static GLboolean
my_glTestFenceAPPLE(void *context, GLuint fence)
{
   (void)fence;
   my_glFinish(context);
   return 1;
}

static void
my_glFinishFenceAPPLE(void *context, GLuint fence)
{
   (void)fence;
   my_glFinish(context);
}

static GLboolean
my_glTestObjectAPPLE(void *context, GLenum object, GLuint name)
{
   (void)object;
   (void)name;
   my_glFinish(context);
   return 1;
}

static void
my_glFinishObjectAPPLE(void *context, GLenum object, GLint name)
{
   static unsigned shown;

   if (shown++ < 8)
      say("glFinishObjectAPPLE(%#x, %d): the GPU is waited for", object, name);
   my_glFinish(context);
}

static void
my_glDeleteTextures(void *context, GLsizei n, const GLuint *textures)
{
   static void (*real)(GLsizei, const GLuint *);
   GLsizei i;

   jackpotlte_gl_use(context);
   if (real == NULL)
      real = (void (*)(GLsizei, const GLuint *))jackpotlte_gl_proc("glDeleteTextures");
   for (i = 0; textures != NULL && i < n; i++)
      shared_forget(context, (int)textures[i]);
   if (real != NULL)
      real(n, textures);
}

/* Offsets from eagl_slots.txt (gl_slots.c): index = offset / 8. */
void ca_fixes_install(void *table[JACKPOTLTE_GL_SLOTS]);   /* ca_fixes.c */

static void my_glBindTexture(void *context, GLenum target, GLuint texture);
static void my_glTexImage2D(void *context, GLenum target, GLint level, GLint internal, GLsizei w,
                            GLsizei h, GLint border, GLenum format, GLenum type, const void *pixels);
static void my_glTexSubImage2D(void *context, GLenum target, GLint level, GLint x, GLint y,
                               GLsizei w, GLsizei h, GLenum format, GLenum type,
                               const void *pixels);

static void
install_fixes(void *table[JACKPOTLTE_GL_SLOTS])
{
   table[0x006] = (void *)my_glBindTexture;          /* 0x0030 */
   table[0x12e] = (void *)my_glTexImage2D;           /* 0x0970 */
   table[0x134] = (void *)my_glTexSubImage2D;        /* 0x09a0 */
   table[0x03c] = (void *)my_glDeleteTextures;       /* 0x01e0 */
   table[0x05a] = (void *)my_glFinish;               /* 0x02d0 */
   table[0x05b] = (void *)my_glFlush;                /* 0x02d8 */
   table[0x1d0] = (void *)my_glGenFencesAPPLE;       /* 0x0e80 */
   table[0x1d1] = (void *)my_glDeleteFencesAPPLE;    /* 0x0e88 */
   table[0x1d2] = (void *)my_glSetFenceAPPLE;        /* 0x0e90 */
   table[0x1d3] = (void *)my_glIsFenceAPPLE;         /* 0x0e98 */
   table[0x1d4] = (void *)my_glTestFenceAPPLE;       /* 0x0ea0 */
   table[0x1d5] = (void *)my_glFinishFenceAPPLE;     /* 0x0ea8 */
   table[0x1d6] = (void *)my_glTestObjectAPPLE;      /* 0x0eb0 */
   table[0x1d7] = (void *)my_glFinishObjectAPPLE;    /* 0x0eb8 */
   table[0x30c] = (void *)my_glFenceSync;            /* 0x1860 */
   ca_fixes_install(table);
   trace_install(table);
}

static struct jackpotlte_eagl *
context_of(id object)
{
   unsigned i;

   for (i = 0; i < CONTEXTS; i++) {
      if (contexts[i] != NULL && contexts[i]->object == object)
         return contexts[i];
   }
   return NULL;
}

static int
resolve_helpers(void)
{
   if (helpers.get_integerv != NULL)
      return 1;
   helpers.get_integerv = (void (*)(unsigned, int *))
      jackpotlte_gl_proc("glGetIntegerv");
   helpers.get_string = (const unsigned char *(*)(unsigned))
      jackpotlte_gl_proc("glGetString");
   return helpers.get_integerv != NULL;
}

/* ---- EAGLContext --------------------------------------------------------- */

/* The one initialiser all three selectors end in. */
static id
make_context(id self, unsigned long api)
{
   struct jackpotlte_eagl *e;
   unsigned place;

   for (place = 0; place < CONTEXTS && contexts[place] != NULL; place++)
      ;
   if (api < 2 || api > 3 || place == CONTEXTS) {
      say("no context of API %lu (%u of %u in use)", api, place, (unsigned)CONTEXTS);
      return nil;
   }
   e = calloc(1, sizeof(*e));
   if (e == NULL)
      return nil;
   e->gl = jackpotlte_gl_create((int)api);
   if (e->gl == NULL || !jackpotlte_gl_make_current(e->gl)) {
      say("%s gives no OpenGL ES %lu context", jackpotlte_gl_library(), api);
      jackpotlte_gl_destroy(e->gl);
      free(e);
      return nil;
   }
   e->object = self;
   e->api = api;
   e->table[0] = e;
   jackpotlte_gl_fill_table(e->table);
   install_fixes(e->table);
   contexts[place] = e;

   if (resolve_helpers()) {
      say("an OpenGL ES %lu context for CoreAnimation by %s: %s; %s", api,
          jackpotlte_gl_library(), (const char *)helpers.get_string(GL_VERSION),
          (const char *)helpers.get_string(GL_RENDERER));
   } else {
      say("an OpenGL ES %lu context for CoreAnimation by %s; its glGetString is "
          "missing", api, jackpotlte_gl_library());
   }
   return self;
}

/*
 * Three ways in.  -initWithAPI:properties: is the one CoreAnimation's renderer
 * uses; the other two are the public ones, replaced so that a context made with
 * either is also one of ours - an object made by Apple's initialiser would have
 * none of this file's state, and -getMacroContextPrivate would answer NULL.
 */
static id
my_initWithAPI_properties(id self, SEL selector, unsigned long api, id properties)
{
   (void)selector;
   (void)properties;
   return make_context(self, api);
}

static id
my_initWithAPI(id self, SEL selector, unsigned long api)
{
   (void)selector;
   return make_context(self, api);
}

static id
my_initWithAPI_sharegroup(id self, SEL selector, unsigned long api, id sharegroup)
{
   (void)selector;
   (void)sharegroup;
   return make_context(self, api);
}

/* The third form: the same context, with a flag that asks for a sharegroup
 * that is only a compute context's.  Nothing here shares, so the flag is
 * noted by being ignored. */
static id
my_initWithAPI_sharedWithCompute(id self, SEL selector, unsigned long api,
                                 BOOL compute)
{
   (void)selector;
   (void)compute;
   return make_context(self, api);
}

static unsigned long
my_API(id self, SEL selector)
{
   struct jackpotlte_eagl *e = context_of(self);

   (void)selector;
   return e != NULL ? e->api : 0;
}

/*
 * Nothing here shares a context with anybody, and this object has none of
 * Apple's ivars, so nil is the only answer that is true and safe. CoreImage
 * makes a CIContext over an EAGLContext by asking this first.
 */
static id
my_sharegroup(id self, SEL selector)
{
   static int said;

   (void)self;
   (void)selector;
   if (!said) {
      said = 1;
      say("-sharegroup asked for: nil (nothing here shares a context)");
   }
   return nil;
}

/* The value is not read: what the caller passes it as has not been established
 * on this build, and a guess would be a read through it. */
static unsigned long
my_setParameter(id self, SEL selector, unsigned long parameter, void *value)
{
   static unsigned noted;

   (void)self;
   (void)selector;
   if (noted < 16) {
      say("-setParameter:%#lx to:%p", parameter, value);
      noted++;
   }
   return 0;
}

static void *
my_getMacroContextPrivate(id self, SEL selector)
{
   struct jackpotlte_eagl *e = context_of(self);

   (void)selector;
   return e != NULL ? e->table : NULL;
}

static unsigned long
my_getParameter(id self, SEL selector, unsigned long parameter, void *value)
{
   static unsigned noted;

   (void)self;
   (void)selector;
   (void)value;
   if (noted < 16) {
      say("-getParameter:%#lx to:%p: not answered", parameter, value);
      noted++;
   }
   return 0;
}

/* The name of the texture bound to GL_TEXTURE_2D, or 0. */
static int
bound_texture(struct jackpotlte_eagl *e)
{
   int name = 0;

   if (!resolve_helpers() || !jackpotlte_gl_make_current(e->gl))
      return 0;
   helpers.get_integerv(GL_TEXTURE_BINDING_2D, &name);
   return name;
}

/*
 * True when the surface's own memory can be the texture, with no copy: one
 * plane of 32-bit BGRA pixels.  That is the format the kernel's GPU driver can
 * map (0154's 0x7e) and the one CoreAnimation composites its own draws into, so
 * both -attachImage: and -texImageIOSurface: take the zero-copy path for it.
 */
static int
surface_shareable(IOSurfaceRef surface)
{
   return IOSurfaceGetPixelFormat(surface) == 'BGRA' &&
          IOSurfaceGetPlaneCount(surface) == 0 &&
          IOSurfaceGetBytesPerElement(surface) == 4;
}

/*
 * How many bytes one pixel of (format, type) is, for the formats CoreAnimation
 * passes -texImageIOSurface:.  0: not one of them.
 */
static size_t
bytes_a_pixel(unsigned long format, unsigned long type)
{
   if (type == GL_UNSIGNED_BYTE) {
      switch (format) {
      case GL_RGBA:
      case GL_BGRA:
         return 4;
      case GL_RGB:
         return 3;
      case GL_LUMINANCE_ALPHA:
         return 2;
      case GL_ALPHA:
      case GL_LUMINANCE:
         return 1;
      default:
         return 0;
      }
   }
   if (type == GL_FLOAT) {
      switch (format) {
      case GL_RGBA:
         return 16;
      case GL_LUMINANCE_ALPHA:
         return 8;
      case GL_ALPHA:
      case GL_LUMINANCE:
         return 4;
      default:
         return 0;
      }
   }
   if (type == GL_HALF_FLOAT) {
      switch (format) {
      case GL_RGBA:
         return 8;
      case GL_LUMINANCE_ALPHA:
         return 4;
      case GL_ALPHA:
      case GL_LUMINANCE:
         return 2;
      default:
         return 0;
      }
   }
   if (type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 ||
       type == GL_UNSIGNED_SHORT_5_5_5_1) {
      return 2;
   }
   return 0;
}

/*
 * A surface that cannot be a texture in place, made into one by copying it with
 * the format the caller asks for.  jt64's 12:05 log is why this exists:
 *
 *   -texImageIOSurface:: surface 6 is '..', 0 planes, 2 bytes a pixel: not a
 *       texture here
 *   -texImageIOSurface:: surface 5 is 'L008..', 0 planes, 2 bytes a pixel: not
 *       a texture here
 *
 * Those are not BGRA and so were refused, and a refused texture is not drawn:
 * the lock screen's clock (a 256x1 float gradient ramp) and its camera button
 * (an 8-bit luminance mask) came out as nothing and as a white square, while
 * the plain-BGRA status bar and home indicator drew.  A copy is the wrong thing
 * for a surface the caller draws *into* (-attachImage: keeps refusing), but for
 * one it only samples, the pixels are all that is wanted.
 */
/* glca9: a texture of `owner`'s is now a copy of `surface` (see copied[]). */
static void
copied_note(void *owner, int texture, IOSurfaceRef surface, uint32_t seed,
            int width, int height, unsigned long internal, unsigned long format,
            unsigned long type, BOOL invert, int target)
{
   unsigned i, slot = COPIED_MAX;

   if (texture == 0 || surface == NULL)
      return;
   for (i = 0; i < COPIED_MAX; i++) {
      if (copied[i].surface != NULL && copied[i].owner == owner &&
          copied[i].texture == texture) {
         slot = i;                     /* the same texture, copied again */
         break;
      }
   }
   for (i = 0; slot == COPIED_MAX && i < COPIED_MAX; i++) {
      if (copied[i].surface == NULL)
         slot = i;
   }
   if (slot == COPIED_MAX)
      slot = copied_next++ % COPIED_MAX;  /* full: an old one goes */
   if (copied[slot].surface != NULL) {
      if (copied[slot].surface != surface) {
         CFRelease(copied[slot].surface);
         copied[slot].surface = (IOSurfaceRef)CFRetain(surface);
      }
   } else {
      copied[slot].surface = (IOSurfaceRef)CFRetain(surface);
      copied_used++;
   }
   copied[slot].owner = owner;
   copied[slot].texture = texture;
   copied[slot].seed = seed;
   copied[slot].width = width;
   copied[slot].height = height;
   copied[slot].internal = internal;
   copied[slot].format = format;
   copied[slot].type = type;
   copied[slot].invert = invert;
   copied[slot].target = target;
   copied[slot].told = 0;
   copied[slot].empty = last_copy_alpha == 0;
   copied[slot].tries = 0;
   copied[slot].next_try = 0;
}

static int copy_again;                 /* surface_copy_texture from a bind: keep CA's filters */
static int alpha_max(const unsigned char *p, int w, int h, unsigned long format,
                     unsigned long type);

static BOOL
surface_copy_texture(struct jackpotlte_eagl *e, IOSurfaceRef surface,
                     int width, int height, unsigned long internal,
                     unsigned long format, unsigned long type, BOOL invert,
                     const char *method)
{
   static void (*tex_image)(unsigned, int, int, int, int, int, unsigned,
                            unsigned, const void *);
   static void (*tex_parameter)(unsigned, unsigned, int);
   static unsigned (*get_error)(void);
   static unsigned char *packed;
   static size_t capacity;
   static uint32_t logged;
   size_t bpr;
   size_t bpp;
   size_t row;
   size_t total;
   size_t alloc;
   const unsigned char *base;
   const unsigned char *source;
   unsigned error;
   uint32_t seed = 0;
   uint32_t pixels = IOSurfaceGetPixelFormat(surface);
   int locked;
   uint32_t copy_seed = 0;
   const int asked_width = width, asked_height = height;
   const unsigned long asked_internal = internal, asked_format = format, asked_type = type;

   if (!resolve_helpers() || !jackpotlte_gl_make_current(e->gl))
      return NO;
   if (tex_image == NULL) {
      tex_image = (void (*)(unsigned, int, int, int, int, int, unsigned,
                            unsigned, const void *))
         jackpotlte_gl_proc("glTexImage2D");
      tex_parameter = (void (*)(unsigned, unsigned, int))
         jackpotlte_gl_proc("glTexParameteri");
      get_error = (unsigned (*)(void))jackpotlte_gl_proc("glGetError");
      if (tex_image == NULL || get_error == NULL) {
         say("%s: the library has no glTexImage2D: surface %u is not a texture",
             method, (unsigned)IOSurfaceGetID(surface));
         return NO;
      }
   }
   if (width <= 0) {
      width = (int)IOSurfaceGetWidth(surface);
   }
   if (height <= 0) {
      height = (int)IOSurfaceGetHeight(surface);
   }
   bpr = IOSurfaceGetBytesPerRow(surface);
   bpp = bytes_a_pixel(format, type);
   if (bpp == 0) {
      /* Nothing usable was asked for, so the surface has to say.  In order of
       * how much each answer means: its stride (a row is a whole number of
       * pixels, or it is not this surface's stride at all), then its own bytes
       * a pixel, then the allocation.  jt64's surface 6 - 256x1, bytesPerElement
       * 2, bytesPerRow 672, 4096 allocated, pixelFormat 0 - is the case that
       * needs the second one: 672 is not 256 of anything, and the allocation's
       * 16 bytes a pixel is the page rounding, not the ramp. */
      size_t whole = IOSurfaceGetAllocSize(surface);
      size_t bpe = IOSurfaceGetBytesPerElement(surface);

      if (width > 0 && bpr % (size_t)width == 0) {
         bpp = bpr / (size_t)width;
      }
      if (bpp == 0 && bpe > 0 && bpe <= 16) {
         bpp = bpe;
      }
      if (bpp == 0 && width > 0 && height > 0 && whole != 0 &&
          whole % ((size_t)width * (size_t)height) == 0) {
         bpp = whole / ((size_t)width * (size_t)height);
      }
      if (bpp == 0) {
         say("%s: surface %u is '%.4s' %dx%d, row %zu, %zu B a pixel, %zu "
             "planes, %zu allocated, and asks for format %#lx type %#lx, which "
             "is no texture this knows", method,
             (unsigned)IOSurfaceGetID(surface),
             (const char *)&(uint32_t){ __builtin_bswap32(pixels) }, width,
             height, bpr, bpe, IOSurfaceGetPlaneCount(surface), whole, format,
             type);
         return NO;
      }
   }
   /* The caller did not say what the pixels are, or said something the surface
    * cannot be: a whole number of bytes a pixel is then the decision. */
   if (format == 0 || type == 0) {
      switch (bpp) {
      case 16:
         format = GL_RGBA;
         type = GL_FLOAT;
         break;
      case 8:
         format = GL_RGBA;
         type = GL_HALF_FLOAT;
         break;
      case 4:
         format = GL_RGBA;
         type = GL_UNSIGNED_BYTE;
         break;
      case 2:
         format = GL_LUMINANCE_ALPHA;
         type = GL_UNSIGNED_BYTE;
         break;
      default:
         format = GL_ALPHA;
         type = GL_UNSIGNED_BYTE;
         break;
      }
   }
   row = (size_t)width * bpp;
   if (bpr < row && width > 0 && bpr % (size_t)width == 0) {
      /* The caller's idea of a pixel is wider than a row of the surface can
       * hold: believe the surface. */
      bpp = bpr / (size_t)width;
      row = (size_t)width * bpp;
      if (bpp == 1) {
         format = GL_ALPHA;
         type = GL_UNSIGNED_BYTE;
      }
   }
   if (internal == 0) {
      /* GL_RGBA8 for a byte BGRA surface, else the format itself. */
      internal = (format == GL_BGRA) ? GL_RGBA8 : format;
      if (bpp == 16) {
         internal = GL_RGBA32F;
      } else if (bpp == 8 && format == GL_RGBA) {
         internal = GL_RGBA16F;
      }
   }
   total = row * (size_t)height;

   locked = IOSurfaceLock(surface, SURFACE_LOCK_READ_ONLY, &seed) == 0;
   copy_seed = IOSurfaceGetSeed(surface);   /* what this copy shows (glca9) */
   base = (const unsigned char *)IOSurfaceGetBaseAddress(surface);
   if (base == NULL || total == 0) {
      if (locked) {
         IOSurfaceUnlock(surface, SURFACE_LOCK_READ_ONLY, &seed);
      }
      say("%s: surface %u has no memory to copy", method,
          (unsigned)IOSurfaceGetID(surface));
      return NO;
   }
   if (bpr == 0) {
      bpr = row;
   }
   /* Always pack: the surface's rows and the texture's rows are not guaranteed
    * to be the same width, and a copy that reads past a row is a fault, while
    * one that copies less than a row is only a wrong picture. */
   if (total > capacity) {
      unsigned char *bigger = realloc(packed, total);

      if (bigger == NULL) {
         if (locked) {
            IOSurfaceUnlock(surface, SURFACE_LOCK_READ_ONLY, &seed);
         }
         say("%s: no room to pack surface %u (%zu bytes)", method,
             (unsigned)IOSurfaceGetID(surface), total);
         return NO;
      }
      packed = bigger;
      capacity = total;
   }
   /* Nothing is read past the surface's allocation.  bytesPerRow times the
    * height need not fit in it - jt64's surface 6 is 256x1 with bytesPerRow 672
    * and a 4096-byte allocation, which is more than 672 - so the last row of a
    * short allocation is copied short rather than read past. */
   alloc = IOSurfaceGetAllocSize(surface);
   for (int y = 0; y < height; y++) {
      size_t n = bpr < row ? bpr : row;
      size_t off = (size_t)(invert ? height - 1 - y : y) * bpr;
      unsigned char *out = packed + (size_t)y * row;

      memset(out, 0, row);
      if (alloc != 0 && off >= alloc) {
         continue;
      }
      if (alloc != 0 && n > alloc - off) {
         n = alloc - off;
      }
      memcpy(out, base + off, n);
   }
   source = packed;
   /*
    * glca9f: CoreAnimation's IOSurfaceWrapClientImage surfaces carry no pixel
    * format here (the kernel says 0, so they come through this copy), and
    * CoreAnimation asks for GL_BGRA whatever they hold.  The apps' SF Symbols
    * are CGImages with kCGBitmapByteOrder32Little | AlphaPremultipliedLast,
    * bytes A,B,G,R: read as B,G,R,A the alpha is the red byte, 0 for a black
    * template glyph, and the glyph is drawn as nothing (Settings' back
    * chevron: 670 bytes not 0 in its 33x57 surface, every one at an offset
    * that is a multiple of 4).  Premultiplied pixels never have a colour byte
    * above their alpha, so when the bytes break that rule as B,G,R,A and keep
    * it as A,B,G,R, they are put in B,G,R,A order.
    */
   if (pixels == 0 && bpp == 4 && format == GL_BGRA && type == GL_UNSIGNED_BYTE) {
      size_t n = (size_t)width * (size_t)height, as_bgra = 0, as_abgr = 0, lit = 0;
      static unsigned said;

      for (size_t i = 0; i < n; i++) {
         const unsigned char *q = packed + 4 * i;
         unsigned char m012 = q[0] > q[1] ? q[0] : q[1], m123 = q[1] > q[2] ? q[1] : q[2];

         if (q[2] > m012)
            m012 = q[2];
         if (q[3] > m123)
            m123 = q[3];
         as_bgra += m012 > q[3];
         as_abgr += m123 > q[0];
         lit += (q[0] | q[1] | q[2] | q[3]) != 0;
      }
      if (lit != 0 && as_bgra != 0 && as_abgr == 0) {
         for (size_t i = 0; i < n; i++) {
            unsigned char *q = packed + 4 * i, a = q[0];

            q[0] = q[1];
            q[1] = q[2];
            q[2] = q[3];
            q[3] = a;
         }
         reordered++;
         if (said++ < 12 || reordered % 500 == 0)
            say("%s: surface %u (%dx%d) holds A,B,G,R bytes (%zu of %zu pixels break B,G,R,A): "
                "put in B,G,R,A order, %lu so far", method, (unsigned)IOSurfaceGetID(surface),
                width, height, as_bgra, n, reordered);
      }
   }

   while (get_error() != 0) {
   }
   tex_image(GL_TEXTURE_2D, 0, (int)internal, width, height, 0, (unsigned)format,
             (unsigned)type, source);
   error = get_error();
   if (locked) {
      IOSurfaceUnlock(surface, SURFACE_LOCK_READ_ONLY, &seed);
   }
   if (error != 0) {
      say("%s: surface %u (%dx%d, row %zu, %zu B a pixel, '%.4s') copy left GL "
          "error %#x", method, (unsigned)IOSurfaceGetID(surface), width, height,
          bpr, bpp,
          (const char *)&(uint32_t){ __builtin_bswap32(pixels) }, error);
      return NO;
   }
   /* Complete the texture: without a mipmap level, the default minification
    * filter samples nothing, whatever was uploaded.  A copy made again from a
    * bind keeps the filters CoreAnimation set since. */
   if (tex_parameter != NULL && !copy_again) {
      tex_parameter(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      tex_parameter(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   }
   last_copy_alpha = (size_t)width * (size_t)height <= 512u * 512u ?
      alpha_max(source, width, height, format, type) : -1;
   if (!copy_again)
      copied_note(e, bound_texture(e), surface, copy_seed, asked_width, asked_height,
                  asked_internal, asked_format, asked_type, invert,
                  strcmp(method, "-attachImage:") == 0);
   if (copy_again)
      return YES;
   {
      int alpha = last_copy_alpha;
      static unsigned empty_said;

      if (alpha == 0 && empty_said++ < 40) {
         /* glca9e: is anything at all in the surface's memory, and where */
         const unsigned char *all = (const unsigned char *)IOSurfaceGetBaseAddress(surface);
         size_t whole = IOSurfaceGetAllocSize(surface), nonzero = 0, first = 0, last = 0;

         if (whole > (1u << 20))
            whole = 1u << 20;
         for (size_t i = 0; all != NULL && i < whole; i++) {
            if (all[i] != 0) {
               if (nonzero++ == 0)
                  first = i;
               last = i;
            }
         }
         say("%s: surface %u (%dx%d, '%.4s', %zu B a pixel, row %zu, %zu allocated at %p, "
             "seed %u; asked %#lx/%#lx/%#lx) copied with alpha 0 everywhere; its memory has "
             "%zu bytes not 0 (from %zu to %zu)", method, (unsigned)IOSurfaceGetID(surface),
             width, height, (const char *)&(uint32_t){ __builtin_bswap32(pixels) }, bpp,
             IOSurfaceGetBytesPerRow(surface), IOSurfaceGetAllocSize(surface), (const void *)all,
             copy_seed, asked_internal, asked_format, asked_type, nonzero, first, last);
      }
   }
   if (logged++ < 16) {
      say("%s: surface %u is '%.4s' %dx%d, row %zu of %zu bytes, %zu B a pixel, "
          "%zu planes, %zu allocated, format %#lx type %#lx: copied into the "
          "texture", method, (unsigned)IOSurfaceGetID(surface),
          (const char *)&(uint32_t){ __builtin_bswap32(pixels) }, width, height,
          bpr, IOSurfaceGetBytesPerRow(surface), bpp,
          IOSurfaceGetPlaneCount(surface), IOSurfaceGetAllocSize(surface),
          format, type);
   }
   return YES;
}

/*
 * glca9b: the largest alpha in tightly packed 8-bit pixels (formats with an
 * alpha byte), or -1 when the format has none this can read.  For finding
 * textures that arrive empty: an app's SF Symbol drew as nothing in a trace.
 */
static int
alpha_max(const unsigned char *p, int w, int h, unsigned long format, unsigned long type)
{
   size_t step, at, n;
   int best = 0;

   if (p == NULL || w <= 0 || h <= 0 || type != GL_UNSIGNED_BYTE)
      return -1;
   if (format == GL_RGBA || format == GL_BGRA) {
      step = 4; at = 3;
   } else if (format == GL_LUMINANCE_ALPHA) {
      step = 2; at = 1;
   } else if (format == GL_ALPHA) {
      step = 1; at = 0;
   } else {
      return -1;
   }
   n = (size_t)w * (size_t)h;
   for (size_t i = 0; i < n && best < 255; i++) {
      if (p[i * step + at] > best)
         best = p[i * step + at];
   }
   return best;
}

static unsigned long uploads, empty_uploads;

/* CoreAnimation's own uploads (the shim's copies call the library directly). */
static void
upload_note(const char *what, int w, int h, unsigned long internal, unsigned long format,
            unsigned long type, const void *pixels)
{
   int alpha = -2;

   uploads++;
   if (w > 0 && h > 0 && (size_t)w * (size_t)h <= 512u * 512u && (uintptr_t)pixels >= 0x100000u) {
      int unpack_buffer = 0;

      /* with a pixel unpack buffer bound, `pixels` is an offset into it */
      if (resolve_helpers())
         helpers.get_integerv(0x88ef /* GL_PIXEL_UNPACK_BUFFER_BINDING */, &unpack_buffer);
      alpha = unpack_buffer != 0 ? -3 : alpha_max(pixels, w, h, format, type);
   }
   if (alpha == 0)
      empty_uploads++;
   if (uploads <= 12 || (alpha == 0 && empty_uploads <= 40) || uploads % 1000 == 0)
      say("upload %lu: %s %dx%d internal %#lx format %#lx type %#lx, %s, alpha max %d "
          "(%lu with alpha 0 everywhere)", uploads, what, w, h, internal, format, type,
          pixels != NULL ? "pixels" : "no pixels", alpha, empty_uploads);
}

static void
my_glTexImage2D(void *context, GLenum target, GLint level, GLint internal, GLsizei w,
                GLsizei h, GLint border, GLenum format, GLenum type, const void *pixels)
{
   static void (*real)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,
                       const void *);

   jackpotlte_gl_use(context);
   if (real == NULL)
      real = (void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,
                       const void *))jackpotlte_gl_proc("glTexImage2D");
   if (level == 0)
      upload_note("glTexImage2D", w, h, (unsigned long)internal, format, type, pixels);
   if (real != NULL)
      real(target, level, internal, w, h, border, format, type, pixels);
}

static void
my_glTexSubImage2D(void *context, GLenum target, GLint level, GLint x, GLint y, GLsizei w,
                   GLsizei h, GLenum format, GLenum type, const void *pixels)
{
   static void (*real)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                       const void *);

   jackpotlte_gl_use(context);
   if (real == NULL)
      real = (void (*)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                       const void *))jackpotlte_gl_proc("glTexSubImage2D");
   if (level == 0)
      upload_note("glTexSubImage2D", w, h, 0, format, type, pixels);
   if (real != NULL)
      real(target, level, x, y, w, h, format, type, pixels);
}

/*
 * glca9: CoreAnimation binds a copied texture again each time it draws it,
 * and never hands its surface over again.  When the surface's seed has moved
 * since the copy (its writer drew into it after the first bind), the copy is
 * made again here, into the texture just bound.
 */
static void
my_glBindTexture(void *context, GLenum target, GLuint texture)
{
   static void (*real)(GLenum, GLuint);
   unsigned i;

   jackpotlte_gl_use(context);
   if (real == NULL)
      real = (void (*)(GLenum, GLuint))jackpotlte_gl_proc("glBindTexture");
   if (real == NULL)
      return;
   real(target, texture);
   if (copied_used == 0 || target != GL_TEXTURE_2D || texture == 0 || context == NULL)
      return;
   for (i = 0; i < COPIED_MAX; i++) {
      uint32_t seed, was;

      if (copied[i].surface == NULL || copied[i].owner != context ||
          copied[i].texture != (int)texture)
         continue;
      seed = IOSurfaceGetSeed(copied[i].surface);
      if (seed == copied[i].seed) {
         /*
          * glca9d: a copy that came out empty (an app's SF Symbol: seed 1, no
          * draw into it) is made again twice a second for 20 s, in case its
          * writer fills it without moving the seed.
          */
         uint64_t now = mach_absolute_time();

         if (!copied[i].empty || copied[i].tries >= 40 || now < copied[i].next_try)
            break;
         copied[i].tries++;
         copied[i].next_try = now + 12000000ull;   /* 0.5 s of 24 MHz ticks */
         copy_again = 1;
         if (surface_copy_texture((struct jackpotlte_eagl *)context, copied[i].surface,
                                  copied[i].width, copied[i].height, copied[i].internal,
                                  copied[i].format, copied[i].type, copied[i].invert,
                                  "retry") && last_copy_alpha > 0) {
            copied[i].empty = 0;
            recopies++;
            say("copy: surface %u (%zux%zu) had pixels at try %u, seed still %u (alpha max %d)",
                (unsigned)IOSurfaceGetID(copied[i].surface),
                IOSurfaceGetWidth(copied[i].surface), IOSurfaceGetHeight(copied[i].surface),
                copied[i].tries, seed, last_copy_alpha);
         } else if (copied[i].tries == 40) {
            static unsigned gave_up;

            if (gave_up++ < 20)
               say("copy: surface %u (%zux%zu) still empty after 40 tries (seed %u)",
                   (unsigned)IOSurfaceGetID(copied[i].surface),
                   IOSurfaceGetWidth(copied[i].surface), IOSurfaceGetHeight(copied[i].surface),
                   seed);
         }
         copy_again = 0;
         break;
      }
      was = copied[i].seed;
      copied[i].seed = seed;           /* a failed copy is not tried again until it moves again */
      copy_again = 1;
      if (surface_copy_texture((struct jackpotlte_eagl *)context, copied[i].surface,
                               copied[i].width, copied[i].height, copied[i].internal,
                               copied[i].format, copied[i].type, copied[i].invert,
                               "bind")) {
         recopies++;
         if (recopies <= 24 || recopies % 500 == 0)
            say("copy: texture %u made again from surface %u (seed %u -> %u), %lu made again",
                (unsigned)texture, (unsigned)IOSurfaceGetID(copied[i].surface), was, seed,
                recopies);
      }
      copy_again = 0;
      break;
   }
}

/*
 * The texture bound to the target is to be the surface's memory.  Returns NO
 * when the surface is not one this can be done for, and says why.
 */
static BOOL
surface_is_texture(struct jackpotlte_eagl *e, IOSurfaceRef surface,
                   unsigned long target, const char *method)
{
   uint32_t pixels = IOSurfaceGetPixelFormat(surface);
   int texture = bound_texture(e);
   static unsigned shown;

   if (target != GL_TEXTURE_2D) {
      say("%s: target %#lx is not GL_TEXTURE_2D", method, target);
      return NO;
   }
   if (texture == 0) {
      say("%s: no texture is bound", method);
      return NO;
   }
   if (!surface_shareable(surface)) {
      say("%s: surface %u is '%.4s', %zu planes, %zu bytes a pixel: not a "
          "texture here", method, (unsigned)IOSurfaceGetID(surface),
          (const char *)&(uint32_t){ __builtin_bswap32(pixels) },
          IOSurfaceGetPlaneCount(surface), IOSurfaceGetBytesPerElement(surface));
      return NO;
   }
   if (!jackpotlte_gl_surface_texture(e->gl, (uint32_t)IOSurfaceGetID(surface)))
      return NO;
   shared_note(e, texture, surface, strcmp(method, "-attachImage:") == 0);
   if (shown++ < 16) {
      say("%s: texture %d is surface %u, %zu x %zu, memory shared", method,
          texture, (unsigned)IOSurfaceGetID(surface), IOSurfaceGetWidth(surface),
          IOSurfaceGetHeight(surface));
   }
   return YES;
}

/*
 * A surface handed to a *sampling* call: in place when it can be (one plane of
 * BGRA), and as a copy in the format the caller asked for when it cannot.
 */
static BOOL
my_texImageIOSurface(id self, SEL selector, IOSurfaceRef surface,
                     unsigned long target, unsigned long internal, int width,
                     int height, unsigned long format, unsigned long type,
                     unsigned int plane, BOOL invert)
{
   struct jackpotlte_eagl *e = context_of(self);

   (void)selector;
   (void)plane;
   (void)invert;
   if (e == NULL || surface == NULL)
      return NO;
   if (target != GL_TEXTURE_2D) {
      say("-texImageIOSurface: target %#lx is not GL_TEXTURE_2D", target);
      return NO;
   }
   if (bound_texture(e) == 0) {
      say("-texImageIOSurface: no texture is bound");
      return NO;
   }
   if (surface_shareable(surface))
      return surface_is_texture(e, surface, target, "-texImageIOSurface:");
   return surface_copy_texture(e, surface, width, height, internal, format, type,
                               invert, "-texImageIOSurface:");
}

static BOOL
my_attachImage(id self, SEL selector, unsigned long target, IOSurfaceRef surface,
               BOOL inverted)
{
   struct jackpotlte_eagl *e = context_of(self);

   (void)selector;
   if (e == NULL || surface == NULL)
      return NO;
   if (surface_shareable(surface))
      return surface_is_texture(e, surface, target, "-attachImage:");
   /*
    * Pixel-format 0, 4 bytes a pixel (Control Center corner masks and similar):
    * not BGRA so surface_is_texture refuses them, which left square unclipped
    * tiles (shot-0191-cc.png).  Copy as BGRA; do not note them as shared
    * (surface_copy_texture does not), so clean_surface never touches them.
    */
   if (IOSurfaceGetPlaneCount(surface) == 0 &&
       IOSurfaceGetBytesPerElement(surface) == 4) {
      return surface_copy_texture(e, surface,
            (int)IOSurfaceGetWidth(surface), (int)IOSurfaceGetHeight(surface),
            GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE, inverted, "-attachImage:");
   }
   return surface_is_texture(e, surface, target, "-attachImage:");
}

/* The same surface method without `invert`, which the build also has.  The
 * caller means the same thing; the flag only says how the surface's rows are
 * handed to the sampler, and Apple's own header spells both. */
static BOOL
my_texImageIOSurface_plane(id self, SEL selector, IOSurfaceRef surface,
                           unsigned long target, unsigned long internal,
                           int width, int height, unsigned long format,
                           unsigned long type, unsigned int plane)
{
   struct jackpotlte_eagl *e = context_of(self);

   (void)selector;
   (void)plane;
   if (e == NULL || surface == NULL)
      return NO;
   if (target != GL_TEXTURE_2D) {
      say("-texImageIOSurface:...plane: target %#lx is not GL_TEXTURE_2D",
          target);
      return NO;
   }
   if (bound_texture(e) == 0) {
      say("-texImageIOSurface:...plane: no texture is bound");
      return NO;
   }
   if (surface_shareable(surface))
      return surface_is_texture(e, surface, target,
                                "-texImageIOSurface:...plane:");
   return surface_copy_texture(e, surface, width, height, internal, format, type,
                               NO, "-texImageIOSurface:...plane:");
}

/*
 * -presentRenderbuffer: and the two timed forms.  What a present has to do
 * here is the flush: the library's driver submits a batch to the GPU on one,
 * and CoreAnimation draws into an IOSurface that the kernel's display path
 * composites by itself, so there is nothing to swap and nowhere to copy - the
 * frame exists once the GPU has written it.  A draw with no flush after it
 * leaves the batch in the library's command stream, which is what the jt60
 * log ends in (glDrawElements, then nothing, and no JackpotlteMali job).
 *
 * The first few presents are logged with the GL error, if any; after that one
 * in six hundred, which is enough to say a frame rate is going by.
 */
static BOOL
present_target(id self, SEL selector, unsigned long target, const char *method)
{
   struct jackpotlte_eagl *e = context_of(self);
   static unsigned presents;

   (void)selector;
   if (e == NULL) {
      if (presents++ < 8)
         say("%s: no context of ours", method);
      return NO;
   }
   jackpotlte_gl_make_current(e->gl);
   {
      void (*flush)(void) = (void (*)(void))jackpotlte_gl_proc("glFlush");
      unsigned (*get_error)(void) =
          (unsigned (*)(void))jackpotlte_gl_proc("glGetError");

      if (flush != NULL)
         flush();
      if (presents < 8) {
         unsigned error = get_error != NULL ? get_error() : 0;

         if (error != 0)
            say("%s: target %#lx, flush left GL error %#x", method, target, error);
         else
            say("%s: target %#lx, flushed", method, target);
      } else if (presents % 600 == 0) {
         say("%s: %u presents", method, presents + 1);
      }
   }
   presents++;
   return YES;
}

static BOOL
my_presentRenderbuffer(id self, SEL selector, unsigned long target)
{
   return present_target(self, selector, target, "-presentRenderbuffer:");
}

static BOOL
my_presentRenderbuffer_atTime(id self, SEL selector, unsigned long target,
                              double when)
{
   (void)when;
   return present_target(self, selector, target, "-presentRenderbuffer:atTime:");
}

static BOOL
my_presentRenderbuffer_afterMinimumDuration(id self, SEL selector,
                                            unsigned long target,
                                            double duration)
{
   (void)duration;
   return present_target(self, selector, target,
                         "-presentRenderbuffer:afterMinimumDuration:");
}

/*
 * -renderbufferStorage:fromDrawable: is the path where CoreAnimation draws
 * to the screen through the context instead of into a surface.  There is no
 * renderbuffer of Apple's here and saying NO could stop a renderer that would
 * otherwise draw, so this says YES and records the drawable: if the log shows
 * it, the surface path was not the one taken and the pixels of this one have
 * nowhere to go yet.
 */
static BOOL
my_renderbufferStorage(id self, SEL selector, unsigned long target, id drawable)
{
   static unsigned noted;

   (void)self;
   (void)selector;
   if (noted < 8) {
      say("-renderbufferStorage:%#lx fromDrawable:%s", target,
          drawable != nil ? class_getName(object_getClass(drawable)) : "nil");
      noted++;
   }
   return YES;
}

/*
 * +setCurrentContext: and +currentContext.  Apple's pair keeps the context in
 * the thread's own storage; here every GL entry already carries the context it
 * means as the table's first argument, so nothing reads this back - the two
 * only have to be consistent with each other and stay out of Apple's engine.
 */
static _Thread_local id current_context;

static void
my_setCurrentContext(id self, SEL selector, id context)
{
   (void)self;
   (void)selector;
   current_context = context;
}

static id
my_currentContext(id self, SEL selector)
{
   (void)self;
   (void)selector;
   return current_context;
}

/*
 * objc_msgSendSuper, read as what is wanted here.  <objc/message.h> declares it
 * as `void objc_msgSendSuper(void)` - Apple's form, which says the arguments
 * are the caller's to know - so this is the one place the two are written out:
 * the declared function pointer, and the call.
 */
static void
send_to_super(struct objc_super *super, SEL selector)
{
   union {
      void (*as_declared)(void);
      void (*as_called)(struct objc_super *, SEL);
   } send;

   send.as_declared = objc_msgSendSuper;
   send.as_called(super, selector);
}

static void
my_dealloc(id self, SEL selector)
{
   struct objc_super super = { self, class_getSuperclass((Class)objc_getClass("EAGLContext")) };
   struct jackpotlte_eagl *e = context_of(self);
   unsigned i;

   if (e != NULL) {
      for (i = 0; i < CONTEXTS; i++) {
         if (contexts[i] == e)
            contexts[i] = NULL;
      }
      jackpotlte_gl_destroy(e->gl);
      free(e);
   }
   send_to_super(&super, selector);
}

/*
 * The methods and the implementations that replace them.  A union, not a cast:
 * the runtime is handed an IMP and the implementations have their own
 * signatures, and reading one function pointer as another is what this says.
 */
union method_prototype {
   IMP imp;
   id (*initialise)(id, SEL, unsigned long, id);          /* with properties or a sharegroup */
   id (*initialise_simple)(id, SEL, unsigned long);
   id (*initialise_compute)(id, SEL, unsigned long, BOOL);
   unsigned long (*api)(id, SEL);
   id (*sharegroup)(id, SEL);
   unsigned long (*parameter)(id, SEL, unsigned long, void *);
   void *(*macro_context)(id, SEL);
   BOOL (*image)(id, SEL, IOSurfaceRef, unsigned long, unsigned long, int, int,
                 unsigned long, unsigned long, unsigned int, BOOL);
   BOOL (*image_plane)(id, SEL, IOSurfaceRef, unsigned long, unsigned long, int,
                       int, unsigned long, unsigned long, unsigned int);
   BOOL (*attach)(id, SEL, unsigned long, IOSurfaceRef, BOOL);
   BOOL (*present)(id, SEL, unsigned long);
   BOOL (*present_time)(id, SEL, unsigned long, double);
   BOOL (*storage)(id, SEL, unsigned long, id);
   void (*set_context)(id, SEL, id);
   id (*get_context)(id, SEL);
   void (*dispose)(id, SEL);
};

__attribute__((constructor)) static void
install(void)
{
   static const struct {
      const char *name;
      union method_prototype ours;
   } methods[] = {
      { "initWithAPI:properties:", { .initialise = my_initWithAPI_properties } },
      { "initWithAPI:", { .initialise_simple = my_initWithAPI } },
      { "initWithAPI:sharegroup:", { .initialise = my_initWithAPI_sharegroup } },
      { "initWithAPI:sharedWithCompute:",
        { .initialise_compute = my_initWithAPI_sharedWithCompute } },
      { "API", { .api = my_API } },
      { "sharegroup", { .sharegroup = my_sharegroup } },
      { "setParameter:to:", { .parameter = my_setParameter } },
      { "getParameter:to:", { .parameter = my_getParameter } },
      { "getMacroContextPrivate", { .macro_context = my_getMacroContextPrivate } },
      { "texImageIOSurface:target:internalFormat:width:height:format:type:plane:invert:",
        { .image = my_texImageIOSurface } },
      { "texImageIOSurface:target:internalFormat:width:height:format:type:plane:",
        { .image_plane = my_texImageIOSurface_plane } },
      { "attachImage:toCoreSurface:invertedRender:", { .attach = my_attachImage } },
      { "renderbufferStorage:fromDrawable:",
        { .storage = my_renderbufferStorage } },
      { "presentRenderbuffer:", { .present = my_presentRenderbuffer } },
      { "presentRenderbuffer:atTime:",
        { .present_time = my_presentRenderbuffer_atTime } },
      { "presentRenderbuffer:afterMinimumDuration:",
        { .present_time = my_presentRenderbuffer_afterMinimumDuration } },
      { "dealloc", { .dispose = my_dealloc } },
   };
   static const struct {
      const char *name;
      union method_prototype ours;
   } class_methods[] = {
      { "setCurrentContext:", { .set_context = my_setCurrentContext } },
      { "currentContext", { .get_context = my_currentContext } },
   };
   Class eagl = (Class)objc_getClass("EAGLContext");
   unsigned i, installed = 0;

   say("library loaded into the display server");
   /* Bring-up aid: where this file and the OpenGL ES library beside it were
    * loaded.  The kernel's crash report prints the faulting pc but no image
    * base, so these numbers are what turn that pc into an offset into a file
    * built here (a dylib's __TEXT is at vmaddr 0, so the header address is
    * the base).  dlsym is not used to find them: a function pointer cast to
    * void * is a pedantic-C warning, and -Werror is on. */
   for (uint32_t i = 0, n = _dyld_image_count(); i < n; i++) {
      const char *where = _dyld_get_image_name(i);
      const char *base;

      if (where == NULL ||
          (strstr(where, "OSMesa") == NULL && strstr(where, "jackpotlte") == NULL)) {
         continue;
      }
      base = strrchr(where, '/');
      say("image %p %s", (void *)_dyld_get_image_header(i),
          base != NULL ? base + 1 : where);
   }
   if (eagl == NULL) {
      say("no EAGLContext class: this library is loaded where OpenGL ES is not");
      return;
   }
   for (i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
      SEL selector = sel_registerName(methods[i].name);
      Method method = class_getInstanceMethod(eagl, selector);

      /* Only methods that are there: the implementations are installed, and the
       * encodings stay the class's own.  A selector iOS does not have is noted,
       * not added with a guess at its signature. */
      if (method != NULL) {
         method_setImplementation(method, methods[i].ours.imp);
         installed++;
      } else {
         say("EAGLContext has no -%s", methods[i].name);
      }
   }
   for (i = 0; i < sizeof(class_methods) / sizeof(class_methods[0]); i++) {
      SEL selector = sel_registerName(class_methods[i].name);
      Method method = class_getClassMethod(eagl, selector);

      if (method != NULL) {
         method_setImplementation(method, class_methods[i].ours.imp);
         installed++;
      } else {
         say("EAGLContext has no +%s", class_methods[i].name);
      }
   }
   say("EAGLContext is ours: %u of %u methods", installed,
       (unsigned)((sizeof(methods) + sizeof(class_methods)) /
                  sizeof(methods[0])));
}
