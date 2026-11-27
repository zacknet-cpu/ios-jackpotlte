/* SPDX-License-Identifier: MIT */
/*
 * The crash trap (bring-up aid).
 *
 * The kernel's crash report prints sig, the faulting pc/lr/sp/fp and a raw
 * frame-pointer walk, but neither the image an address is in nor that image's
 * base.  On the phone there is no atos and no way to take the report off the
 * device, so those numbers alone name nothing: `pc 0x1ef9e9e70` is only a
 * class of bug, not a place in a file.
 *
 * This library is injected into the display server (0159's jtexecinject), so
 * it can answer that question from inside the dying process: it installs a
 * SIGSEGV/SIGBUS/SIGILL handler that writes every address of the fault out as
 * "image + offset (base)", then returns with the disposition reset to SIG_DFL
 * and lets the faulting instruction re-execute - which kills the process the
 * normal way, so the kernel's own report still follows.
 *
 * The image table is built once, at load, and the handler only reads it:
 * _dyld_image_count() and friends take dyld's own lock, and a fault taken
 * while dyld holds that lock (a lazy bind, a dlopen) would hang the handler on
 * its second query - which is exactly what happened on jt60, where the handler
 * printed `lr` and then stopped.
 *
 * The output goes to stderr, which for a launchd daemon is the console - the
 * same place this library's other lines reach the panel.  A signal handler may
 * only call async-signal-safe functions, and snprintf is not one of them; it is
 * used anyway, because the alternative is a hand-written formatter and this
 * code runs once, in a process that is already dying.
 */
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>

#define FRAMES 24
#define IMAGES 768

/* One loaded image: the mapped __TEXT (what a code address lands in) and the
 * address the Mach-O header is at, which is what an offset is from. */
struct image {
   uintptr_t text;
   uintptr_t end;
   uintptr_t base;
   const char *name;
};

static struct image images[IMAGES];
static unsigned nimages;

static void
read_images(void)
{
   uint32_t i, n = _dyld_image_count();

   for (i = 0; i < n && nimages < IMAGES; i++) {
      const struct mach_header_64 *header =
          (const struct mach_header_64 *)_dyld_get_image_header(i);
      const char *where = _dyld_get_image_name(i);
      intptr_t slide = _dyld_get_image_vmaddr_slide(i);
      const uint8_t *command, *end;
      uintptr_t text = 0, text_end = 0;

      if (header == NULL || where == NULL || header->magic != MH_MAGIC_64) {
         continue;
      }
      command = (const uint8_t *)(header + 1);
      end = command + header->sizeofcmds;
      while (command + sizeof(struct load_command) <= end) {
         const struct load_command *lc = (const struct load_command *)command;

         if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg =
                (const struct segment_command_64 *)command;

            if (memcmp(seg->segname, "__TEXT", 7) == 0) {
               text = (uintptr_t)seg->vmaddr + (uintptr_t)slide;
               text_end = text + (uintptr_t)seg->vmsize;
               break;           /* one __TEXT a Mach-O */
            }
         }
         if (lc->cmdsize < sizeof(*lc)) {
            break;
         }
         command += lc->cmdsize;
      }
      if (text == 0) {
         continue;
      }
      images[nimages].text = text;
      images[nimages].end = text_end;
      images[nimages].base = (uintptr_t)header;
      images[nimages].name = where;
      nimages++;
   }
}

static void
emit(const char *label, int index, uintptr_t address)
{
   const struct image *found = NULL;
   const char *leaf = NULL;
   char line[400];
   int length;
   unsigned i;

   for (i = 0; i < nimages; i++) {
      if (address >= images[i].text && address < images[i].end) {
         found = &images[i];
         break;
      }
   }
   if (found != NULL) {
      leaf = strrchr(found->name, '/');
      leaf = leaf != NULL ? leaf + 1 : found->name;
      length = snprintf(line, sizeof(line),
          "JTTRAP %s %u %#llx = %s + %#llx (base %#llx)\n", label, index,
          (unsigned long long)address, leaf,
          (unsigned long long)(address - found->base),
          (unsigned long long)found->base);
   } else {
      length = snprintf(line, sizeof(line), "JTTRAP %s %u %#llx = no image\n",
          label, index, (unsigned long long)address);
   }
   if (length > 0) {
      ssize_t wrote = write(2, line,
          (size_t)(length < (int)sizeof(line) ? length : (int)sizeof(line) - 1));

      (void)wrote;
   }
}

/*
 * glca29 (P14 Always-On Display, 11-27): CoreAnimation's flipbook renders the
 * AOD frames off-screen through a CA::OGL::Context whose GL state cache (the
 * pointer at context+16: bound texture target per unit at +144, texture at
 * +176) was never set, because Apple ships the flipbook only with Metal.  The
 * first texture made for an IOSurface then stores through NULL (QuartzCore
 * 16.7 +0x182fb8: ldr x10,[x21,#16]; ...; strh w8,[x10,#144]) and backboardd
 * dies at every lock.  Here such a fault is mended once per context: the
 * faulting instruction's base register is NULL, one of the six instructions
 * before it loaded that register from [Xc,#16] (LDR Xt,[Xn,#16]), and Xc
 * looks like a CA::OGL::Context (the GL function table at +2040 is set): the
 * context gets a zeroed state block (a fresh GL context has nothing bound),
 * the register is pointed at it and the instruction runs again.  At most
 * STATE_BLOCKS contexts; anything else goes the usual way.
 */
#define STATE_BLOCKS 16
#define STATE_BYTES 4096
static uint8_t state_blocks[STATE_BLOCKS][STATE_BYTES] __attribute__((aligned(64)));
static unsigned state_used;

static const struct image *
image_named(const char *leafname)
{
   unsigned i;

   for (i = 0; i < nimages; i++) {
      const char *leaf = strrchr(images[i].name, '/');

      leaf = leaf != NULL ? leaf + 1 : images[i].name;
      if (strcmp(leaf, leafname) == 0) {
         return &images[i];
      }
   }
   return NULL;
}

static int
mend_gl_state(siginfo_t *info, ucontext_t *uc)
{
   const struct image *qc = image_named("QuartzCore");
   uint64_t pc, far, *regs;
   uint32_t insn, base;
   int back;

   if (uc == NULL || info == NULL || qc == NULL || state_used >= STATE_BLOCKS) {
      return 0;
   }
   pc = (uint64_t)uc->uc_mcontext->__ss.__pc;
   far = (uint64_t)(uintptr_t)info->si_addr;
   if (far >= 0x1000 || pc < qc->text || pc + 4 > qc->end || (pc & 3) != 0) {
      return 0;
   }
   regs = (uint64_t *)uc->uc_mcontext->__ss.__x;
   insn = *(const uint32_t *)(uintptr_t)pc;
   /* load/store, unsigned immediate offset: bits 29-27 = 111, bit 24 = 1 */
   if ((insn & 0x3b000000) != 0x39000000) {
      return 0;
   }
   base = (insn >> 5) & 31;
   if (base >= 29 || regs[base] != 0) {
      return 0;
   }
   for (back = 1; back <= 6; back++) {
      uint64_t at = pc - 4u * (unsigned)back;
      uint32_t prev, rn;
      uint64_t ctx;

      if (at < qc->text) {
         break;
      }
      prev = *(const uint32_t *)(uintptr_t)at;
      /* LDR Xt, [Xn, #16]: 0xF9400000 | imm12 2 << 10 | Rn << 5 | Rt */
      if ((prev & 0xfffffc1f) != (0xf9400800u | base)) {
         continue;
      }
      rn = (prev >> 5) & 31;
      ctx = rn < 29 ? regs[rn] : 0;
      if (ctx < 0x100000000ull || (ctx & 7) != 0 ||
          ((const uint64_t *)(uintptr_t)ctx)[2] != 0 ||
          ((const uint64_t *)(uintptr_t)ctx)[2040 / 8] == 0) {
         return 0;
      }
      /* +16 is the state CA wants, +24 the state last given to GL (the
       * texture-unit bind at QuartzCore +0x23293c compares the two); both
       * are unset in this context, and a fresh one has nothing bound */
      if (state_used + 2 > STATE_BLOCKS) {
         return 0;
      }
      ((uint64_t *)(uintptr_t)ctx)[2] = (uint64_t)(uintptr_t)state_blocks[state_used];
      regs[base] = (uint64_t)(uintptr_t)state_blocks[state_used];
      state_used++;
      if (((const uint64_t *)(uintptr_t)ctx)[3] == 0) {
         ((uint64_t *)(uintptr_t)ctx)[3] = (uint64_t)(uintptr_t)state_blocks[state_used];
         state_used++;
      }
      {
         char line[160];
         int length = snprintf(line, sizeof(line),
             "JTFIX glca29: QuartzCore + %#llx: GL state of context %#llx was NULL, gave it blocks (+16/+24), %u used\n",
             (unsigned long long)(pc - qc->base), (unsigned long long)ctx, state_used);
         if (length > 0) {
            ssize_t wrote = write(2, line,
                (size_t)(length < (int)sizeof(line) ? length : (int)sizeof(line) - 1));
            (void)wrote;
         }
      }
      return 1;
   }
   return 0;
}

static void install_handlers(void);

static void
trap(int sig, siginfo_t *info, void *context)
{
   ucontext_t *uc = (ucontext_t *)context;
   uint64_t pc = 0, far = 0, fp = 0;
   char line[96];
   int length, frame;

   if (sig == SIGSEGV || sig == SIGBUS) {
      if (mend_gl_state(info, uc)) {
         install_handlers();    /* SA_RESETHAND took ours off */
         return;                /* the instruction runs again, now with a state block */
      }
   }
   if (uc != NULL) {
      pc = (uint64_t)uc->uc_mcontext->__ss.__pc;
      fp = (uint64_t)uc->uc_mcontext->__ss.__fp;
   }
   far = (uint64_t)(uintptr_t)(info != NULL ? info->si_addr : NULL);

   /* The first four lines are the ones that must survive a handler that does
    * not: far and pc say where, lr and sp what led there. */
   length = snprintf(line, sizeof(line), "JTTRAP sig %d code %d, %u images\n",
       sig, info != NULL ? info->si_code : 0, nimages);
   if (length > 0) {
      ssize_t wrote = write(2, line,
          (size_t)(length < (int)sizeof(line) ? length : (int)sizeof(line) - 1));

      (void)wrote;
   }
   emit("far", 0, (uintptr_t)far);
   emit("pc", 0, (uintptr_t)pc);
   if (uc != NULL) {
      emit("lr", 0, (uintptr_t)uc->uc_mcontext->__ss.__lr);
      emit("sp", 0, (uintptr_t)uc->uc_mcontext->__ss.__sp);
   }

   /* AArch64 frame records: [fp] is the caller's fp, [fp + 8] its return
    * address.  Both must move up the stack for the walk to make sense; a frame
    * pointer that does not is where the walk stops, not an address to read. */
   for (frame = 1; frame <= FRAMES && fp >= 0x1000 && (fp & 0xf) == 0; frame++) {
      uint64_t next = ((const uint64_t *)(uintptr_t)fp)[0];
      uint64_t ret = ((const uint64_t *)(uintptr_t)fp)[1];

      emit("frame", frame, (uintptr_t)ret);
      if (next <= fp) {
         break;
      }
      fp = next;
   }

   /* SA_RESETHAND has already put SIG_DFL in place: returning re-executes the
    * faulting instruction, which faults again and dies the normal way, with
    * the kernel's report and a corpse launchd understands.  raise() would set
    * the signal pending while it is blocked - our handler would then pause()
    * forever and the process would look alive with a stuck thread, which is
    * what jt60 showed. */
}

static void
install_handlers(void)
{
   struct sigaction action;
   int signals[] = { SIGSEGV, SIGBUS, SIGILL };
   unsigned i;

   memset(&action, 0, sizeof(action));
   action.sa_sigaction = trap;
   action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
   sigemptyset(&action.sa_mask);
   for (i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
      (void)sigaction(signals[i], &action, NULL);
   }
}

__attribute__((constructor)) static void
install_trap(void)
{
   static char alternate[64 * 1024];
   stack_t stack;

   read_images();

   stack.ss_sp = alternate;
   stack.ss_size = sizeof(alternate);
   stack.ss_flags = 0;
   (void)sigaltstack(&stack, NULL);
   install_handlers();
}
