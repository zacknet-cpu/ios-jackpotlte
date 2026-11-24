/* SPDX-License-Identifier: MIT */
/*
 * trace.c: what CoreAnimation draws, written to files the console's fget can
 * read (backboardd runs as mobile, so they are under mobile's Library):
 *
 *   a8gl-shaders.txt   every distinct shader CoreAnimation hands over, as it
 *                      wrote it (before ca_fixes.c rewrites it), once; and each
 *                      program it links, as "link P: S/HASH S/HASH".
 *   a8gl-trace.txt     the draws of a few frames: a line for each draw, and the
 *                      whole state (textures, attributes, uniforms) of the
 *                      draws that touch a rectangle of the screen.
 *
 * The trace is armed by the file a8gl-trace-on, "X0 Y0 X1 Y1 FRAMES" (screen
 * pixels, top left origin), and armed again each time that file changes.
 * Nothing is traced, and the draws cost one test, while it is not armed.
 */
#include "gl_slots.h"
#include "jackpotlte_gl.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <mach/mach_time.h>

#define LIBRARY_DIR     "/private/var/mobile/Library/"
#define SHADERS_FILE    LIBRARY_DIR "a8gl-shaders.txt"
#define TRACE_FILE      LIBRARY_DIR "a8gl-trace.txt"
#define TRIGGER_FILE    LIBRARY_DIR "a8gl-trace-on"

#define SHADERS_MAX     (3u << 20)      /* bytes the shader file may grow to */
#define TRACE_MAX       (3u << 20)      /* bytes one trace may write */

#define GL_FLOAT_T                  0x1406
#define GL_BYTE_T                   0x1400
#define GL_UNSIGNED_BYTE_T          0x1401
#define GL_SHORT_T                  0x1402
#define GL_UNSIGNED_SHORT_T         0x1403
#define GL_INT_T                    0x1404
#define GL_UNSIGNED_INT_T           0x1405
#define GL_HALF_FLOAT_T             0x140b
#define GL_HALF_FLOAT_OES_T         0x8d61
#define GL_FLOAT_VEC2               0x8b50
#define GL_FLOAT_VEC3               0x8b51
#define GL_FLOAT_VEC4               0x8b52
#define GL_INT_VEC2                 0x8b53
#define GL_INT_VEC3                 0x8b54
#define GL_INT_VEC4                 0x8b55
#define GL_BOOL_T                   0x8b56
#define GL_FLOAT_MAT2               0x8b5a
#define GL_FLOAT_MAT3               0x8b5b
#define GL_FLOAT_MAT4               0x8b5c
#define GL_SAMPLER_2D               0x8b5e
#define GL_SHADER_TYPE              0x8b4f
#define GL_FRAGMENT_SHADER          0x8b30
#define GL_ACTIVE_UNIFORMS          0x8b86
#define GL_ACTIVE_ATTRIBUTES        0x8b89
#define GL_CURRENT_PROGRAM          0x8b8d
#define GL_FRAMEBUFFER              0x8d40
#define GL_FRAMEBUFFER_BINDING      0x8ca6
#define GL_COLOR_ATTACHMENT0        0x8ce0
#define GL_ATTACHMENT_TYPE          0x8cd0
#define GL_ATTACHMENT_NAME          0x8cd1
#define GL_VIEWPORT                 0x0ba2
#define GL_SCISSOR_BOX              0x0c10
#define GL_SCISSOR_TEST             0x0c11
#define GL_BLEND                    0x0be2
#define GL_BLEND_SRC_RGB            0x80c9
#define GL_BLEND_DST_RGB            0x80c8
#define GL_BLEND_SRC_ALPHA          0x80cb
#define GL_BLEND_DST_ALPHA          0x80ca
#define GL_BLEND_EQUATION_RGB       0x8009
#define GL_BLEND_EQUATION_ALPHA     0x883d
#define GL_COLOR_WRITEMASK          0x0c23
#define GL_STENCIL_TEST             0x0b90
#define GL_DEPTH_TEST               0x0b71
#define GL_ACTIVE_TEXTURE           0x84e0
#define GL_TEXTURE0                 0x84c0
#define GL_TEXTURE_2D               0x0de1
#define GL_TEXTURE_BINDING_2D       0x8069
#define GL_TEXTURE_WIDTH            0x1000
#define GL_TEXTURE_HEIGHT           0x1001
#define GL_TEXTURE_INTERNAL_FORMAT  0x1003
#define GL_TEXTURE_MIN_FILTER       0x2801
#define GL_TEXTURE_WRAP_S           0x2802
#define GL_VERTEX_ATTRIB_ENABLED    0x8622
#define GL_VERTEX_ATTRIB_SIZE       0x8623
#define GL_VERTEX_ATTRIB_STRIDE     0x8624
#define GL_VERTEX_ATTRIB_TYPE       0x8625
#define GL_VERTEX_ATTRIB_NORMALIZED 0x886a
#define GL_VERTEX_ATTRIB_POINTER    0x8645
#define GL_VERTEX_ATTRIB_BUFFER     0x889f
#define GL_CURRENT_VERTEX_ATTRIB    0x8626
#define GL_ELEMENT_ARRAY_BINDING    0x8895

static struct {
   void (*get_integerv)(GLenum, GLint *);
   void (*get_booleanv)(GLenum, GLboolean *);
   GLboolean (*is_enabled)(GLenum);
   void (*active_texture)(GLenum);
   void (*get_tex_level_parameteriv)(GLenum, GLint, GLenum, GLint *);
   void (*get_tex_parameteriv)(GLenum, GLenum, GLint *);
   void (*get_programiv)(GLuint, GLenum, GLint *);
   void (*get_active_uniform)(GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, GLchar *);
   void (*get_active_attrib)(GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, GLchar *);
   GLint (*get_uniform_location)(GLuint, const GLchar *);
   GLint (*get_attrib_location)(GLuint, const GLchar *);
   void (*get_uniformfv)(GLuint, GLint, GLfloat *);
   void (*get_uniformiv)(GLuint, GLint, GLint *);
   void (*get_vertex_attribiv)(GLuint, GLenum, GLint *);
   void (*get_vertex_attribfv)(GLuint, GLenum, GLfloat *);
   void (*get_vertex_attrib_pointerv)(GLuint, GLenum, void **);
   void (*get_shaderiv)(GLuint, GLenum, GLint *);
   void (*get_attached_shaders)(GLuint, GLsizei, GLsizei *, GLuint *);
   void (*get_framebuffer_attachment_parameteriv)(GLenum, GLenum, GLenum, GLint *);
   void (*read_pixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
   void (*bind_framebuffer)(GLenum, GLuint);
   int loaded, good;
} gl;

#define PROC(field, name) gl.field = (__typeof__(gl.field))jackpotlte_gl_proc(name)

static bool
load(void)
{
   if (gl.loaded)
      return gl.good;
   gl.loaded = 1;
   PROC(get_integerv, "glGetIntegerv");
   PROC(get_booleanv, "glGetBooleanv");
   PROC(is_enabled, "glIsEnabled");
   PROC(active_texture, "glActiveTexture");
   PROC(get_tex_level_parameteriv, "glGetTexLevelParameteriv");
   PROC(get_tex_parameteriv, "glGetTexParameteriv");
   PROC(get_programiv, "glGetProgramiv");
   PROC(get_active_uniform, "glGetActiveUniform");
   PROC(get_active_attrib, "glGetActiveAttrib");
   PROC(get_uniform_location, "glGetUniformLocation");
   PROC(get_attrib_location, "glGetAttribLocation");
   PROC(get_uniformfv, "glGetUniformfv");
   PROC(get_uniformiv, "glGetUniformiv");
   PROC(get_vertex_attribiv, "glGetVertexAttribiv");
   PROC(get_vertex_attribfv, "glGetVertexAttribfv");
   PROC(get_vertex_attrib_pointerv, "glGetVertexAttribPointerv");
   PROC(get_shaderiv, "glGetShaderiv");
   PROC(get_attached_shaders, "glGetAttachedShaders");
   PROC(get_framebuffer_attachment_parameteriv, "glGetFramebufferAttachmentParameteriv");
   PROC(read_pixels, "glReadPixels");
   PROC(bind_framebuffer, "glBindFramebuffer");
   gl.good = gl.get_integerv && gl.get_booleanv && gl.is_enabled && gl.active_texture &&
             gl.get_tex_level_parameteriv && gl.get_tex_parameteriv && gl.get_programiv &&
             gl.get_active_uniform && gl.get_active_attrib && gl.get_uniform_location &&
             gl.get_attrib_location && gl.get_uniformfv && gl.get_uniformiv &&
             gl.get_vertex_attribiv && gl.get_vertex_attribfv && gl.get_vertex_attrib_pointerv &&
             gl.get_shaderiv && gl.get_attached_shaders && gl.get_framebuffer_attachment_parameteriv;
   return gl.good;
}

/* ca_fixes.c: the function CoreAnimation is drawing (>= 90: not known), and
 * the matrix it last gave a program of its own */
unsigned ca_fixes_function(void);
const float *ca_fixes_matrix(void);

/* ---- shaders ------------------------------------------------------------ */

#define IDS 4096
static uint64_t shader_hash[IDS];               /* by shader name % IDS */
static uint64_t program_hash[IDS][2];           /* by program name % IDS: vertex, fragment */
static uint64_t seen[2048];
static unsigned seen_count;
static size_t shaders_written;

static uint64_t
fnv(const char *text, size_t length)
{
   uint64_t h = 0xcbf29ce484222325ull;

   for (size_t i = 0; i < length; i++)
      h = (h ^ (unsigned char)text[i]) * 0x100000001b3ull;
   return h;
}

static bool
seen_before(uint64_t h)
{
   for (unsigned i = 0; i < seen_count; i++) {
      if (seen[i] == h)
         return true;
   }
   if (seen_count < sizeof(seen) / sizeof(seen[0]))
      seen[seen_count++] = h;
   return false;
}

static void (*next_shader_source)(void *, GLuint, GLsizei, const GLchar *const *, const GLint *);
static void (*next_link_program)(void *, GLuint);
static void (*next_draw_arrays)(void *, GLenum, GLint, GLsizei);
static void (*next_draw_elements)(void *, GLenum, GLsizei, GLenum, const void *);
static void (*next_viewport)(void *, GLint, GLint, GLsizei, GLsizei);
static GLint screen_framebuffer;        /* the last framebuffer given the screen's viewport */

static void
my_glShaderSource(void *context, GLuint shader, GLsizei count, const GLchar *const *strings,
                  const GLint *lengths)
{
   size_t size = 0;
   char *text;

   for (GLsizei i = 0; strings != NULL && i < count; i++)
      size += lengths != NULL && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);
   text = malloc(size + 1);
   if (text != NULL) {
      size_t at = 0;
      uint64_t h;

      for (GLsizei i = 0; i < count; i++) {
         size_t n = lengths != NULL && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);

         memcpy(text + at, strings[i], n);
         at += n;
      }
      text[at] = '\0';
      h = fnv(text, at);
      shader_hash[shader % IDS] = h;
      if (!seen_before(h) && shaders_written < SHADERS_MAX) {
         FILE *f = fopen(SHADERS_FILE, "a");

         if (f != NULL) {
            GLint type = 0;

            jackpotlte_gl_use(context);
            if (load())
               gl.get_shaderiv(shader, GL_SHADER_TYPE, &type);
            fprintf(f, "== shader %u %s %016llx %zu bytes\n%s\n== end\n", shader,
                    type == GL_FRAGMENT_SHADER ? "fragment" : "vertex", (unsigned long long)h, at,
                    text);
            shaders_written += at + 64;
            fclose(f);
         }
      }
      free(text);
   }
   next_shader_source(context, shader, count, strings, lengths);
}

static void
my_glLinkProgram(void *context, GLuint program)
{
   GLuint shaders[4];
   GLsizei n = 0;

   jackpotlte_gl_use(context);
   if (load()) {
      gl.get_attached_shaders(program, 4, &n, shaders);
      program_hash[program % IDS][0] = program_hash[program % IDS][1] = 0;
      for (GLsizei i = 0; i < n && i < 4; i++) {
         GLint type = 0;

         gl.get_shaderiv(shaders[i], GL_SHADER_TYPE, &type);
         program_hash[program % IDS][type == GL_FRAGMENT_SHADER] = shader_hash[shaders[i] % IDS];
      }
   }
   next_link_program(context, program);
}

/* ---- the trace ---------------------------------------------------------- */

static FILE *trace;
static size_t trace_written;
/* a frame's lines wait here; only a frame with a draw inside the rectangle is
 * written out (and counted), so the trace can be armed before what it is
 * about is on the screen */
#define FRAME_ROOM (1u << 20)
static char *frame_text;
static size_t frame_used;
static bool frame_hit;
static int armed_frames;
static int want_pixels;                 /* the trigger's sixth number: dump the rectangle's pixels */
static int region[4];                   /* x0 y0 x1 y1, screen pixels, top left origin */
static unsigned long trace_frame_number, trace_draw_number;
static time_t trigger_time;
static long trigger_nsec;
static uint64_t last_look;

static void
out(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void
out(const char *format, ...)
{
   va_list arguments;
   int n;

   if (trace == NULL || frame_text == NULL || frame_used + 1024 > FRAME_ROOM)
      return;
   va_start(arguments, format);
   n = vsnprintf(frame_text + frame_used, FRAME_ROOM - frame_used, format, arguments);
   va_end(arguments);
   if (n > 0)
      frame_used += (size_t)n < FRAME_ROOM - frame_used ? (size_t)n : FRAME_ROOM - frame_used - 1;
}

static float
half_to_float(uint16_t h)
{
   uint32_t sign = (uint32_t)(h & 0x8000) << 16, exponent = (h >> 10) & 0x1f, mantissa = h & 0x3ff;
   uint32_t bits;
   float f;

   if (exponent == 0) {
      if (mantissa == 0) {
         bits = sign;
      } else {
         exponent = 127 - 15 + 1;
         while (!(mantissa & 0x400)) {
            mantissa <<= 1;
            exponent--;
         }
         mantissa &= 0x3ff;
         bits = sign | exponent << 23 | mantissa << 13;
      }
   } else if (exponent == 31) {
      bits = sign | 0x7f800000u | mantissa << 13;
   } else {
      bits = sign | (exponent + 127 - 15) << 23 | mantissa << 13;
   }
   memcpy(&f, &bits, sizeof(f));
   return f;
}

static size_t
type_size(GLint type)
{
   switch (type) {
   case GL_BYTE_T:
   case GL_UNSIGNED_BYTE_T:
      return 1;
   case GL_SHORT_T:
   case GL_UNSIGNED_SHORT_T:
   case GL_HALF_FLOAT_T:
   case GL_HALF_FLOAT_OES_T:
      return 2;
   default:
      return 4;
   }
}

static float
component(const unsigned char *p, GLint type, GLint normalized)
{
   switch (type) {
   case GL_FLOAT_T:
      return *(const float *)(const void *)p;
   case GL_HALF_FLOAT_T:
   case GL_HALF_FLOAT_OES_T:
      return half_to_float(*(const uint16_t *)(const void *)p);
   case GL_UNSIGNED_BYTE_T:
      return normalized ? *p / 255.f : *p;
   case GL_BYTE_T:
      return normalized ? *(const int8_t *)p / 127.f : *(const int8_t *)p;
   case GL_UNSIGNED_SHORT_T:
      return normalized ? *(const uint16_t *)(const void *)p / 65535.f : *(const uint16_t *)(const void *)p;
   case GL_SHORT_T:
      return normalized ? *(const int16_t *)(const void *)p / 32767.f : *(const int16_t *)(const void *)p;
   case GL_INT_T:
      return (float)*(const int32_t *)(const void *)p;
   case GL_UNSIGNED_INT_T:
      return (float)*(const uint32_t *)(const void *)p;
   default:
      return 0;
   }
}

struct attribute {
   GLint enabled, size, type, stride, normalized, buffer;
   const unsigned char *pointer;
};

static void
attribute_of(GLuint index, struct attribute *a)
{
   void *pointer = NULL;

   memset(a, 0, sizeof(*a));
   gl.get_vertex_attribiv(index, GL_VERTEX_ATTRIB_ENABLED, &a->enabled);
   gl.get_vertex_attribiv(index, GL_VERTEX_ATTRIB_SIZE, &a->size);
   gl.get_vertex_attribiv(index, GL_VERTEX_ATTRIB_TYPE, &a->type);
   gl.get_vertex_attribiv(index, GL_VERTEX_ATTRIB_STRIDE, &a->stride);
   gl.get_vertex_attribiv(index, GL_VERTEX_ATTRIB_NORMALIZED, &a->normalized);
   gl.get_vertex_attribiv(index, GL_VERTEX_ATTRIB_BUFFER, &a->buffer);
   gl.get_vertex_attrib_pointerv(index, GL_VERTEX_ATTRIB_POINTER, &pointer);
   a->pointer = pointer;
}

/* vertex v of the attribute, four components (missing ones 0, 0, 0, 1) */
static bool
vertex_of(const struct attribute *a, unsigned v, float value[4])
{
   size_t one = type_size(a->type), stride;

   value[0] = value[1] = value[2] = 0;
   value[3] = 1;
   if (!a->enabled || a->buffer != 0 || a->pointer == NULL || a->size < 1 || a->size > 4)
      return false;
   stride = a->stride != 0 ? (size_t)a->stride : one * (size_t)a->size;
   for (GLint c = 0; c < a->size; c++)
      value[c] = component(a->pointer + stride * v + one * (size_t)c, a->type, a->normalized);
   return true;
}

/* the vertex indices a draw uses: up to `room`, how many in all */
static unsigned
indices_of(bool elements, GLint first, GLsizei count, GLenum type, const void *list,
           unsigned *indices, unsigned room)
{
   GLint element_buffer = 0;
   unsigned n = 0;

   if (count <= 0)
      return 0;
   if (!elements) {
      for (GLsizei i = 0; i < count && n < room; i++)
         indices[n++] = (unsigned)(first + i);
      return n;
   }
   gl.get_integerv(GL_ELEMENT_ARRAY_BINDING, &element_buffer);
   if (element_buffer != 0 || list == NULL)
      return 0;
   for (GLsizei i = 0; i < count && n < room; i++) {
      if (type == GL_UNSIGNED_SHORT_T)
         indices[n++] = ((const uint16_t *)list)[i];
      else if (type == GL_UNSIGNED_BYTE_T)
         indices[n++] = ((const uint8_t *)list)[i];
      else
         indices[n++] = ((const uint32_t *)list)[i];
   }
   return n;
}

static void
matrix_of(GLint program, float m[16])
{
   static const float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
   GLint at = program != 0 ? gl.get_uniform_location((GLuint)program, "vertex_matrix") : -1;

   if (at >= 0) {
      gl.get_uniformfv((GLuint)program, at, m);
   } else {
      const float *ca = ca_fixes_matrix();

      memcpy(m, ca != NULL ? ca : identity, 16 * sizeof(float));
   }
}

/* the screen rectangle (top left origin) of the draw, from its positions */
static bool
bounds_of(GLint program, const unsigned *indices, unsigned n, const GLint viewport[4], float box[4])
{
   struct attribute position;
   GLint at = program != 0 ? gl.get_attrib_location((GLuint)program, "vertex_position") : 0;
   float m[16];
   bool any = false;

   if (at < 0)
      at = 0;
   attribute_of((GLuint)at, &position);
   matrix_of(program, m);
   for (unsigned i = 0; i < n; i++) {
      float p[4], c[4], x, y;

      if (!vertex_of(&position, indices[i], p))
         return false;
      for (int r = 0; r < 4; r++)
         c[r] = m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r] * p[3];
      if (c[3] == 0)
         continue;
      x = viewport[0] + (c[0] / c[3] + 1) * .5f * viewport[2];
      y = viewport[1] + (c[1] / c[3] + 1) * .5f * viewport[3];
      /* CoreAnimation's targets have their first row at the top: window y is
       * already the screen's row (for the screen's own target) */
      if (!any) {
         box[0] = box[2] = x;
         box[1] = box[3] = y;
         any = true;
      } else {
         box[0] = x < box[0] ? x : box[0];
         box[2] = x > box[2] ? x : box[2];
         box[1] = y < box[1] ? y : box[1];
         box[3] = y > box[3] ? y : box[3];
      }
   }
   return any;
}

static void
textures_out(void)
{
   GLint active = 0;

   gl.get_integerv(GL_ACTIVE_TEXTURE, &active);
   for (GLenum unit = 0; unit < 4; unit++) {
      GLint name = 0, w = 0, h = 0, format = 0, filter = 0, wrap = 0;

      gl.active_texture(GL_TEXTURE0 + unit);
      gl.get_integerv(GL_TEXTURE_BINDING_2D, &name);
      if (name == 0)
         continue;
      gl.get_tex_level_parameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
      gl.get_tex_level_parameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
      gl.get_tex_level_parameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &format);
      gl.get_tex_parameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &filter);
      gl.get_tex_parameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &wrap);
      out("   unit %u: texture %d %dx%d format %#x filter %#x wrap %#x\n", unit, name, w, h,
          format, filter, wrap);
   }
   gl.active_texture((GLenum)active);
}

static void
attributes_out(GLint program, const unsigned *indices, unsigned n)
{
   GLint count = 0;

   if (program == 0) {
      for (GLuint i = 0; i < 4; i++) {
         struct attribute a;
         float v[4];

         attribute_of(i, &a);
         if (!a.enabled)
            continue;
         out("   attribute %u: %d x %#x stride %d%s", i, a.size, a.type, a.stride,
             a.buffer ? " (buffer)" : "");
         for (unsigned k = 0; k < n && k < 4; k++) {
            if (vertex_of(&a, indices[k], v))
               out(" [%g %g %g %g]", v[0], v[1], v[2], v[3]);
         }
         out("\n");
      }
      return;
   }
   gl.get_programiv((GLuint)program, GL_ACTIVE_ATTRIBUTES, &count);
   for (GLint i = 0; i < count && i < 16; i++) {
      char name[64] = "";
      GLint size = 0, at;
      GLenum type = 0;
      struct attribute a;
      float v[4];

      gl.get_active_attrib((GLuint)program, (GLuint)i, sizeof(name), NULL, &size, &type, name);
      at = gl.get_attrib_location((GLuint)program, name);
      if (at < 0)
         continue;
      attribute_of((GLuint)at, &a);
      if (!a.enabled) {
         gl.get_vertex_attribfv((GLuint)at, GL_CURRENT_VERTEX_ATTRIB, v);
         out("   attribute %s (%d): constant [%g %g %g %g]\n", name, at, v[0], v[1], v[2], v[3]);
         continue;
      }
      out("   attribute %s (%d): %d x %#x%s stride %d%s", name, at, a.size, a.type,
          a.normalized ? " normalized" : "", a.stride, a.buffer ? " (buffer)" : "");
      for (unsigned k = 0; k < n && k < 6; k++) {
         if (vertex_of(&a, indices[k], v))
            out(" [%g %g %g %g]", v[0], v[1], v[2], v[3]);
      }
      out("\n");
   }
}

static void
uniforms_out(GLint program)
{
   GLint count = 0;

   if (program == 0)
      return;
   gl.get_programiv((GLuint)program, GL_ACTIVE_UNIFORMS, &count);
   for (GLint i = 0; i < count && i < 32; i++) {
      char name[64] = "", element[80];
      GLint size = 0, floats;
      GLenum type = 0;
      char *bracket;

      gl.get_active_uniform((GLuint)program, (GLuint)i, sizeof(name), NULL, &size, &type, name);
      bracket = strchr(name, '[');
      if (bracket != NULL)
         *bracket = '\0';
      floats = type == GL_FLOAT_T ? 1 : type == GL_FLOAT_VEC2 ? 2 : type == GL_FLOAT_VEC3 ? 3 :
               type == GL_FLOAT_VEC4 ? 4 : type == GL_FLOAT_MAT2 ? 4 : type == GL_FLOAT_MAT3 ? 9 :
               type == GL_FLOAT_MAT4 ? 16 : 0;
      out("   uniform %s", name);
      for (GLint k = 0; k < size && k < 8; k++) {
         GLint at;

         if (size > 1)
            snprintf(element, sizeof(element), "%s[%d]", name, k);
         else
            snprintf(element, sizeof(element), "%s", name);
         at = gl.get_uniform_location((GLuint)program, element);
         if (at < 0)
            continue;
         if (floats > 0) {
            float v[16];

            gl.get_uniformfv((GLuint)program, at, v);
            out(" [");
            for (GLint c = 0; c < floats; c++)
               out(c == 0 ? "%g" : " %g", v[c]);
            out("]");
         } else {
            GLint v[4] = { 0, 0, 0, 0 };

            gl.get_uniformiv((GLuint)program, at, v);
            out(" <%d %d %d %d>", v[0], v[1], v[2], v[3]);
         }
      }
      out("\n");
   }
}

static bool draw_inside;

static void
trace_draw(bool elements, GLenum mode, GLint first, GLsizei count, GLenum type, const void *list)
{
   static unsigned indices[4096];
   GLint program = 0, framebuffer = 0, attachment = 0, attachment_type = 0, viewport[4] = { 0, 0, 0, 0 };
   GLint scissor[4] = { 0, 0, 0, 0 }, blend[6] = { 0, 0, 0, 0, 0, 0 };
   GLboolean mask[4] = { 1, 1, 1, 1 };
   unsigned n, function = ca_fixes_function();
   float box[4] = { 0, 0, 0, 0 };
   bool have_box, inside;

   if (!load())
      return;
   trace_draw_number++;
   gl.get_integerv(GL_CURRENT_PROGRAM, &program);
   gl.get_integerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
   if (framebuffer != 0) {
      gl.get_framebuffer_attachment_parameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_ATTACHMENT_TYPE,
                                                &attachment_type);
      if (attachment_type != 0)
         gl.get_framebuffer_attachment_parameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                                   GL_ATTACHMENT_NAME, &attachment);
   }
   gl.get_integerv(GL_VIEWPORT, viewport);
   gl.get_integerv(GL_SCISSOR_BOX, scissor);
   gl.get_integerv(GL_BLEND_SRC_RGB, &blend[0]);
   gl.get_integerv(GL_BLEND_DST_RGB, &blend[1]);
   gl.get_integerv(GL_BLEND_SRC_ALPHA, &blend[2]);
   gl.get_integerv(GL_BLEND_DST_ALPHA, &blend[3]);
   gl.get_integerv(GL_BLEND_EQUATION_RGB, &blend[4]);
   gl.get_integerv(GL_BLEND_EQUATION_ALPHA, &blend[5]);
   gl.get_booleanv(GL_COLOR_WRITEMASK, mask);
   n = indices_of(elements, first, count, type, list, indices, sizeof(indices) / sizeof(indices[0]));
   have_box = n > 0 && bounds_of(program, indices, n, viewport, box);
   inside = have_box && box[0] <= region[2] && box[2] >= region[0] && box[1] <= region[3] &&
            box[3] >= region[1];
   out("D %lu.%lu fb %d tex %d vp %d,%d %dx%d sc %s %d,%d %dx%d blend %s %#x %#x %#x %#x eq %#x %#x mask %d%d%d%d "
       "prog %d (%016llx %016llx) fn %u %s %#x n %d box %s%.0f,%.0f-%.0f,%.0f%s\n",
       trace_frame_number, trace_draw_number, framebuffer, attachment, viewport[0], viewport[1], viewport[2],
       viewport[3], gl.is_enabled(GL_SCISSOR_TEST) ? "on" : "off", scissor[0], scissor[1], scissor[2],
       scissor[3], gl.is_enabled(GL_BLEND) ? "on" : "off", blend[0], blend[1], blend[2], blend[3],
       blend[4], blend[5], mask[0], mask[1], mask[2], mask[3], program,
       (unsigned long long)program_hash[program % IDS][0], (unsigned long long)program_hash[program % IDS][1],
       function, elements ? "elements" : "arrays", mode, count, have_box ? "" : "? ", box[0], box[1], box[2],
       box[3], inside ? " *" : "");
   draw_inside = inside;
   if (!inside)
      return;
   frame_hit = true;
   if (gl.is_enabled(GL_STENCIL_TEST) || gl.is_enabled(GL_DEPTH_TEST))
      out("   stencil %d depth %d\n", gl.is_enabled(GL_STENCIL_TEST), gl.is_enabled(GL_DEPTH_TEST));
   textures_out();
   attributes_out(program, indices, n);
   uniforms_out(program);
}

/* want_pixels 2: after a draw inside the rectangle, what it left there (a
 * small rectangle: read straight after the draw, so the GPU is waited for) */
static void pixels_after(void);

static void
my_glDrawArrays(void *context, GLenum mode, GLint first, GLsizei count)
{
   bool traced = armed_frames > 0 && trace != NULL;

   if (traced) {
      jackpotlte_gl_use(context);
      trace_draw(false, mode, first, count, 0, NULL);
   }
   next_draw_arrays(context, mode, first, count);
   if (traced && want_pixels == 2 && draw_inside)
      pixels_after();
}

static void
my_glDrawElements(void *context, GLenum mode, GLsizei count, GLenum type, const void *indices)
{
   bool traced = armed_frames > 0 && trace != NULL;

   if (traced) {
      jackpotlte_gl_use(context);
      trace_draw(true, mode, 0, count, type, indices);
   }
   next_draw_elements(context, mode, count, type, indices);
   if (traced && want_pixels == 2 && draw_inside)
      pixels_after();
}

static void
pixels_after(void)
{
   int w = region[2] - region[0], h = region[3] - region[1];
   uint32_t row[64];

   if (!draw_inside || gl.read_pixels == NULL || w <= 0 || h <= 0 || w > 64 || h > 64)
      return;
   out("   pixels after it, %dx%d from %d,%d:\n", w, h, region[0], region[1]);
   for (int y = 0; y < h; y++) {
      gl.read_pixels(region[0], region[1] + y, w, 1, 0x1908 /* RGBA */, GL_UNSIGNED_BYTE_T, row);
      out("   P %d:", region[1] + y);
      for (int x = 0; x < w; x++) {
         const unsigned char *p = (const unsigned char *)&row[x];

         out(" %02x%02x%02x%02x", p[0], p[1], p[2], p[3]);
      }
      out("\n");
   }
}

/* the rectangle's pixels in the framebuffer bound at the end of the frame
 * (the screen's), a row a line, RRGGBBAA */
static void
pixels_out(void)
{
   GLint framebuffer = 0, viewport[4] = { 0, 0, 0, 0 };
   int w = region[2] - region[0], h = region[3] - region[1];
   uint32_t *row;

   if (gl.read_pixels == NULL || w <= 0 || h <= 0 || w > 256 || h > 256)
      return;
   gl.get_integerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
   gl.get_integerv(GL_VIEWPORT, viewport);
   row = malloc((size_t)w * 4);
   if (row == NULL)
      return;
   if (framebuffer == 0 && screen_framebuffer != 0 && gl.bind_framebuffer != NULL) {
      gl.bind_framebuffer(GL_FRAMEBUFFER, (GLuint)screen_framebuffer);
      fprintf(trace, "== (the screen's framebuffer %d bound for this)\n", screen_framebuffer);
   }
   fprintf(trace, "== pixels of framebuffer %d (viewport %dx%d), %dx%d from %d,%d\n", framebuffer, viewport[2],
           viewport[3], w, h, region[0], region[1]);
   for (int y = 0; y < h; y++) {
      gl.read_pixels(region[0], region[1] + y, w, 1, 0x1908 /* RGBA */, GL_UNSIGNED_BYTE_T, row);
      fprintf(trace, "P %d:", region[1] + y);
      for (int x = 0; x < w; x++) {
         const unsigned char *p = (const unsigned char *)&row[x];

         fprintf(trace, " %02x%02x%02x%02x", p[0], p[1], p[2], p[3]);
      }
      fprintf(trace, "\n");
   }
   free(row);
   if (framebuffer == 0 && screen_framebuffer != 0 && gl.bind_framebuffer != NULL)
      gl.bind_framebuffer(GL_FRAMEBUFFER, 0);
}

static void
my_glViewport(void *context, GLint x, GLint y, GLsizei w, GLsizei h)
{
   next_viewport(context, x, y, w, h);
   if (x == 0 && y == 0 && w == 1080 && h == 2220 && load())
      gl.get_integerv(GL_FRAMEBUFFER_BINDING, &screen_framebuffer);
}

/* eagl.c, at the end of every frame (glFenceSync) */
void
trace_frame(void)
{
   uint64_t now = mach_absolute_time();
   struct stat info;
   static int pixels_now;

   if (pixels_now) {
      /* "X0 Y0 X1 Y1 0 1": the rectangle's pixels at the end of this frame */
      pixels_now = 0;
      trace = fopen(TRACE_FILE, "w");
      if (trace != NULL) {
         if (load())
            pixels_out();
         fprintf(trace, "== done\n");
         fclose(trace);
         trace = NULL;
      }
   }

   if (armed_frames > 0 && trace != NULL) {
      out("F %lu ends after %lu draws\n", trace_frame_number, trace_draw_number);
      trace_draw_number = 0;
      if (frame_hit && frame_text != NULL) {
         fwrite(frame_text, 1, frame_used, trace);
         trace_written += frame_used;
         trace_frame_number++;
         armed_frames--;
         if (trace_written > TRACE_MAX)
            armed_frames = 0;
         if (armed_frames == 0 && want_pixels)
            pixels_out();
         if (armed_frames == 0) {
            fprintf(trace, "== done\n");
            fclose(trace);
            trace = NULL;
         } else {
            fflush(trace);
         }
      }
      frame_used = 0;
      frame_hit = false;
   }
   /* the trigger is looked at about twice a second (24 MHz ticks) */
   if (now - last_look < 12000000ull)
      return;
   last_look = now;
   if (stat(TRIGGER_FILE, &info) != 0) {
      trigger_time = 1;                 /* a trigger made from now on is new */
      return;
   }
   if (info.st_mtimespec.tv_sec == trigger_time && info.st_mtimespec.tv_nsec == trigger_nsec)
      return;
   if (trigger_time == 0) {
      /* the file was there when the display server started: an old trigger */
      trigger_time = info.st_mtimespec.tv_sec;
      trigger_nsec = info.st_mtimespec.tv_nsec;
      return;
   }
   trigger_time = info.st_mtimespec.tv_sec;
   trigger_nsec = info.st_mtimespec.tv_nsec;
   {
      FILE *f = fopen(TRIGGER_FILE, "r");
      int frames = 0;

      if (f == NULL)
         return;
      want_pixels = 0;
      if (fscanf(f, "%d %d %d %d %d %d", &region[0], &region[1], &region[2], &region[3], &frames,
                 &want_pixels) < 5)
         frames = 0;
      fclose(f);
      if (frames <= 0) {
         pixels_now = want_pixels;
         return;
      }
      if (trace != NULL)
         fclose(trace);
      if (frame_text == NULL)
         frame_text = malloc(FRAME_ROOM);
      trace = fopen(TRACE_FILE, "w");
      if (trace == NULL || frame_text == NULL)
         return;
      frame_used = 0;
      frame_hit = false;
      trace_written = 0;
      trace_frame_number = 0;
      trace_draw_number = 0;
      armed_frames = frames > 50 ? 50 : frames;
      fprintf(trace, "== trace of %d frames with a draw in the rectangle %d,%d-%d,%d\n", armed_frames,
              region[0], region[1], region[2], region[3]);
   }
}

/* Offsets from eagl_slots.txt: index = offset / 8. After ca_fixes_install. */
void
trace_install(void *table_entries[JACKPOTLTE_GL_SLOTS])
{
   memcpy(&next_shader_source, &table_entries[0x254], sizeof(next_shader_source));
   memcpy(&next_link_program, &table_entries[0x258], sizeof(next_link_program));
   memcpy(&next_draw_arrays, &table_entries[0x042], sizeof(next_draw_arrays));
   memcpy(&next_draw_elements, &table_entries[0x044], sizeof(next_draw_elements));
   memcpy(&next_viewport, &table_entries[0x150], sizeof(next_viewport));
   table_entries[0x150] = (void *)my_glViewport;           /* 0x0a80 */
   table_entries[0x254] = (void *)my_glShaderSource;       /* 0x12a0 */
   table_entries[0x258] = (void *)my_glLinkProgram;        /* 0x12c0 */
   table_entries[0x042] = (void *)my_glDrawArrays;         /* 0x0210 */
   table_entries[0x044] = (void *)my_glDrawElements;       /* 0x0220 */
}
