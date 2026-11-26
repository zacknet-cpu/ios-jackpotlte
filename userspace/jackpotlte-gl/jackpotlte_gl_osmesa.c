/* SPDX-License-Identifier: MIT */
/*
 * The OpenGL ES library behind userspace/jackpotlte-gl/eagl.c: Mesa's OSMesa,
 * as tools/mesa-jackpotlte/ builds it (panfrost on Bifrost v6, the phone's
 * Mali-G71), with an OpenGL ES profile (tools/mesa-jackpotlte/0004).
 *
 * This file is all that knows which library draws.  The EAGL face wants four
 * things besides the gl* calls themselves: a context of a version of OpenGL ES,
 * that context made the calling thread's, a context taken away again, and a
 * name for the log.  OSMesa is reached through dlopen because the library is
 * put on the phone beside this one, not linked against it: the build of this
 * file is the same whether the library is there or not, and a phone without it
 * logs that and draws nothing rather than failing to load.
 *
 * The gl* functions are never looked up by name here.  jackpotlte_gl_proc is
 * OSMesa's own OSMesaGetProcAddress: the guest of the table (gl_slots.c) asks
 * for a name and gets the library's function, which is the only way to reach
 * it - a plain dlsym(RTLD_DEFAULT, "glBindTexture") would find the dyld shared
 * cache's OpenGLES.framework first, and Apple's driver knows nothing of these
 * contexts.
 */
#include "jackpotlte_gl.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mach-o/dyld.h>
#include <zlib.h>
#include <CommonCrypto/CommonDigest.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* GL/osmesa.h's attribute and format values (Mesa 25.0.7, include/GL/osmesa.h;
 * OSMESA_ES_PROFILE is 0004's addition). */
#define OSMESA_FORMAT                 0x22
#define OSMESA_RGBA                   0x1908
#define OSMESA_PROFILE                0x33
#define OSMESA_ES_PROFILE             0x38
#define OSMESA_CONTEXT_MAJOR_VERSION  0x36
#define OSMESA_CONTEXT_MINOR_VERSION  0x37

/* The GL names this file itself uses. */
#define GL_TEXTURE_2D                 0x0de1
#define GL_UNSIGNED_BYTE              0x1401
#define GL_NO_ERROR                   0

/* OSMesa renders into memory it is handed; these are the pixels it may draw
 * into when the caller asks for its own window (nothing here does: what is
 * drawn is drawn into SweetSurfaces, 0003). */
#define WINDOW 16

struct jackpotlte_gl {
   void *context;   /* OSMesaContext */
   void *window;    /* the memory OSMesaMakeCurrent is given */
};

/* The library, opened once. */
static struct {
   void *handle;
   const char *where;
   void *(*create_context)(const int *attribs, void *share);
   void (*destroy_context)(void *context);
   unsigned char (*make_current)(void *context, void *buffer, unsigned type,
                                 int width, int height);
   jackpotlte_gl_func (*get_proc)(const char *name);
} lib;

/* The context this thread's GL calls are for.  OSMesa keeps one per thread. */
static _Thread_local struct jackpotlte_gl *current;

static void
say(const char *format, ...)
{
   char line[512];
   va_list arguments;

   va_start(arguments, format);
   (void)vsnprintf(line, sizeof(line), format, arguments);
   va_end(arguments);
   fprintf(stderr, "jackpotlte-gl: %s\n", line);
   /* eagl.c: on the kernel console too when the boot-arg jtgllog=1 is set. */
   jackpotlte_gl_log(line);
}

/*
 * glca16: Mesa 25.0.7 as built for the phone (mesa-out b8da8f55, LC_UUID
 * 938e27a4-44bb-3ae4-b065-c20ecaefb07b) writes the Bifrost framebuffer
 * preload's padding attribute buffer 0x100 bytes into its 0x20-byte pool
 * allocation (pan_preload_fb_part + 0x348: str q0, [x0, #0x100]; the
 * allocation is 2 x 16-byte ATTRIBUTE_BUFFER, the second belongs at +0x10).
 * Usually that only overwrites the next descriptor in the pool; at the end of
 * a pool buffer it lands in the next mapping, and backboardd died with SIGBUS
 * (12 of its 16 crashes, 11-22..24, all at Mesa + 0x41a308).  The fix is that
 * one instruction (imm12 0x10 -> 0x1): the library is copied from
 * /usr/local/lib to /private/var/mobile/Library under its own name, checked
 * (size, UUID, the old bytes) and patched there once, and that copy is loaded.
 * Switch: a8gl-mesafix "0" loads the original.
 */
#define MESA_SRC   "/usr/local/lib/libOSMesa.8.dylib"
#define MESA_FIXED "/private/var/mobile/Library/libOSMesa.8.a8fix2.dylib"
static const unsigned char mesa_uuid[16] = {
   0x93, 0x8e, 0x27, 0xa4, 0x44, 0xbb, 0x3a, 0xe4, 0xb0, 0x65, 0xc2, 0x0e, 0xca, 0xef, 0xb0, 0x7b,
};
/*
 * glca17b: one more, in this port's own DRM stand-in (userspace/libjackpotlte,
 * built into Mesa): jt_bo_madvise asked the kernel, which answers it without
 * doing anything (retained = 1, JackpotlteMali.cpp); ~5% of the render thread
 * while scrolling.  It now stores retained = 1 and returns 0 in place.
 * jt_wait_bo stays a kernel call: WAIT_BO really waits for the buffer's last
 * job (the top of externalMethod; the switch's case is dead code), and glca17,
 * which answered it in place too, let Mesa reuse buffers the GPU was still
 * reading: job faults, 10 s waits (11-24 17:33 UTC).  Its copy
 * (libOSMesa.8.a8fixed.dylib) is not used by anything.
 */
static const struct {
   size_t offset, length;
   unsigned char was[20], fixed[20];
} mesa_fixes[] = {
   /* pan_preload_fb_part + 0x348: str q0, [x0, #0x100] -> [x0, #0x10] */
   { 0x41a308, 4, { 0x00, 0x40, 0x80, 0x3d }, { 0x00, 0x04, 0x80, 0x3d } },
   /* jt_bo_madvise: cbz x3, 1f; mov w8, #1; str w8, [x3]; 1: mov w0, #0; ret */
   { 0x49bea0, 20,
     { 0xe8, 0x03, 0x00, 0xaa, 0xa0, 0x02, 0x80, 0x12, 0xa8, 0x02, 0x00, 0xb4,
       0x81, 0x02, 0x00, 0x34, 0xff, 0xc3, 0x00, 0xd1 },
     { 0x63, 0x00, 0x00, 0xb4, 0x28, 0x00, 0x80, 0x52, 0x68, 0x00, 0x00, 0xb9,
       0x00, 0x00, 0x80, 0x52, 0xc0, 0x03, 0x5f, 0xd6 } },
};

static int
mesa_uuid_matches(const unsigned char *b, size_t size)
{
   uint32_t ncmds, at = 32;

   if (size < 32 || *(const uint32_t *)b != 0xfeedfacfu)
      return 0;
   ncmds = *(const uint32_t *)(b + 16);
   for (uint32_t i = 0; i < ncmds && at + 8 <= size; i++) {
      uint32_t cmd = *(const uint32_t *)(b + at), len = *(const uint32_t *)(b + at + 4);

      if (cmd == 0x1b && at + 24 <= size)          /* LC_UUID */
         return memcmp(b + at + 8, mesa_uuid, 16) == 0;
      if (len < 8)
         return 0;
      at += len;
   }
   return 0;
}

static const char *
mesa_fixed_copy(void)
{
   struct stat src, dst;
   unsigned char *b = NULL;
   size_t n = sizeof(mesa_fixes) / sizeof(mesa_fixes[0]);
   int fd, ok = 1;
   FILE *sw = fopen("/private/var/mobile/Library/a8gl-mesafix", "r");

   if (sw != NULL) {
      int c = fgetc(sw);

      fclose(sw);
      if (c == '0') {
         say("glca16: Mesa fix off (a8gl-mesafix \"0\"): the original library");
         return NULL;
      }
   }
   if (stat(MESA_SRC, &src) != 0)
      return NULL;
   /* a copy made before: the right size and every fix in place */
   if (stat(MESA_FIXED, &dst) == 0 && dst.st_size == src.st_size &&
       (fd = open(MESA_FIXED, O_RDONLY)) >= 0) {
      for (size_t i = 0; i < n && ok; i++) {
         unsigned char now[20];
         size_t len = mesa_fixes[i].length;

         ok = pread(fd, now, len, (off_t)mesa_fixes[i].offset) == (ssize_t)len &&
              memcmp(now, mesa_fixes[i].fixed, len) == 0;
      }
      close(fd);
      if (ok)
         return MESA_FIXED;
   }
   /* make it: read, check, patch, write beside, rename */
   b = malloc((size_t)src.st_size);
   fd = b != NULL ? open(MESA_SRC, O_RDONLY) : -1;
   if (fd < 0 || read(fd, b, (size_t)src.st_size) != (ssize_t)src.st_size) {
      say("glca16: cannot read %s: the original library", MESA_SRC);
      if (fd >= 0)
         close(fd);
      free(b);
      return NULL;
   }
   close(fd);
   if (!mesa_uuid_matches(b, (size_t)src.st_size)) {
      say("glca16: %s is not the Mesa build the fix is for (UUID): the original library", MESA_SRC);
      free(b);
      return NULL;
   }
   for (size_t i = 0; i < n; i++) {
      size_t len = mesa_fixes[i].length;

      if (mesa_fixes[i].offset + len > (size_t)src.st_size ||
          memcmp(b + mesa_fixes[i].offset, mesa_fixes[i].was, len) != 0) {
         say("glca16: %s has other bytes at %#zx: the original library", MESA_SRC, mesa_fixes[i].offset);
         free(b);
         return NULL;
      }
      memcpy(b + mesa_fixes[i].offset, mesa_fixes[i].fixed, len);
   }
   fd = open(MESA_FIXED ".new", O_WRONLY | O_CREAT | O_TRUNC, 0644);
   ok = fd >= 0 && write(fd, b, (size_t)src.st_size) == (ssize_t)src.st_size && fsync(fd) == 0;
   if (fd >= 0)
      close(fd);
   free(b);
   if (!ok || rename(MESA_FIXED ".new", MESA_FIXED) != 0) {
      say("glca16: cannot write %s: the original library", MESA_FIXED);
      return NULL;
   }
   say("glca17b: %s made from %s with %zu fixes (preload padding descriptor; madvise "
       "answered in place)", MESA_FIXED, MESA_SRC, n);
   return MESA_FIXED;
}

/*
 * glca18: a rebuilt Mesa (-O3, the two fixes above as source) is shipped
 * compressed, because the console's file put takes at most 4 MiB a file and
 * one put cannot outlast the bridge's 180 s: /private/var/mobile/Library/
 * libOSMesa.8.next.z.0, .z.1, ... joined are "A8Z1", the library's size (8
 * bytes, little-endian), its SHA-256 (32 bytes), then a zlib stream.  The
 * shim inflates that once into libOSMesa.8.next.dylib (size and hash checked,
 * the hash kept beside it) and loads it when a8gl-mesa says "1".  libz and
 * CommonCrypto are reached with dlopen/dlsym, so the shim links nothing new.
 * Anything wrong: the patched copy (glca16), then the original.
 */
#define MESA_NEXT     "/private/var/mobile/Library/libOSMesa.8.next.dylib"
#define MESA_NEXT_PKG "/private/var/mobile/Library/libOSMesa.8.next.z"
#define MESA_PKG_MAX  64

static int
mesa_next_wanted(void)
{
   FILE *f = fopen("/private/var/mobile/Library/a8gl-mesa", "r");
   int c = f != NULL ? fgetc(f) : EOF;

   if (f != NULL)
      fclose(f);
   return c == '1';
}

static void
hex32(const unsigned char *h, char out[65])
{
   for (int i = 0; i < 32; i++)
      snprintf(out + 2 * i, 3, "%02x", h[i]);
}

static unsigned char *
read_whole(const char *path, size_t *size)
{
   struct stat st;
   unsigned char *b;
   int fd = open(path, O_RDONLY);

   if (fd < 0)
      return NULL;
   if (fstat(fd, &st) != 0 || st.st_size <= 0 || (b = malloc((size_t)st.st_size)) == NULL) {
      close(fd);
      return NULL;
   }
   if (read(fd, b, (size_t)st.st_size) != st.st_size) {
      free(b);
      close(fd);
      return NULL;
   }
   close(fd);
   *size = (size_t)st.st_size;
   return b;
}

static const char *
mesa_next(void)
{
   typedef int (*inflate_init_fn)(z_streamp, int, const char *, int);
   typedef int (*inflate_fn)(z_streamp, int);
   typedef int (*inflate_end_fn)(z_streamp);
   typedef int (*sha_init_fn)(CC_SHA256_CTX *);
   typedef int (*sha_update_fn)(CC_SHA256_CTX *, const void *, CC_LONG);
   typedef int (*sha_final_fn)(unsigned char *, CC_SHA256_CTX *);
   void *libz;
   inflate_init_fn zinit;
   inflate_fn zinflate;
   inflate_end_fn zend;
   sha_init_fn hinit;
   sha_update_fn hupdate;
   sha_final_fn hfinal;
   unsigned char hdr[44], got[32], *out = NULL;
   char want_hex[65], have_hex[65] = "", part[256];
   uint64_t raw_size = 0, written = 0;
   size_t hdr_have = 0;
   struct stat st;
   z_stream z;
   CC_SHA256_CTX ctx;
   int fd = -1, zret = Z_OK, ok = 0, parts = 0, started = 0;
   FILE *f;

   if (!mesa_next_wanted())
      return NULL;
   hinit = (sha_init_fn)dlsym(RTLD_DEFAULT, "CC_SHA256_Init");
   hupdate = (sha_update_fn)dlsym(RTLD_DEFAULT, "CC_SHA256_Update");
   hfinal = (sha_final_fn)dlsym(RTLD_DEFAULT, "CC_SHA256_Final");
   libz = dlopen("/usr/lib/libz.1.dylib", RTLD_NOW | RTLD_LOCAL);
   zinit = libz != NULL ? (inflate_init_fn)dlsym(libz, "inflateInit2_") : NULL;
   zinflate = libz != NULL ? (inflate_fn)dlsym(libz, "inflate") : NULL;
   zend = libz != NULL ? (inflate_end_fn)dlsym(libz, "inflateEnd") : NULL;
   if (hinit == NULL || hupdate == NULL || hfinal == NULL || zinit == NULL || zinflate == NULL || zend == NULL) {
      say("glca18: libz or CommonCrypto not found: the old Mesa");
      return NULL;
   }

   /* the package's header (in part 0) says which library it holds */
   {
      size_t n = 0;
      unsigned char *p0 = read_whole(MESA_NEXT_PKG ".0", &n);

      if (p0 == NULL || n < sizeof(hdr) || memcmp(p0, "A8Z1", 4) != 0) {
         free(p0);
         /* no package: an already unpacked library still counts */
         f = fopen(MESA_NEXT ".sha256", "r");
         if (f != NULL && fgets(have_hex, sizeof(have_hex), f) != NULL && stat(MESA_NEXT, &st) == 0) {
            fclose(f);
            say("glca18: %s (sha256 %.16s..., no package beside it)", MESA_NEXT, have_hex);
            return MESA_NEXT;
         }
         if (f != NULL)
            fclose(f);
         say("glca18: a8gl-mesa says 1 but there is no %s.0: the old Mesa", MESA_NEXT_PKG);
         return NULL;
      }
      memcpy(hdr, p0, sizeof(hdr));
      free(p0);
   }
   for (int i = 0; i < 8; i++)
      raw_size |= (uint64_t)hdr[4 + i] << (8 * i);
   hex32(hdr + 12, want_hex);
   f = fopen(MESA_NEXT ".sha256", "r");
   if (f != NULL) {
      if (fgets(have_hex, sizeof(have_hex), f) == NULL)
         have_hex[0] = '\0';
      fclose(f);
   }
   if (stat(MESA_NEXT, &st) == 0 && (uint64_t)st.st_size == raw_size && strcmp(have_hex, want_hex) == 0) {
      say("glca18: %s is unpacked already (%llu bytes, sha256 %.16s...)", MESA_NEXT,
          (unsigned long long)raw_size, want_hex);
      return MESA_NEXT;
   }

   /* inflate every part in order into MESA_NEXT.new, hashing what comes out */
   out = malloc(1u << 20);
   memset(&z, 0, sizeof(z));
   if (out == NULL || zinit(&z, 15, ZLIB_VERSION, (int)sizeof(z_stream)) != Z_OK) {
      free(out);
      say("glca18: inflate would not start: the old Mesa");
      return NULL;
   }
   started = 1;
   hinit(&ctx);
   fd = open(MESA_NEXT ".new", O_WRONLY | O_CREAT | O_TRUNC, 0644);
   for (parts = 0; fd >= 0 && parts < MESA_PKG_MAX && zret != Z_STREAM_END; parts++) {
      size_t n = 0, skip;
      unsigned char *b;

      snprintf(part, sizeof(part), "%s.%d", MESA_NEXT_PKG, parts);
      b = read_whole(part, &n);
      if (b == NULL)
         break;
      skip = parts == 0 ? sizeof(hdr) : 0;
      hdr_have += n;
      z.next_in = b + skip;
      z.avail_in = (uInt)(n - skip);
      while (z.avail_in > 0 && zret == Z_OK) {
         z.next_out = out;
         z.avail_out = 1u << 20;
         zret = zinflate(&z, Z_NO_FLUSH);
         if (zret != Z_OK && zret != Z_STREAM_END)
            break;
         {
            size_t have = (1u << 20) - z.avail_out;

            if (have > 0) {
               if (write(fd, out, have) != (ssize_t)have) {
                  zret = Z_ERRNO;
                  break;
               }
               hupdate(&ctx, out, (CC_LONG)have);
               written += have;
            }
         }
      }
      free(b);
   }
   hfinal(got, &ctx);
   hex32(got, have_hex);
   if (started)
      zend(&z);
   free(out);
   ok = fd >= 0 && zret == Z_STREAM_END && written == raw_size && strcmp(have_hex, want_hex) == 0;
   if (fd >= 0) {
      if (ok && fsync(fd) != 0)
         ok = 0;
      close(fd);
   }
   if (!ok || rename(MESA_NEXT ".new", MESA_NEXT) != 0) {
      say("glca18: package not unpacked (%d parts, %llu bytes read, zlib %d, %llu of %llu bytes, "
          "sha256 %.16s... wanted %.16s...): the old Mesa", parts, (unsigned long long)hdr_have, zret,
          (unsigned long long)written, (unsigned long long)raw_size, have_hex, want_hex);
      unlink(MESA_NEXT ".new");
      return NULL;
   }
   f = fopen(MESA_NEXT ".sha256", "w");
   if (f != NULL) {
      fputs(want_hex, f);
      fclose(f);
   }
   say("glca18: %s unpacked from %d parts (%llu bytes, sha256 %.16s...)", MESA_NEXT, parts,
       (unsigned long long)written, want_hex);
   return MESA_NEXT;
}

/*
 * Where the library may be: the one beside this one first (the phone's root has
 * both in the same directory), then the same library a directory up, then
 * whatever the loader's own search finds.  JACKPOTLTE_GL_LIBRARY answers for
 * all of them, which is how a build under test is pointed at a new one.
 */
static int
open_library(void)
{
   static const char *const beside[] = {
      "@loader_path/libOSMesa.8.dylib",
      "@loader_path/../lib/libOSMesa.8.dylib",
      "libOSMesa.8.dylib",
      /* a shim updated over USB lives in /private/var/mobile/Library;
       * Mesa stays where the image put it */
      "/usr/local/lib/libOSMesa.8.dylib",
   };
   static int tried;
   const char *wanted;
   unsigned i;

   if (tried)
      return lib.handle != NULL ? 0 : -1;
   tried = 1;

   /* glca20: Mesa's own debug flags for this backboardd (switch file
    * a8gl-mesadebug, e.g. "perf"), its log in a8gl-mesa-log.txt instead of on
    * the console.  Read before Mesa is loaded: the screen takes them once. */
   {
      FILE *f = fopen("/private/var/mobile/Library/a8gl-mesadebug", "r");
      char flags[64];

      if (f != NULL) {
         size_t n = fread(flags, 1, sizeof(flags) - 1, f);

         fclose(f);
         while (n > 0 && (flags[n - 1] == '\n' || flags[n - 1] == '\r' || flags[n - 1] == ' '))
            n--;
         flags[n] = '\0';
         if (n > 0) {
            setenv("PAN_MESA_DEBUG", flags, 1);
            setenv("MESA_LOG_FILE", "/private/var/mobile/Library/a8gl-mesa-log.txt", 1);
            say("glca20: PAN_MESA_DEBUG=%s, Mesa's log in a8gl-mesa-log.txt", flags);
         }
      }
   }
   /* glca25: Mesa without GL error checks (KHR_no_error, Mesa's documented
    * MESA_NO_ERROR): every GL call CoreAnimation makes skips its validation.
    * Switch a8gl-noerror "1" (read before Mesa is loaded, like the above). */
   {
      FILE *f = fopen("/private/var/mobile/Library/a8gl-noerror", "r");
      int c = f != NULL ? fgetc(f) : EOF;

      if (f != NULL)
         fclose(f);
      if (c == '1') {
         setenv("MESA_NO_ERROR", "1", 1);
         say("glca25: MESA_NO_ERROR=1 (a8gl-noerror \"1\")");
      }
   }

   wanted = getenv("JACKPOTLTE_GL_LIBRARY");
   if (wanted != NULL && wanted[0] != '\0') {
      lib.handle = dlopen(wanted, RTLD_NOW | RTLD_LOCAL);
      lib.where = wanted;
   }
   if (lib.handle == NULL) {
      const char *next = mesa_next();             /* glca18 */

      if (next != NULL) {
         lib.handle = dlopen(next, RTLD_NOW | RTLD_LOCAL);
         lib.where = next;
         if (lib.handle == NULL)
            say("glca18: %s did not load (%s): the old Mesa", next, dlerror());
      }
   }
   if (lib.handle == NULL) {
      const char *fixed = mesa_fixed_copy();      /* glca16 */

      if (fixed != NULL) {
         lib.handle = dlopen(fixed, RTLD_NOW | RTLD_LOCAL);
         lib.where = fixed;
         if (lib.handle == NULL)
            say("glca16: %s did not load (%s): the original library", fixed, dlerror());
      }
   }
   for (i = 0; i < sizeof(beside) / sizeof(beside[0]) && lib.handle == NULL; i++) {
      lib.handle = dlopen(beside[i], RTLD_NOW | RTLD_LOCAL);
      lib.where = beside[i];
   }
   if (lib.handle == NULL) {
      lib.where = "none";
      say("no OpenGL ES library: %s", dlerror());
      return -1;
   }

   lib.create_context = (void *(*)(const int *, void *))
      dlsym(lib.handle, "OSMesaCreateContextAttribs");
   lib.destroy_context = (void (*)(void *))dlsym(lib.handle, "OSMesaDestroyContext");
   lib.make_current = (unsigned char (*)(void *, void *, unsigned, int, int))
      dlsym(lib.handle, "OSMesaMakeCurrent");
   lib.get_proc = (jackpotlte_gl_func (*)(const char *))
      dlsym(lib.handle, "OSMesaGetProcAddress");
   if (!lib.create_context || !lib.destroy_context || !lib.make_current || !lib.get_proc) {
      say("%s is not an OSMesa library: no OSMesaCreateContextAttribs, "
          "OSMesaDestroyContext, OSMesaMakeCurrent or OSMesaGetProcAddress", lib.where);
      dlclose(lib.handle);
      lib.handle = NULL;
      return -1;
   }
   say("the library is %s", lib.where);
   /* Bring-up aid: where the two images of this GL path sit.  The kernel's
    * crash report prints the faulting pc but no image base, so these numbers
    * are what turn that pc into an offset into one of the files built here.
    * A dylib's __TEXT is at vmaddr 0, so the header address is the base. */
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
   return 0;
}

const char *
jackpotlte_gl_library(void)
{
   return lib.handle != NULL ? lib.where : "none";
}

struct jackpotlte_gl *
jackpotlte_gl_create(int version)
{
   int attribs[] = {
      OSMESA_FORMAT, OSMESA_RGBA,
      OSMESA_PROFILE, OSMESA_ES_PROFILE,
      OSMESA_CONTEXT_MAJOR_VERSION, version,
      OSMESA_CONTEXT_MINOR_VERSION, 0,
      0,
   };
   struct jackpotlte_gl *gl;

   if (version < 2 || version > 3) {
      say("no context of OpenGL ES %d: this shim makes 2 and 3", version);
      return NULL;
   }
   if (open_library() != 0)
      return NULL;

   gl = calloc(1, sizeof(*gl));
   if (gl == NULL)
      return NULL;
   gl->window = calloc((size_t)WINDOW * WINDOW, 4);
   gl->context = lib.create_context(attribs, NULL);
   if (gl->window == NULL || gl->context == NULL) {
      /* The context is NULL when the library does not take the ES profile
       * attribute, which is what Mesa without 0004 does. */
      say("no OpenGL ES %d context from %s", version, jackpotlte_gl_library());
      free(gl->window);
      free(gl);
      return NULL;
   }
   return gl;
}

/* glca15: real OSMesaMakeCurrent calls (a thread changing contexts), for the
 * perf2 line */
unsigned long jackpotlte_gl_switches;

int
jackpotlte_gl_make_current(struct jackpotlte_gl *gl)
{
   if (gl == current)
      return 1;
   jackpotlte_gl_switches++;

   /* NULL, NULL: no context on this thread, which is how OSMesa is told. */
   if (!lib.make_current(gl != NULL ? gl->context : NULL,
                         gl != NULL ? gl->window : NULL,
                         gl != NULL ? GL_UNSIGNED_BYTE : 0,
                         gl != NULL ? WINDOW : 0,
                         gl != NULL ? WINDOW : 0)) {
      say("OSMesaMakeCurrent failed");
      return 0;
   }
   current = gl;
   return 1;
}

void
jackpotlte_gl_destroy(struct jackpotlte_gl *gl)
{
   if (gl == NULL)
      return;
   if (current == gl) {
      lib.make_current(NULL, NULL, 0, 0, 0);
      current = NULL;
   }
   lib.destroy_context(gl->context);
   free(gl->window);
   free(gl);
}

jackpotlte_gl_func
jackpotlte_gl_proc(const char *name)
{
   /* Bring-up aid: the first time a name is resolved is the first time the
    * caller reaches that entry point (gl_slots.c asks once), so this log is
    * the order CoreAnimation's GL calls are made in -- and the last one before
    * a crash names the call it died in.  The strings are the table's own
    * literals; only their addresses are kept. */
   static const char *seen[96];
   static unsigned nseen;

   if (lib.handle == NULL && open_library() != 0)
      return NULL;
   if (name != NULL && nseen < sizeof(seen) / sizeof(seen[0])) {
      unsigned i;

      for (i = 0; i < nseen; i++) {
         if (strcmp(seen[i], name) == 0)
            break;
      }
      if (i == nseen) {
         seen[nseen++] = name;
         say("gl %s", name);
      }
   }
   return lib.get_proc(name);
}

int
jackpotlte_gl_surface_texture(struct jackpotlte_gl *gl, uint32_t surface)
{
   static void (*target_texture)(unsigned target, void *image);
   static unsigned (*get_error)(void);
   unsigned error;

   if (gl == NULL || surface == 0)
      return 0;
   if (target_texture == NULL) {
      /* The library's own names for these: the table's entries are resolved the
       * same way, and for the same reason. */
      target_texture = (void (*)(unsigned, void *))
         jackpotlte_gl_proc("glEGLImageTargetTexture2DOES");
      get_error = (unsigned (*)(void))jackpotlte_gl_proc("glGetError");
      if (target_texture == NULL || get_error == NULL) {
         say("the library has no glEGLImageTargetTexture2DOES: a surface cannot "
             "be a texture");
         target_texture = NULL;
         return 0;
      }
   }
   if (current != gl)
      jackpotlte_gl_make_current(gl);

   /* Left over from earlier calls: what this call did is the next one. */
   while (get_error() != GL_NO_ERROR) {
   }
   /* What glEGLImageTargetTexture2DOES is given here is the SweetSurface's ID:
     * with no EGL there is no image object, and 0003's frontend hook reads it as
     * the surface the kernel's driver has a buffer of. */
   target_texture(GL_TEXTURE_2D, (void *)(uintptr_t)surface);
   error = get_error();
   if (error != GL_NO_ERROR) {
      say("surface %u is not a texture here (GL error %#x)", surface, error);
      return 0;
   }
   return 1;
}
