/* SPDX-License-Identifier: MIT */
/*
 * ca_fixes.c: things CoreAnimation's OpenGL ES renderer expects of Apple's
 * driver that Mesa does differently.  The approach is Scion/HakiOS's
 * (userland/src/eagl.c of github.com/cooblg/scion-kernel, note9 branch, MIT):
 *
 *   shaders    CoreAnimation writes GLSL ES 1.00 with Apple's sampler2DRect /
 *              texture2DRect() (a texture read at pixel coordinates).  Each
 *              shader is handed on as a 3.00 one: the 1.00 words are defined
 *              back, texture2DRect() divides by textureSize(), the extension
 *              lines that are core in 3.00 are dropped, lowp/mediump become
 *              highp.  Every shader that does not compile, and every program
 *              that does not link, is logged with the compiler's words.
 *   delete     Apple's table has one entry for glDeleteProgram and
 *              glDeleteShader: the name decides which one is meant.
 *   no program CoreAnimation draws some things (the lock screen wallpaper and
 *              the clock seen through it, the home indicator) with
 *              glUseProgram(0).  Mesa draws nothing then.  Such a draw with a
 *              texture bound is made with a plain program of ours.
 *
 * Switches, read once when backboardd starts (create the file to turn the
 * fix off, no reboot needed, kill backboardd):
 *   /private/var/mobile/Library/a8gl-noglsl    shaders passed on as they are
 *   /private/var/mobile/Library/a8gl-noplain   no-program draws left alone
 */
#include "gl_slots.h"
#include "jackpotlte_gl.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <unistd.h>

#define GL_TEXTURE_2D                           0x0de1
#define GL_TEXTURE0                             0x84c0
#define GL_ACTIVE_TEXTURE                       0x84e0
#define GL_TEXTURE_BINDING_2D                   0x8069
#define GL_TEXTURE_HEIGHT                       0x1001
#define GL_CURRENT_PROGRAM                      0x8b8d
#define GL_FRAGMENT_SHADER                      0x8b30
#define GL_VERTEX_SHADER                        0x8b31
#define GL_SHADER_TYPE                          0x8b4f
#define GL_COMPILE_STATUS                       0x8b81
#define GL_LINK_STATUS                          0x8b82
#define GL_FRAMEBUFFER                          0x8d40
#define GL_FRAMEBUFFER_BINDING                  0x8ca6
#define GL_COLOR_ATTACHMENT0                    0x8ce0
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE   0x8cd0
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME   0x8cd1
#define GL_TEXTURE                              0x1702
#define GL_VERTEX_ATTRIB_ARRAY_ENABLED          0x8622
#define GL_BLEND                                0x0be2

/* the library's own functions (not the table's: those are CoreAnimation's) */
static struct {
   void (*shader_source)(GLuint, GLsizei, const GLchar *const *, const GLint *);
   void (*compile_shader)(GLuint);
   void (*get_shaderiv)(GLuint, GLenum, GLint *);
   void (*get_shader_info_log)(GLuint, GLsizei, GLsizei *, GLchar *);
   void (*link_program)(GLuint);
   void (*get_programiv)(GLuint, GLenum, GLint *);
   void (*get_program_info_log)(GLuint, GLsizei, GLsizei *, GLchar *);
   GLboolean (*is_program)(GLuint);
   void (*delete_program)(GLuint);
   void (*delete_shader)(GLuint);
   GLuint (*create_program)(void);
   GLuint (*create_shader)(GLenum);
   void (*attach_shader)(GLuint, GLuint);
   void (*bind_attrib_location)(GLuint, GLuint, const GLchar *);
   GLint (*get_uniform_location)(GLuint, const GLchar *);
   void (*use_program)(GLuint);
   void (*uniform_matrix4fv)(GLint, GLsizei, GLboolean, const GLfloat *);
   void (*uniform1i)(GLint, GLint);
   void (*uniform1f)(GLint, GLfloat);
   void (*get_integerv)(GLenum, GLint *);
   void (*active_texture)(GLenum);
   void (*get_tex_level_parameteriv)(GLenum, GLint, GLenum, GLint *);
   void (*get_framebuffer_attachment_parameteriv)(GLenum, GLenum, GLenum, GLint *);
   void (*get_vertex_attribiv)(GLuint, GLenum, GLint *);
   void (*get_vertex_attrib_pointerv)(GLuint, GLenum, void **);
   void (*enable_vertex_attrib_array)(GLuint);
   void (*disable_vertex_attrib_array)(GLuint);
   void (*uniform4fv)(GLint, GLsizei, const GLfloat *);
   void (*uniform2fv)(GLint, GLsizei, const GLfloat *);
   GLboolean (*is_enabled)(GLenum);
   void (*enable)(GLenum);
   void (*disable)(GLenum);
   void (*get_uniformfv)(GLuint, GLint, GLfloat *);        /* glca21 */
   GLint (*get_attrib_location)(GLuint, const GLchar *);
   void (*scissor)(GLint, GLint, GLsizei, GLsizei);
   void (*get_attached_shaders)(GLuint, GLsizei, GLsizei *, GLuint *);
   void (*get_shader_source)(GLuint, GLsizei, GLsizei *, GLchar *);
   void (*get_floatv)(GLenum, GLfloat *);                  /* glca22d */
   void (*get_booleanv)(GLenum, GLboolean *);
   void (*clear_color)(GLfloat, GLfloat, GLfloat, GLfloat);
   void (*color_mask)(GLboolean, GLboolean, GLboolean, GLboolean);
   void (*bind_texture)(GLenum, GLuint);                   /* glca23 */
   void (*gen_textures)(GLsizei, GLuint *);
   void (*delete_textures)(GLsizei, const GLuint *);
   void (*tex_parameteri)(GLenum, GLenum, GLint);
   void (*get_tex_parameteriv)(GLenum, GLenum, GLint *);
   void (*get_uniformiv)(GLuint, GLint, GLint *);
   void (*real_flush)(void);
   int loaded;
} gl;

/* the table's entries before ours (they keep gl_slots.c's after-draw submit) */
static void (*next_draw_arrays)(void *, GLenum, GLint, GLsizei);
static void (*next_draw_elements)(void *, GLenum, GLsizei, GLenum, const void *);
static void (*next_uniform_matrix4fv)(void *, GLint, GLsizei, GLboolean, const GLfloat *);

static int glsl_on = 1;
static int plain_on = 1;
static int debug_shape;         /* a8gl-dbg-shape "1": shape shaders show (coverage, 20 w, d / 2) */

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

/* long text (a shader) in pieces the log takes */
static void
say_long(const char *text)
{
   size_t length = strlen(text);

   for (size_t at = 0; at < length; at += 400)
      say("  | %.400s", text + at);
}

#define PROC(field, name) gl.field = (__typeof__(gl.field))jackpotlte_gl_proc(name)

static void
load(void)
{
   if (gl.loaded)
      return;
   gl.loaded = 1;
   PROC(shader_source, "glShaderSource");
   PROC(compile_shader, "glCompileShader");
   PROC(get_shaderiv, "glGetShaderiv");
   PROC(get_shader_info_log, "glGetShaderInfoLog");
   PROC(link_program, "glLinkProgram");
   PROC(get_programiv, "glGetProgramiv");
   PROC(get_program_info_log, "glGetProgramInfoLog");
   PROC(is_program, "glIsProgram");
   PROC(delete_program, "glDeleteProgram");
   PROC(delete_shader, "glDeleteShader");
   PROC(create_program, "glCreateProgram");
   PROC(create_shader, "glCreateShader");
   PROC(attach_shader, "glAttachShader");
   PROC(bind_attrib_location, "glBindAttribLocation");
   PROC(get_uniform_location, "glGetUniformLocation");
   PROC(use_program, "glUseProgram");
   PROC(uniform_matrix4fv, "glUniformMatrix4fv");
   PROC(uniform1i, "glUniform1i");
   PROC(uniform1f, "glUniform1f");
   PROC(get_integerv, "glGetIntegerv");
   PROC(active_texture, "glActiveTexture");
   PROC(get_tex_level_parameteriv, "glGetTexLevelParameteriv");
   PROC(get_framebuffer_attachment_parameteriv, "glGetFramebufferAttachmentParameteriv");
   PROC(get_vertex_attribiv, "glGetVertexAttribiv");
   PROC(get_vertex_attrib_pointerv, "glGetVertexAttribPointerv");
   PROC(enable_vertex_attrib_array, "glEnableVertexAttribArray");
   PROC(disable_vertex_attrib_array, "glDisableVertexAttribArray");
   PROC(uniform4fv, "glUniform4fv");
   PROC(uniform2fv, "glUniform2fv");
   PROC(is_enabled, "glIsEnabled");
   PROC(enable, "glEnable");
   PROC(disable, "glDisable");
   PROC(get_uniformfv, "glGetUniformfv");
   PROC(get_attrib_location, "glGetAttribLocation");
   PROC(scissor, "glScissor");
   PROC(get_attached_shaders, "glGetAttachedShaders");
   PROC(get_shader_source, "glGetShaderSource");
   PROC(get_floatv, "glGetFloatv");
   PROC(get_booleanv, "glGetBooleanv");
   PROC(clear_color, "glClearColor");
   PROC(color_mask, "glColorMask");
   PROC(bind_texture, "glBindTexture");
   PROC(gen_textures, "glGenTextures");
   PROC(delete_textures, "glDeleteTextures");
   PROC(tex_parameteri, "glTexParameteri");
   PROC(get_tex_parameteriv, "glGetTexParameteriv");
   PROC(get_uniformiv, "glGetUniformiv");
   PROC(real_flush, "glFlush");
}

/* ---- shaders ------------------------------------------------------------ */

static const char all_highp[] =
   "#define lowp highp\n"
   "#define mediump highp\n";

/*
 * Rounded corners without derivatives.  On this GPU (Mesa 25.0.7 panfrost,
 * Bifrost v6, Mali-G71) dFdx/dFdy/fwidth come out wrong: measured on the dock's
 * corner, fwidth(d) was about 3.7 where 0.05 is right, and a sign-keeping
 * dFdx of the texture coordinates was as wrong the other way.  CoreAnimation
 * draws every rounded shape with coverage = (w/2 + 1 - d) / w, w = fwidth(d)
 * (supercircleImage, circleImage, squareImage), so its rounded rectangles
 * came out square with a soft smudge (Control Center, the dock, the island).
 *
 * Here the size of a pixel in the shape's coordinates is worked out without the
 * GPU's derivatives: the vertex shader hands on the vertex's place (CA's
 * vertex_position is in pixels of the target) both interpolated and flat (the
 * provoking vertex's), with the flat texture coordinates; a fragment divides
 * how far its texture coordinates are from the provoking vertex's by how far
 * it is from it in pixels.  Each shape's fwidth() becomes the derivative of
 * its own d by that size.  Switch: a8gl-shapes "0".
 */
static const char shape_head[] =
   "in highp vec2 a8_px;\n"
   "flat in highp vec2 a8_fpx;\n"
   "flat in highp vec4 a8_fuv;\n"
   "highp float a8_s0 = 0.0166667, a8_s1 = 0.0166667;\n"
   "highp float a8_pick(highp vec2 uv);\n"
   "highp vec4 a8dbg = vec4(0., 0., 1., 1.);\n";

static const char shape_vertex_head[] =
   "out highp vec2 a8_px;\n"
   "flat out highp vec2 a8_fpx;\n"
   "flat out highp vec4 a8_fuv;\n"
   "#define main a8_vmain\n"
   "#line 1\n";

static const struct {
   const char *header;           /* the function's first line, as CA writes it */
   const char *line;             /* its fwidth() line */
   const char *with;             /* that line without derivatives */
} shapes[] = {
   { "lowp vec4 supercircleImage(highp vec2 uv) {", "w = max(fwidth(d), 1e-4);",
     "w = max(2.68 * 1.199 * a8_sc * (pow(uv.x, 1.68) + pow(uv.y, 1.68)), 1e-4);" },
   { "lowp vec4 circleImage(mediump vec2 uv) {", "w = max(fwidth(d), 1e-4);",
     "w = max(a8_sc * (abs(uv.x) + abs(uv.y)) / max(d, 1e-4), 1e-4);" },
   { "lowp vec4 squareImage(mediump vec2 uv) {", "w = max(fwidth(uv), 1e-4);",
     "w = max(vec2(a8_sc), 1e-4);" },
};
#define SHAPES (int)(sizeof(shapes) / sizeof(shapes[0]))
#define PICK " highp float a8_sc = a8_pick(uv);"

static int shapes_on = 1;

/* the text with each shape's fwidth() replaced, or NULL when it has none */
static char *
rewrite_shapes(const char *text)
{
   size_t room = strlen(text) + 1;
   char *out;
   bool any = false;

   for (int i = 0; i < SHAPES; i++)
      room += strlen(PICK) + strlen(shapes[i].with);
   out = malloc(room);
   if (out == NULL)
      return NULL;
   strcpy(out, text);
   for (int i = 0; i < SHAPES; i++) {
      char *head = strstr(out, shapes[i].header), *line;
      size_t tail;

      if (head == NULL)
         continue;
      head += strlen(shapes[i].header);
      line = strstr(head, shapes[i].line);
      if (line == NULL)
         continue;
      /* the fwidth line first (it is after the head), then the pick */
      tail = strlen(line + strlen(shapes[i].line));
      memmove(line + strlen(shapes[i].with), line + strlen(shapes[i].line), tail + 1);
      memcpy(line, shapes[i].with, strlen(shapes[i].with));
      memmove(head + strlen(PICK), head, strlen(head) + 1);
      memcpy(head, PICK, strlen(PICK));
      any = true;
   }
   if (!any) {
      free(out);
      return NULL;
   }
   return out;
}

/* what follows a shape shader: the scales, then main() as usual */
static void
shape_end(char *end, size_t room, const char *text, bool fetch, bool debug)
{
   bool has0 = strstr(text, "texcoord0_0") != NULL, has1 = strstr(text, "texcoord1_0") != NULL;

   snprintf(end, room,
            "\n#undef main\n"
            "highp float a8_pick(highp vec2 uv) { return %s; }\n"
            "void a8_scales() {\n"
            "  highp float far = length(a8_px - a8_fpx);\n"
            "  if (far < 1.5) return;\n"
            "%s%s"
            "}\n"
            "void main() { a8_scales(); %sa8_main(); a8_colour = %s; }\n",
            has1 && has0 ? "uv == texcoord1_0 ? a8_s1 : a8_s0" : has1 ? "a8_s1" : "a8_s0",
            has0 ? "  a8_s0 = length(texcoord0_0 - a8_fuv.xy) / far;\n" : "",
            has1 ? "  a8_s1 = length(texcoord1_0 - a8_fuv.zw) / far;\n" : "",
            fetch ? "a8_last[0] = a8_colour; " : "", debug ? "a8dbg" : "a8_data[0]");
}

static const char fragment_start[] =
   "#define varying in\n"
   "#define texture2D texture\n"
   "#define texture2DProj textureProj\n"
   "#define texture2DLod textureLod\n"
   "#define textureCube texture\n"
   "#define sampler2DRect sampler2D\n"
   "#define texture2DRect(s, c) texture(s, (c) / vec2(textureSize(s, 0)))\n"
   "highp vec4 a8_data[1];\n"
   "#define gl_FragData a8_data\n"
   "#define gl_FragColor a8_data[0]\n";

/* main() is renamed and called from one that copies the colour out (and,
 * with framebuffer fetch, what is there already in) */
static const char fragment_plain[] =
   "layout(location = 0) out highp vec4 a8_colour;\n"
   "#define main a8_main\n"
   "#line 1\n";
static const char fragment_plain_end[] =
   "\n#undef main\n"
   "void main() { a8_main(); a8_colour = a8_data[0]; }\n";
static const char fragment_fetch[] =
   "layout(location = 0) inout highp vec4 a8_colour;\n"
   "highp vec4 a8_last[1];\n"
   "#define gl_LastFragData a8_last\n"
   "#define main a8_main\n"
   "#line 1\n";
static const char fragment_fetch_end[] =
   "\n#undef main\n"
   "void main() { a8_last[0] = a8_colour; a8_main(); a8_colour = a8_data[0]; }\n";

static const char vertex_start[] =
   "#define attribute in\n"
   "#define varying out\n"
   "#define texture2D texture\n"
   "#define texture2DLod textureLod\n"
   "#line 1\n";

/* 1.00 extensions that are core in 3.00: the line goes, these defines come */
static const struct {
   const char *name;
   const char *defines;
} core_in_300[] = {
   { "GL_OES_standard_derivatives", "" },
   { "GL_EXT_shader_texture_lod",
     "#define texture2DLodEXT textureLod\n#define texture2DProjLodEXT textureProjLod\n"
     "#define textureCubeLodEXT textureLod\n#define texture2DGradEXT textureGrad\n"
     "#define texture2DProjGradEXT textureProjGrad\n#define textureCubeGradEXT textureGrad\n" },
   { "GL_EXT_frag_depth", "#define gl_FragDepthEXT gl_FragDepth\n" },
   { "GL_EXT_draw_buffers", "" },
};
#define CORE_COUNT (int)(sizeof(core_in_300) / sizeof(core_in_300[0]))
#define CORE_DEFINES_ROOM 400

/* the core_in_300 entry the "#extension" line names, or -1 */
static int
core_of(const char *line)
{
   line += strlen("#extension");
   line += strspn(line, " \t");
   for (int i = 0; i < CORE_COUNT; i++) {
      size_t n = strlen(core_in_300[i].name);

      if (strncmp(line, core_in_300[i].name, n) == 0 && strchr(" \t:", line[n]) != NULL)
         return i;
   }
   return -1;
}

/* the text each shader was given, for the log when it fails */
#define KEPT 256
static char *kept[KEPT];

static void
keep_text(GLuint shader, char *text)
{
   free(kept[shader % KEPT]);
   kept[shader % KEPT] = text;
}

static void
my_glShaderSource(void *context, GLuint shader, GLsizei count, const GLchar *const *strings,
                  const GLint *lengths)
{
   GLint type = 0;
   size_t size = 1;
   unsigned done = 0;
   char *text, *head, *line;
   bool fragment, fetch;
   const GLchar *parts[6];
   char *debug_text, *shape_text, *shape_start = NULL;
   char shape_tail[768];

   jackpotlte_gl_use(context);
   load();
   if (gl.shader_source == NULL)
      return;
   for (GLsizei i = 0; i < count; i++)
      size += lengths != NULL && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);
   text = calloc(1, 2 * size + 64 + CORE_DEFINES_ROOM);
   if (text == NULL) {
      gl.shader_source(shader, count, strings, lengths);
      return;
   }
   for (GLsizei i = 0; i < count; i++)
      strncat(text, strings[i], lengths != NULL && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]));

   if (!glsl_on || strstr(text, "#version") != NULL) {
      /* switched off, or already a versioned shader: as it is */
      const GLchar *one = text;

      gl.shader_source(shader, 1, &one, NULL);
      keep_text(shader, text);
      return;
   }

   if (gl.get_shaderiv != NULL)
      gl.get_shaderiv(shader, GL_SHADER_TYPE, &type);
   fragment = type == GL_FRAGMENT_SHADER;

   /* "#extension" lines go first; the core ones become their defines */
   head = text + size;
   strcpy(head, "#version 300 es\n");
   for (line = text; (line = strstr(line, "#extension")) != NULL; line += 2) {
      size_t length = strcspn(line, "\n");
      int core = core_of(line);

      if (core < 0) {
         strncat(head, line, length);
         strcat(head, "\n");
      } else if (!(done & (1u << core))) {
         done |= 1u << core;
         strcat(head, core_in_300[core].defines);
      }
      line[0] = '/';
      line[1] = '/';
   }

   fetch = fragment && strstr(text, "gl_LastFragData") != NULL;
   parts[0] = head;
   parts[1] = all_highp;
   parts[2] = fragment ? fragment_start : vertex_start;
   parts[3] = fragment ? (fetch ? fragment_fetch : fragment_plain) : "";
   parts[4] = text;
   parts[5] = fragment ? (fetch ? fragment_fetch_end : fragment_plain_end) : "";

   /* shapes: their fwidth() without derivatives (see shape_head) */
   shape_text = NULL;
   debug_text = NULL;
   if (shapes_on && fragment && (shape_text = rewrite_shapes(text)) != NULL) {
      const char *body = shape_text;
      size_t n = strlen(fetch ? fragment_fetch : fragment_plain) + sizeof(shape_head);

      /* a8gl-dbg-shape "1": a shape shows (coverage, 20 w, d / 2) instead */
      if (debug_shape == 1 && !fetch) {
         static const char from[] = "mediump float l = clamp((w * .5 + (1. - d)) / w, 0., 1.);";
         static const char to[] = "mediump float l = clamp((w * .5 + (1. - d)) / w, 0., 1.); a8dbg = vec4(l, w * 20., d * .5, 1.);";
         char *at = strstr(shape_text, from);

         if (at != NULL && (debug_text = malloc(strlen(shape_text) + sizeof(to))) != NULL) {
            size_t before = (size_t)(at - shape_text);

            memcpy(debug_text, shape_text, before);
            memcpy(debug_text + before, to, sizeof(to) - 1);
            strcpy(debug_text + before + sizeof(to) - 1, at + sizeof(from) - 1);
            body = debug_text;
         }
      }
      shape_start = malloc(n);
      if (shape_start != NULL) {
         strcpy(shape_start, shape_head);
         strcat(shape_start, fetch ? fragment_fetch : fragment_plain);
         shape_end(shape_tail, sizeof(shape_tail), text, fetch, debug_text != NULL);
         parts[3] = shape_start;
         parts[4] = body;
         parts[5] = shape_tail;
      }
   } else if (shapes_on && !fragment && strstr(text, "texcoord0_0 =") != NULL &&
              strstr(text, "vertex_position") != NULL) {
      /* CoreAnimation's vertex shader: also the places a shape shader needs */
      parts[3] = shape_vertex_head;
      snprintf(shape_tail, sizeof(shape_tail),
               "\n#undef main\nvoid main() { a8_vmain(); a8_px = vertex_position.xy / vertex_position.w; "
               "a8_fpx = a8_px; a8_fuv = vec4(texcoord0_0, %s); }\n",
               strstr(text, "texcoord1_0 =") != NULL ? "texcoord1_0" : "vec2(0.)");
      parts[5] = shape_tail;
   }
   gl.shader_source(shader, 6, parts, NULL);
   free(debug_text);
   free(shape_text);
   free(shape_start);
   keep_text(shader, text);
}

static void
my_glCompileShader(void *context, GLuint shader)
{
   static unsigned compiled, failed;
   GLint good = 1;
   char words[1024] = "";

   jackpotlte_gl_use(context);
   load();
   if (gl.compile_shader == NULL)
      return;
   gl.compile_shader(shader);
   compiled++;
   if (gl.get_shaderiv != NULL)
      gl.get_shaderiv(shader, GL_COMPILE_STATUS, &good);
   if (good) {
      if (compiled == 1 || compiled % 50 == 0)
         say("shaders: %u compiled, %u failed (glsl rewrite %s)", compiled, failed, glsl_on ? "on" : "off");
      return;
   }
   failed++;
   if (gl.get_shader_info_log != NULL)
      gl.get_shader_info_log(shader, sizeof(words), NULL, words);
   say("A SHADER DOES NOT COMPILE (shader %u, failure %u): %s", shader, failed, words);
   if (failed <= 6 && kept[shader % KEPT] != NULL)
      say_long(kept[shader % KEPT]);
}

static void
my_glLinkProgram(void *context, GLuint program)
{
   static unsigned failed;
   GLint good = 1;
   char words[1024] = "";

   jackpotlte_gl_use(context);
   load();
   if (gl.link_program == NULL)
      return;
   gl.link_program(program);
   if (gl.get_programiv != NULL)
      gl.get_programiv(program, GL_LINK_STATUS, &good);
   if (good)
      return;
   failed++;
   if (gl.get_program_info_log != NULL)
      gl.get_program_info_log(program, sizeof(words), NULL, words);
   say("A PROGRAM DOES NOT LINK (program %u, failure %u): %s", program, failed, words);
}

/* Apple's one entry for both: the name says which */
static void
my_glDeleteProgram(void *context, GLuint name)
{
   jackpotlte_gl_use(context);
   load();
   if (gl.is_program != NULL && gl.is_program(name)) {
      if (gl.delete_program != NULL)
         gl.delete_program(name);
   } else if (gl.delete_shader != NULL) {
      gl.delete_shader(name);
   }
}

/* ---- draws with no program ---------------------------------------------- */

static const char plain_vertex[] =
   "#version 300 es\n"
   "uniform highp mat4 vertex_matrix;\n"
   "in highp vec4 vertex_position;\n"
   "in lowp vec4 vertex_color;\n"
   "in highp vec2 vertex_texcoord0;\n"
   "out lowp vec4 color;\n"
   "out highp vec2 texcoord;\n"
   "void main() {\n"
   "  gl_Position = vertex_matrix * vertex_position;\n"
   "  color = vertex_color;\n"
   "  texcoord = vertex_texcoord0;\n"
   "}\n";

/* colour times the texture (coordinates in its pixels) */
static const char plain_fragment[] =
   "#version 300 es\n"
   "uniform lowp sampler2D texture0;\n"
   "in lowp vec4 color;\n"
   "in highp vec2 texcoord;\n"
   "out lowp vec4 result;\n"
   "void main() {\n"
   "  result = color * texture(texture0, texcoord / vec2(textureSize(texture0, 0)));\n"
   "}\n";

/* the home indicator: what is behind it looked up in a one-row table on unit 1
 * (Scion's arithmetic for CoreAnimation's homeAffordanceBase filter) */
static const char table_fragment[] =
   "#version 300 es\n"
   "uniform lowp sampler2D texture0;\n"
   "uniform lowp sampler2D table;\n"
   "in lowp vec4 color;\n"
   "in highp vec2 texcoord;\n"
   "out lowp vec4 result;\n"
   "const mediump float white = .284, amount = 1., overlay = .31;\n"
   "void main() {\n"
   "  mediump vec4 behind = texture(texture0, texcoord / vec2(textureSize(texture0, 0)));\n"
   "  mediump vec4 entry = texture(table, vec2(dot(behind.rgb, vec3(.2125, .7154, .0721)), .5));\n"
   "  mediump vec4 bar = clamp(entry + vec4(vec3(white), 1.), 0., 1.) * amount;\n"
   "  mediump vec4 under = behind * (1. - bar.a) + bar;\n"
   "  mediump vec4 over = under * overlay;\n"
   "  mediump vec3 dark = 2. * under.rgb * over.rgb;\n"
   "  mediump vec3 light = over.a * under.a - 2. * (under.a - under.rgb) * (over.a - over.rgb);\n"
   "  mediump vec3 both = mix(dark, light, step(under.a * .5, under.rgb));\n"
   "  mediump float opacity = over.a + under.a * (1. - over.a);\n"
   "  result = vec4(clamp(both + over.rgb * (1. - under.a) + under.rgb * (1. - over.a), 0., opacity), opacity);\n"
   "}\n";


/*
 * Programs for CoreAnimation "functions" its OpenGL ES renderer has no shader
 * for (Scion's eagl.c, MIT). Which function a draw is comes from CA's
 * GLContext (hooks below): the byte at *(*(context + 0x10) + 0x10).
 *
 *   vcm (45)  vibrantColorMatrix: a colour matrix over what is behind, by the
 *             layer's alpha (the lock screen's clock and its dimming
 *             gradient). Reads the framebuffer (EXT_shader_framebuffer_fetch).
 *   c4p (20)  colorMatrix over premultiplied pixels (masks of the clock digits).
 *   sb3 (40)  four samples around a point (shadow blur).
 *   ds8 (28)  eight times smaller in one go (first step of backdrop blur).
 */
static const char vibrant_fragment[] =
   "#version 300 es\n"
   "#extension GL_EXT_shader_framebuffer_fetch : require\n"
   "uniform lowp sampler2D texture0;\n"
   "uniform mediump vec4 colours[5];\n"
   "in lowp vec4 color;\n"
   "in highp vec2 texcoord;\n"
   "layout(location = 0) inout highp vec4 result;\n"
   "void main() {\n"
   "  mediump vec4 layer = color * texture(texture0, texcoord / vec2(textureSize(texture0, 0)));\n"
   "  mediump vec4 behind = result, s = behind;\n"
   "  s.rgb = s.rgb / max(s.a, .005);\n"
   "  s = s.r * colours[0] + s.g * colours[1] + s.b * colours[2] + s.a * colours[3] + colours[4];\n"
   "  s.a = clamp(s.a, 0., 1.);\n"
   "  s.rgb = clamp(s.rgb * s.a, 0., 1.);\n"
   "  result = mix(behind, s, layer.a);\n"
   "}\n";

static const char masking_fragment[] =
   "#version 300 es\n"
   "uniform lowp sampler2D texture0;\n"
   "uniform mediump vec4 colours[5];\n"
   "in lowp vec4 color;\n"
   "in highp vec2 texcoord;\n"
   "out lowp vec4 result;\n"
   "void main() {\n"
   "  mediump vec4 s = color * texture(texture0, texcoord / vec2(textureSize(texture0, 0)));\n"
   "  result = clamp(s.r * colours[0] + s.g * colours[1] + s.b * colours[2] + s.a * colours[3] + colours[4], 0., 1.);\n"
   "}\n";

static const char blur4_fragment[] =
   "#version 300 es\n"
   "uniform lowp sampler2D texture0;\n"
   "uniform highp vec2 places[4];\n"
   "in lowp vec4 color;\n"
   "in highp vec2 texcoord;\n"
   "out lowp vec4 result;\n"
   "void main() {\n"
   "  highp vec2 middle = texcoord / vec2(textureSize(texture0, 0));\n"
   "  result = .25 * (texture(texture0, middle + places[0]) + texture(texture0, middle + places[1]) +\n"
   "      texture(texture0, middle + places[2]) + texture(texture0, middle + places[3]));\n"
   "}\n";

/*
 * glca4: "ds8" (28) as this renderer uses it shrinks by 32, not 8: the screen
 * (1080x2220, its coordinates 0..1088 x 0..2240) into 34x70 for Control
 * Center's backdrop, 14x14 out of 448x448 for the dock's; and the blur that
 * should follow ("sb3", 40) is a function the OpenGL ES renderer has no shader
 * for, so nothing smooths the result before it is stretched back 32 times.
 * Sixteen samples of a 1024-pixel block made it blocky and made it shimmer.
 * Here each output pixel is a Gaussian average over 3 x 3 output pixels' worth
 * of the source (sigma 0.75 of one), 16 x 16 bilinear samples: the small
 * picture is already blurred, and stretched it is a smooth frosted backdrop.
 * `reach`: source pixels per output pixel, from the draw's own vertices.
 */
static const char wide_fragment[] =
   "#version 300 es\n"
   "uniform highp sampler2D texture0;\n"
   "uniform highp float reach;\n"
   "in lowp vec4 color;\n"
   "in highp vec2 texcoord;\n"
   "out highp vec4 result;\n"
   "void main() {\n"
   "  highp vec2 size = vec2(textureSize(texture0, 0));\n"
   "  highp float step = reach * 3. / 16., sigma = reach * .75;\n"
   "  highp vec4 sum = vec4(0.);\n"
   "  highp float weights = 0.;\n"
   "  for (int j = 0; j < 16; j++) {\n"
   "    for (int i = 0; i < 16; i++) {\n"
   "      highp vec2 at = (vec2(float(i), float(j)) - 7.5) * step;\n"
   "      highp float w = exp(-dot(at, at) / (2. * sigma * sigma));\n"
   "      sum += w * texture(texture0, (texcoord + at) / size);\n"
   "      weights += w;\n"
   "    }\n"
   "  }\n"
   "  result = sum / weights;\n"
   "}\n";

/*
 * glca22: "vrb" (39), CoreAnimation's variable blur (CAFilter variableBlur):
 * the backdrop under the expanded Dynamic Island, Spotlight's top. Its draw
 * reads the backdrop (unit 0, source pixels in texcoord0) and a one-column
 * mask (unit 1, e.g. 1x2381, coordinates in vertex_texcoord1, attribute 3);
 * the result is put on the screen with blending off, so with no shader for
 * it (nothing drawn) the screen got the target's empty pixels: a black block
 * the width of the screen and ~610 px tall (user report 11-25). The two "vrd"
 * (38) passes before it read the texture they write and are still left out,
 * so the source is the unblurred backdrop. Here each pixel averages 17
 * samples on a golden-angle spiral of radius `radius` x the mask's alpha.
 * The four numbers CA gives are logged ("glca22: function 39 values") but not
 * used yet. Switch a8gl-vblur: "0" off (old behaviour), "N" radius N pixels
 * (default 18).
 */
static const char vblur_vertex[] =
   "#version 300 es\n"
   "uniform highp mat4 vertex_matrix;\n"
   "in highp vec4 vertex_position;\n"
   "in lowp vec4 vertex_color;\n"
   "in highp vec2 vertex_texcoord0;\n"
   "in highp vec2 vertex_texcoord1;\n"
   "out lowp vec4 color;\n"
   "out highp vec2 texcoord;\n"
   "out highp vec2 texcoord1;\n"
   "void main() {\n"
   "  gl_Position = vertex_matrix * vertex_position;\n"
   "  color = vertex_color;\n"
   "  texcoord = vertex_texcoord0;\n"
   "  texcoord1 = vertex_texcoord1;\n"
   "}\n";

static const char vblur_fragment[] =
   "#version 300 es\n"
   "uniform highp sampler2D texture0;\n"
   "uniform highp sampler2D mask;\n"
   "uniform highp float radius;\n"
   "in lowp vec4 color;\n"
   "in highp vec2 texcoord;\n"
   "in highp vec2 texcoord1;\n"
   "out highp vec4 result;\n"
   "void main() {\n"
   "  highp vec2 size = vec2(textureSize(texture0, 0));\n"
   "  highp vec2 at = texcoord / size;\n"
   "  highp float r = radius * clamp(texture(mask, texcoord1).a, 0., 1.);\n"
   "  highp vec4 sum = texture(texture0, at);\n"
   "  if (r >= .75) {\n"
   "    for (int i = 0; i < 16; i++) {\n"
   "      highp float k = float(i) + .5;\n"
   "      highp vec2 o = vec2(cos(k * 2.39996323), sin(k * 2.39996323)) * (sqrt(k / 16.) * r);\n"
   "      sum += texture(texture0, at + o / size);\n"
   "    }\n"
   "    sum *= 1. / 17.;\n"
   "  }\n"
   "  result = color * sum;\n"
   "}\n";

/*
 * glca22: "alt" (59), taken to be an alpha threshold (2 numbers: the
 * threshold, then a width or 0). Off unless the switch a8gl-alt holds "1":
 * where CA uses it is not known yet (its values are logged).
 */
static const char alt_fragment[] =
   "#version 300 es\n"
   "uniform lowp sampler2D texture0;\n"
   "uniform highp vec4 values;\n"
   "in lowp vec4 color;\n"
   "in highp vec2 texcoord;\n"
   "out lowp vec4 result;\n"
   "void main() {\n"
   "  mediump vec4 s = color * texture(texture0, texcoord / vec2(textureSize(texture0, 0)));\n"
   "  highp float w = (values.y > 0. && values.y < .5) ? max(values.y, .004) : .02;\n"
   "  highp float a = smoothstep(values.x - w, values.x + w, s.a);\n"
   "  result = vec4(s.rgb / max(s.a, .004) * a, a);\n"
   "}\n";

static const char box_fragment[] =
   "#version 300 es\n"
   "uniform lowp sampler2D texture0;\n"
   "uniform highp vec2 places[4];\n"
   "in lowp vec4 color;\n"
   "in highp vec2 texcoord;\n"
   "out lowp vec4 result;\n"
   "void main() {\n"
   "  highp vec2 middle = texcoord / vec2(textureSize(texture0, 0));\n"
   "  highp vec4 sum = vec4(0.);\n"
   "  for (int i = 0; i < 4; i++) {\n"
   "    sum += texture(texture0, middle + places[i]) + texture(texture0, middle - places[i]);\n"
   "    sum += texture(texture0, middle + places[i] * vec2(1., -1.)) + texture(texture0, middle + places[i] * vec2(-1., 1.));\n"
   "  }\n"
   "  result = sum * .0625;\n"
   "}\n";

#define FUNCTION_MATRIX_PREMULTIPLIED   20
#define FUNCTION_DOWNSAMPLE_8           28
#define FUNCTION_SHADOW_BLUR            40
#define FUNCTION_VIBRANT_MATRIX         45
#define FUNCTION_HOME_AFFORDANCE        53
#define FUNCTION_VARIABLE_BLUR          39   /* glca22 "vrb" */
#define FUNCTION_ALPHA_THRESHOLD        59   /* glca22 "alt" */
#define FUNCTION_CUSTOM                 89
#define FUNCTIONS                       90

static const char function_names[] =
   "clr img ida cim cid mua m1a lrp lrc lra add spl s1a cma bds bco bim c33 c3b c4b c4p c3a c3x c4x bia grl ds4 ds6 "
   "ds8 ups db2 db4 nb7 n11 n15 n19 n23 n27 vrd vrb sb3 axl rad rac con vcm vdk vlt map mpc crv hue clm hom opa met "
   "srl ega sdr alt p3c edr hrm lut ovr ovl lub aup alm stl pc0 pc1 prc psl ps0 ps1 psm psr pcr pbl peo bib asg a75 "
   "a50 atc ipc apl 3dl cst ";

static void *ca_context;                       /* CA's GLContext, from the hooks */
static GLfloat function_values[FUNCTIONS][20];
static uint8_t function_given[FUNCTIONS];

static unsigned
current_function(void)
{
   if (ca_context == NULL)
      return FUNCTIONS;                         /* not known */
   return *(*(uint8_t **)((char *)ca_context + 0x10) + 0x10);
}

/* trace.c's view of them */
unsigned ca_fixes_function(void);
const float *ca_fixes_function_values(unsigned function, unsigned *count);
const float *ca_fixes_matrix(void);

unsigned
ca_fixes_function(void)
{
   return current_function();
}

/* glca22: the numbers CA last gave a function (trace.c prints them) */
const float *
ca_fixes_function_values(unsigned function, unsigned *count)
{
   if (function >= FUNCTIONS) {
      *count = 0;
      return NULL;
   }
   *count = function_given[function];
   return function_values[function];
}

/* ---- hooks into CoreAnimation's GLES context (QuartzCore 20H392) -------- */

#define VT_GLES_CONTEXT         0x1d5b2f6e8UL   /* vtable for CA::OGL::GLESContext */
#define VSLOT(offset)           (VT_GLES_CONTEXT + 0x10 + (offset))
#define FUNCTION_SUPPORTED      0x1884b2164UL   /* its entry at 0x378 */
#define FUNCTION_UNIFORM        0x1884b21acUL   /* its entry at 0x388 */

static int (*orig_function_supported)(void *context);
static void (*orig_function_uniform)(void *context, unsigned index, size_t components, size_t count,
                                     const float *values);

static int
my_function_supported(void *context)
{
   static uint8_t said[256];
   int supported = orig_function_supported(context);
   unsigned function = *(*(uint8_t **)((char *)context + 0x10) + 0x10);

   ca_context = context;
   if (!supported && !said[function & 0xff]) {
      said[function & 0xff] = 1;
      say("CA has no shader for function %u (\"%.3s\"): what needs it is not drawn", function,
          function < FUNCTIONS ? function_names + 4 * function : "?");
   }
   return supported;
}

static void
my_function_uniform(void *context, unsigned index, size_t components, size_t count, const float *values)
{
   static uint8_t said[FUNCTIONS];
   unsigned function = *(*(uint8_t **)((char *)context + 0x10) + 0x10);

   ca_context = context;
   if (function < FUNCTIONS && index == 0 && components * count <= 20) {
      memcpy(function_values[function], values, components * count * sizeof(values[0]));
      function_given[function] = (uint8_t)(components * count);
      if (!said[function]) {
         said[function] = 1;
         say("CA gives function %u (\"%.3s\") %zu numbers", function, function_names + 4 * function,
             components * count);
      }
      {
         /* glca22: the numbers themselves, a few times, for the functions
          * whose stand-ins are being worked out */
         static uint8_t told[FUNCTIONS];

         if ((function == 38 || function == FUNCTION_VARIABLE_BLUR || function == FUNCTION_ALPHA_THRESHOLD ||
              function == 61) && told[function] < 4) {
            char line[200];
            int at = 0;

            told[function]++;
            line[0] = '\0';
            for (size_t i = 0; i < components * count && at < (int)sizeof(line) - 16; i++)
               at += snprintf(line + at, sizeof(line) - (size_t)at, " %g", values[i]);
            say("glca22: function %u (\"%.3s\") values:%s", function, function_names + 4 * function, line);
         }
      }
   }
   orig_function_uniform(context, index, components, count, values);
}

static intptr_t
quartzcore_slide(void)
{
   for (uint32_t i = 0; i < _dyld_image_count(); i++) {
      const char *name = _dyld_get_image_name(i);

      if (name != NULL && strstr(name, "/QuartzCore.framework/QuartzCore") != NULL)
         return _dyld_get_image_vmaddr_slide(i);
   }
   return -1;
}

/* one vtable entry (unslid `slot`) that must hold `expected`: replaced */
static uintptr_t
replace_virtual(const char *what, intptr_t slide, uintptr_t slot, uintptr_t expected, uintptr_t replacement)
{
   uintptr_t *entry = (uintptr_t *)(slot + slide);
   vm_address_t page = (vm_address_t)entry & ~(vm_address_t)(vm_page_size - 1);
   uintptr_t original;

   if (*entry != expected + slide) {
      say("hook %s: found %#lx, expected %#lx; left alone", what, (unsigned long)(*entry - slide),
          (unsigned long)expected);
      return 0;
   }
   if (vm_protect(mach_task_self(), page, vm_page_size, FALSE,
                  VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
      say("hook %s: the table cannot be made writable", what);
      return 0;
   }
   original = *entry;
   *entry = replacement;
   vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
   return original;
}

static void
hook_coreanimation(void)
{
   intptr_t slide = quartzcore_slide();

   if (slide == -1) {
      say("hooks: QuartzCore not found");
      return;
   }
   orig_function_supported = (int (*)(void *))replace_virtual("function_supported", slide,
         VSLOT(0x378), FUNCTION_SUPPORTED, (uintptr_t)my_function_supported);
   orig_function_uniform = (void (*)(void *, unsigned, size_t, size_t, const float *))replace_virtual(
         "function_uniform", slide, VSLOT(0x388), FUNCTION_UNIFORM, (uintptr_t)my_function_uniform);
   say("hooks: function_supported %s, function_uniform %s", orig_function_supported ? "on" : "OFF",
       orig_function_uniform ? "on" : "OFF");
}

static const GLfloat identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

/* the matrix CoreAnimation last gave a program of its own (always from the
 * same place of its, so the pointer stays good) */
static const GLfloat *ca_matrix;

const float *
ca_fixes_matrix(void)
{
   return ca_matrix;
}

struct our_program {
   GLuint program;
   GLint matrix;
   GLint numbers;      /* "colours" or "places", -1 if none */
   int tried;
};
static struct our_program plain, table, vibrant, masking, blur4, box, wide, vblur, athresh;
static GLint vblur_radius = -1;          /* glca22: its "radius" uniform */
static int vblur_px = 18;                /* a8gl-vblur: 0 off, else the radius in pixels */
static int alt_on;                       /* a8gl-alt "1": the alpha threshold stand-in */
/*
 * glca22b: a draw with no program that we do not draw is not handed on
 * either. Handed on, Mesa still wrote into the target: the variable blur's
 * two "vrd" passes (which read the texture they write) turned the backdrop
 * copy to 0,0,0,0 (trace pixels 11-25: 1087c7ff before, 00000000 after), and
 * that is what the Dynamic Island's black block was made of. Switch
 * a8gl-skipnp "0": hand them on as before.
 */
static int skipnp_on = 1;
static bool plain_skip;                  /* plain_begin: leave this draw out */
static unsigned long skipped_np;         /* how many were left out */

/*
 * glca22d: a clear that Apple's tile-based GPU never writes. Before each of
 * the variable blur's "vrd" passes CoreAnimation binds the target again,
 * clears it (to 0,0,0,0) and draws reading the same texture: on Apple's GPU the
 * clear only sets the tile memory, and the pass reads the texture's memory,
 * which still holds the backdrop copied there by the pass before (trace 11-25:
 * "C bind fb 103, clear 0x4000 fb 103" between the copy and the vrd draw; the
 * copy read back 1087c7ff, after the clear 00000000). Mesa writes the clear,
 * so the backdrop was gone before the blur read it: the Dynamic Island's black
 * block. Here a colour clear of a target whose texture was drawn into earlier
 * and that nothing has drawn into since it was bound again is held back; a
 * no-program draw that reads that texture drops it (and is left out itself),
 * anything else makes it happen first: the next draw, another framebuffer or
 * attachment, a read, a flush, finish or fence. Switch a8gl-deferclear "0".
 */
#define FB_NAMES        8192
#define TEX_NAMES       65536
static int deferclear_on = 1;
static GLuint cur_fb, cur_tex;                 /* the bound framebuffer and its colour texture */
static GLuint fb_tex[FB_NAMES];               /* colour texture of each framebuffer (0: not known) */
static uint8_t tex_drawn[TEX_NAMES];          /* drawn into since it was last attached */
static unsigned draws_since_bind;
static unsigned long clears_held, clears_dropped;
static struct {
   bool active;
   void *context;
   GLbitfield mask;
   GLfloat colour[4];
   GLboolean scissor_on, colour_mask[4];
   GLint box[4];
} held;
static bool plain_reads_target;               /* plain_begin: a skipped draw read its own target */
/* glca23 (below) */
static void *alias_ctx;
static int cur_scr = -1;
static void alias_drawn(int k, const GLint *box);
static void alias_target(void *context);
static void alias_reset(void);
static void (*next_clear)(void *, GLbitfield);
static void (*next_bind_framebuffer)(void *, GLenum, GLuint);
static void (*next_framebuffer_texture_2d)(void *, GLenum, GLenum, GLenum, GLuint, GLint);
static void (*next_read_pixels)(void *, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
static void (*next_flush)(void *);
static void (*next_finish)(void *);
static GLsync (*next_fence_sync)(void *, GLenum, GLbitfield);

/* the held clear, now (with the state it was asked with) */
static void
held_clear_now(void)
{
   GLfloat colour[4];
   GLboolean scissor_on, mask[4];
   GLint box[4];

   if (!held.active)
      return;
   held.active = false;
   gl.get_floatv(0x0C22 /* COLOR_CLEAR_VALUE */, colour);
   gl.get_booleanv(0x0C23 /* COLOR_WRITEMASK */, mask);
   gl.get_integerv(0x0C10 /* SCISSOR_BOX */, box);
   scissor_on = gl.is_enabled(0x0C11 /* SCISSOR_TEST */);
   gl.clear_color(held.colour[0], held.colour[1], held.colour[2], held.colour[3]);
   gl.color_mask(held.colour_mask[0], held.colour_mask[1], held.colour_mask[2], held.colour_mask[3]);
   gl.scissor(held.box[0], held.box[1], held.box[2], held.box[3]);
   if (held.scissor_on)
      gl.enable(0x0C11);
   else
      gl.disable(0x0C11);
   next_clear(held.context, held.mask);
   if (held.context == alias_ctx)
      alias_drawn(cur_scr, NULL);              /* glca23 */
   gl.clear_color(colour[0], colour[1], colour[2], colour[3]);
   gl.color_mask(mask[0], mask[1], mask[2], mask[3]);
   gl.scissor(box[0], box[1], box[2], box[3]);
   if (scissor_on)
      gl.enable(0x0C11);
   else
      gl.disable(0x0C11);
}

static bool
held_procs(void)
{
   return gl.get_floatv && gl.get_booleanv && gl.clear_color && gl.color_mask && gl.scissor && gl.is_enabled &&
          gl.enable && gl.disable && gl.get_integerv;
}

static void
my_glClear(void *context, GLbitfield mask)
{
   jackpotlte_gl_use(context);
   load();
   if (held.active)
      held_clear_now();
   if (deferclear_on && held_procs() && mask == 0x4000 /* COLOR_BUFFER_BIT */ && cur_fb != 0 && cur_tex != 0 &&
       cur_tex < TEX_NAMES && tex_drawn[cur_tex] && draws_since_bind == 0) {
      held.active = true;
      held.context = context;
      held.mask = mask;
      gl.get_floatv(0x0C22, held.colour);
      gl.get_booleanv(0x0C23, held.colour_mask);
      gl.get_integerv(0x0C10, held.box);
      held.scissor_on = gl.is_enabled(0x0C11);
      clears_held++;
      return;
   }
   if (context == alias_ctx && (mask & 0x4000))
      alias_drawn(cur_scr, NULL);              /* glca23 */
   next_clear(context, mask);
}

static void
my_glBindFramebuffer(void *context, GLenum target, GLuint framebuffer)
{
   jackpotlte_gl_use(context);
   if (held.active)
      held_clear_now();
   next_bind_framebuffer(context, target, framebuffer);
   if (target == 0x8D40 /* FRAMEBUFFER */ || target == 0x8CA9 /* DRAW_FRAMEBUFFER */) {
      cur_fb = framebuffer;
      cur_tex = framebuffer < FB_NAMES ? fb_tex[framebuffer] : 0;
      draws_since_bind = 0;
      load();
      alias_target(context);                   /* glca23 */
   }
}

static void
my_glFramebufferTexture2D(void *context, GLenum target, GLenum attachment, GLenum textarget, GLuint texture,
                          GLint level)
{
   jackpotlte_gl_use(context);
   if (held.active)
      held_clear_now();
   next_framebuffer_texture_2d(context, target, attachment, textarget, texture, level);
   if (attachment == 0x8CE0 /* COLOR_ATTACHMENT0 */) {
      if (cur_fb < FB_NAMES)
         fb_tex[cur_fb] = level == 0 ? texture : 0;
      cur_tex = level == 0 ? texture : 0;
      if (texture < TEX_NAMES)
         tex_drawn[texture] = 0;
      load();
      alias_target(context);                   /* glca23 */
   }
}

static void
my_glReadPixels(void *context, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,
                void *pixels)
{
   jackpotlte_gl_use(context);
   if (held.active)
      held_clear_now();
   next_read_pixels(context, x, y, width, height, format, type, pixels);
}

static void
my_glFlush(void *context)
{
   if (held.active) {
      jackpotlte_gl_use(context);
      held_clear_now();
   }
   next_flush(context);
   alias_reset();                              /* glca23 */
}

static void
my_glFinish(void *context)
{
   if (held.active) {
      jackpotlte_gl_use(context);
      held_clear_now();
   }
   next_finish(context);
   alias_reset();                              /* glca23 */
}

static GLsync
my_glFenceSync(void *context, GLenum condition, GLbitfield flags)
{
   if (held.active) {
      jackpotlte_gl_use(context);
      held_clear_now();
   }
   {
      GLsync fence = next_fence_sync(context, condition, flags);

      alias_reset();                           /* glca23 */
      return fence;
   }
}
static GLint wide_reach = -1;          /* its "reach" uniform */
static int wide_on = 1;                /* a8gl-wide "0": the 16-sample stand-in instead */

static void
make_program_with(struct our_program *our, const char *vertex, const char *fragment, const char *numbers,
                  const char *what)
{
   GLuint p, shader;
   const GLchar *text;
   GLint good = 0;

   our->tried = 1;
   p = gl.create_program();
   shader = gl.create_shader(GL_VERTEX_SHADER);
   text = vertex;
   gl.shader_source(shader, 1, &text, NULL);
   gl.compile_shader(shader);
   gl.attach_shader(p, shader);
   shader = gl.create_shader(GL_FRAGMENT_SHADER);
   text = fragment;
   gl.shader_source(shader, 1, &text, NULL);
   gl.compile_shader(shader);
   gl.attach_shader(p, shader);
   gl.bind_attrib_location(p, 0, "vertex_position");
   gl.bind_attrib_location(p, 1, "vertex_color");
   gl.bind_attrib_location(p, 2, "vertex_texcoord0");
   gl.bind_attrib_location(p, 3, "vertex_texcoord1");     /* glca22: the variable blur's mask */
   gl.link_program(p);
   gl.get_programiv(p, GL_LINK_STATUS, &good);
   say("a program of ours for draws with no program (%s): %s", what, good ? "made" : "NOT MADE");
   if (!good) {
      char words[512] = "";

      if (gl.get_shader_info_log != NULL)
         gl.get_shader_info_log(shader, sizeof(words), NULL, words);
      if (words[0] != '\0')
         say("  its fragment shader: %s", words);
      return;
   }
   our->program = p;
   our->matrix = gl.get_uniform_location(p, "vertex_matrix");
   our->numbers = numbers != NULL ? gl.get_uniform_location(p, numbers) : -1;
   gl.use_program(p);
   gl.uniform1i(gl.get_uniform_location(p, "texture0"), 0);
   if (fragment == table_fragment)
      gl.uniform1i(gl.get_uniform_location(p, "table"), 1);
   if (gl.get_uniform_location(p, "mask") >= 0)
      gl.uniform1i(gl.get_uniform_location(p, "mask"), 1);
}

static void
make_program(struct our_program *our, const char *fragment, const char *numbers, const char *what)
{
   make_program_with(our, plain_vertex, fragment, numbers, what);
}

static bool
procs_for_plain(void)
{
   return gl.create_program && gl.create_shader && gl.shader_source && gl.compile_shader &&
          gl.attach_shader && gl.bind_attrib_location && gl.link_program && gl.get_programiv &&
          gl.get_uniform_location && gl.use_program && gl.uniform_matrix4fv && gl.uniform1i &&
          gl.get_integerv && gl.active_texture && gl.get_vertex_attribiv &&
          gl.enable_vertex_attrib_array && gl.disable_vertex_attrib_array &&
          gl.uniform4fv && gl.uniform2fv && gl.is_enabled && gl.enable && gl.disable;
}

/* the texture the bound framebuffer draws into, or 0 */
static GLint
colour_texture(void)
{
   GLint framebuffer = 0, type = 0, name = 0;

   gl.get_integerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
   if (framebuffer == 0 || gl.get_framebuffer_attachment_parameteriv == NULL)
      return 0;
   gl.get_framebuffer_attachment_parameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                             GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
   if (type != GL_TEXTURE)
      return 0;
   gl.get_framebuffer_attachment_parameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                             GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
   return name;
}

struct plain_state {
   GLint active;
   GLint enabled[3];
   GLboolean blended;   /* blending was on and our program blends itself */
};

/* how many source pixels one target pixel of a no-program draw covers: from
 * its first vertices' places (attribute 0, target pixels) and texture
 * coordinates (attribute 2, source pixels), both client arrays; 0 if unknown */
static float
vertex_float(GLuint index, unsigned vertex, unsigned component)
{
   GLint size = 0, type = 0, stride = 0, buffer = 0;
   void *pointer = NULL;

   gl.get_vertex_attribiv(index, 0x8623 /* SIZE */, &size);
   gl.get_vertex_attribiv(index, 0x8625 /* TYPE */, &type);
   gl.get_vertex_attribiv(index, 0x8624 /* STRIDE */, &stride);
   gl.get_vertex_attribiv(index, 0x889f /* BUFFER_BINDING */, &buffer);
   gl.get_vertex_attrib_pointerv(index, 0x8645 /* POINTER */, &pointer);
   if (buffer != 0 || pointer == NULL || type != 0x1406 /* FLOAT */ || (GLint)component >= size)
      return 0;
   if (stride == 0)
      stride = size * 4;
   return ((const float *)(const void *)((const char *)pointer + (size_t)stride * vertex))[component];
}

static float
reach_of_draw(void)
{
   float best = 0;

   if (gl.get_vertex_attrib_pointerv == NULL)
      return 0;
   for (unsigned a = 0; a < 3; a++) {
      unsigned b = (a + 1) % 3;
      float px = vertex_float(0, b, 0) - vertex_float(0, a, 0), py = vertex_float(0, b, 1) - vertex_float(0, a, 1);
      float tx = vertex_float(2, b, 0) - vertex_float(2, a, 0), ty = vertex_float(2, b, 1) - vertex_float(2, a, 1);
      float p = px * px + py * py, t = tx * tx + ty * ty;

      if (p > .25f && t > 0) {
         float r = __builtin_sqrtf(t / p);

         if (r > best)
            best = r;
      }
   }
   return best > 64 ? 64 : best;
}

/* a census: each function the first time it is met with no program */
static void
census(unsigned function, const char *how)
{
   static uint8_t said[FUNCTIONS + 1];
   unsigned at = function < FUNCTIONS ? function : FUNCTIONS;

   if (said[at])
      return;
   said[at] = 1;
   say("no-program draw of function %u (\"%.3s\"): %s", function,
       function < FUNCTIONS ? function_names + 4 * function : "?", how);
}

/* true: our program is in use for this draw (plain_end puts things back) */
static bool
plain_begin(void *context, struct plain_state *state)
{
   GLint program = 0, texture0 = 0, texture1 = 0, rows = 0;
   struct our_program *our = NULL;
   unsigned function;
   float reach = 0;

   plain_skip = false;
   plain_reads_target = false;
   if (!plain_on)
      return false;
   jackpotlte_gl_use(context);
   load();
   if (!procs_for_plain())
      return false;
   gl.get_integerv(GL_CURRENT_PROGRAM, &program);
   if (program != 0)
      return false;
   function = current_function();

   gl.get_integerv(GL_ACTIVE_TEXTURE, &state->active);
   gl.active_texture(GL_TEXTURE0 + 1);
   gl.get_integerv(GL_TEXTURE_BINDING_2D, &texture1);
   if (texture1 != 0 && gl.get_tex_level_parameteriv != NULL)
      gl.get_tex_level_parameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &rows);
   gl.active_texture(GL_TEXTURE0);
   gl.get_integerv(GL_TEXTURE_BINDING_2D, &texture0);

   /* no texture, or one that is also what is drawn into: left alone (Scion:
    * such draws painted white or read unwritten memory) */
   if (texture0 == 0 || texture0 == colour_texture()) {
      gl.active_texture((GLenum)state->active);
      census(function, texture0 == 0 ? "no texture, not drawn" : "reads what it writes, not drawn");
      plain_skip = true;
      plain_reads_target = texture0 != 0;
      return false;
   }

   if (function == FUNCTION_VIBRANT_MATRIX && function_given[function] == 20) {
      if (!vibrant.tried)
         make_program(&vibrant, vibrant_fragment, "colours", "colour matrix over what is behind");
      our = &vibrant;
   } else if (function == FUNCTION_MATRIX_PREMULTIPLIED && function_given[function] == 20) {
      if (!masking.tried)
         make_program(&masking, masking_fragment, "colours", "colour matrix, premultiplied");
      our = &masking;
   } else if (function == FUNCTION_SHADOW_BLUR && function_given[function] == 8) {
      if (!blur4.tried)
         make_program(&blur4, blur4_fragment, "places", "four samples around a point");
      our = &blur4;
   } else if (function == FUNCTION_DOWNSAMPLE_8 && wide_on && (reach = reach_of_draw()) > 1.5f) {
      if (!wide.tried) {
         make_program(&wide, wide_fragment, NULL, "a wide Gaussian downsample");
         if (wide.program != 0)
            wide_reach = gl.get_uniform_location(wide.program, "reach");
      }
      our = &wide;
   } else if (function == FUNCTION_DOWNSAMPLE_8 && function_given[function] == 8) {
      if (!box.tried)
         make_program(&box, box_fragment, "places", "eight times smaller");
      our = &box;
   } else if (function == FUNCTION_VARIABLE_BLUR && vblur_px > 0 && texture1 != 0) {
      if (!vblur.tried) {
         make_program_with(&vblur, vblur_vertex, vblur_fragment, NULL, "a variable blur through a one-column mask");
         if (vblur.program != 0)
            vblur_radius = gl.get_uniform_location(vblur.program, "radius");
      }
      our = &vblur;
   } else if (function == FUNCTION_ALPHA_THRESHOLD && alt_on) {
      if (!athresh.tried)
         make_program(&athresh, alt_fragment, "values", "an alpha threshold");
      our = &athresh;
   } else if (function == FUNCTION_VIBRANT_MATRIX || function == FUNCTION_MATRIX_PREMULTIPLIED ||
              function == FUNCTION_SHADOW_BLUR || function == FUNCTION_DOWNSAMPLE_8 ||
              (function < FUNCTIONS && function != FUNCTION_CUSTOM &&
               function != FUNCTION_HOME_AFFORDANCE && !(texture1 != 0 && rows == 1))) {
      /* a function we have no stand-in for: drawn as a picture it made a
       * red veil (vcm) or blocks (the blur chain), so it is left out */
      gl.active_texture((GLenum)state->active);
      census(function, "no stand-in, not drawn");
      plain_skip = true;
      return false;
   } else if (texture1 != 0 && rows == 1) {
      if (!table.tried)
         make_program(&table, table_fragment, NULL, "through a table");
      our = &table;
   } else {
      if (!plain.tried)
         make_program(&plain, plain_fragment, NULL, "as they are");
      our = &plain;
   }
   if (our->program == 0) {
      gl.use_program(0);
      gl.active_texture((GLenum)state->active);
      census(function, "our program did not build, not drawn");
      plain_skip = true;
      return false;
   }

   gl.use_program(our->program);
   gl.uniform_matrix4fv(our->matrix, 1, 0, ca_matrix != NULL ? ca_matrix : identity);
   if (our == &vibrant || our == &masking)
      gl.uniform4fv(our->numbers, 5, function_values[function]);
   else if (our == &blur4 || our == &box)
      gl.uniform2fv(our->numbers, 4, function_values[function]);
   else if (our == &wide)
      gl.uniform1f(wide_reach, reach);
   else if (our == &vblur)
      gl.uniform1f(vblur_radius, (GLfloat)vblur_px);
   else if (our == &athresh)
      gl.uniform4fv(our->numbers, 1, function_values[function]);
   state->blended = 0;
   if (our == &vibrant && gl.is_enabled(GL_BLEND)) {
      state->blended = 1;      /* the program mixes by itself */
      gl.disable(GL_BLEND);
   }
   for (GLuint i = 0; i < 3; i++) {
      gl.get_vertex_attribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &state->enabled[i]);
      if (!state->enabled[i])
         gl.enable_vertex_attrib_array(i);
   }
   census(function, our == &plain ? "drawn as the picture (colour times texture)" :
                    our == &table ? "drawn through the table (home indicator)" :
                    our == &vibrant ? "drawn with our vibrant colour matrix" :
                    our == &masking ? "drawn with our premultiplied colour matrix" :
                    our == &blur4 ? "drawn with our four-sample blur" :
                    our == &wide ? "drawn with our wide Gaussian downsample" :
                    our == &vblur ? "drawn with our variable blur (glca22)" :
                    our == &athresh ? "drawn with our alpha threshold (glca22)" : "drawn with our 8x downsample");
   return true;
}

static void
plain_end(struct plain_state *state)
{
   if (state->blended)
      gl.enable(GL_BLEND);
   for (GLuint i = 0; i < 3; i++) {
      if (!state->enabled[i])
         gl.disable_vertex_attrib_array(i);
   }
   gl.use_program(0);
   gl.active_texture((GLenum)state->active);
}

/*
 * glca21: each draw scissored to its own vertices.  panfrost sizes a pass (the
 * tiles its fragment job loads and stores) by the union of its draws'
 * viewport and scissor, and CoreAnimation draws with the whole target as the
 * viewport and no scissor, so every pass on the screen loads and stores all
 * 9.6 MB however little it draws.  Control Center's backdrops end the screen's
 * pass about 8 times a frame (11-25 Mesa log: "read of 1080x2220 ... submits
 * its writer: 1080x2220, 1 draws"), the main GPU cost there.  Here a draw's
 * vertex_position (client memory) goes through the program's vertex_matrix
 * and the viewport, and the draw gets that box, 2 pixels wider, as its
 * scissor (within CoreAnimation's own scissor); the scissor state is put back
 * after the draw.  Draws whose vertices are not floats in client memory, with
 * w <= 0, more than 4096 vertices, or a box of most of the viewport are left
 * alone.  Switch: a8gl-bounds "0".
 */
static int bounds_on = 1;
static unsigned long bounds_draws, bounds_cut, bounds_left;
#define BOUNDS_PROGRAMS 2048
static struct {
   GLint matrix, position;
   GLuint known;
} bounds_prog[BOUNDS_PROGRAMS];
static unsigned bounds_odd_said;

/*
 * glca21b: only a program whose vertex shader sets gl_Position once, as
 * "gl_Position = vertex_matrix * vertex_position;" (whitespace aside), has its
 * draws scissored; the first version also cut the Dynamic Island's draw in
 * Control Center, so some programs place their vertices another way.  The
 * first few other vertex shaders that use vertex_matrix are logged.
 */
static bool
bounds_program_plain(GLuint program)
{
   GLuint shaders[4];
   GLsizei n = 0;
   bool plain = false;

   if (gl.get_attached_shaders == NULL || gl.get_shader_source == NULL || gl.get_shaderiv == NULL)
      return false;
   gl.get_attached_shaders(program, 4, &n, shaders);
   for (GLsizei i = 0; i < n; i++) {
      GLint type = 0, length = 0;

      gl.get_shaderiv(shaders[i], 0x8B4F /* SHADER_TYPE */, &type);
      if (type != 0x8B31 /* VERTEX_SHADER */)
         continue;
      gl.get_shaderiv(shaders[i], 0x8B88 /* SHADER_SOURCE_LENGTH */, &length);
      if (length <= 0 || length > 65536)
         return false;
      {
         char *text = malloc((size_t)length + 1), *squeezed = malloc((size_t)length + 1);
         size_t k = 0;
         GLsizei got = 0;

         if (text == NULL || squeezed == NULL) {
            free(text);
            free(squeezed);
            return false;
         }
         gl.get_shader_source(shaders[i], length + 1, &got, text);
         text[got < length ? got : length] = '\0';
         for (const char *c = text; *c != '\0'; c++)
            if (*c != ' ' && *c != '\t' && *c != '\n' && *c != '\r')
               squeezed[k++] = *c;
         squeezed[k] = '\0';
         {
            /* writes of gl_Position: "gl_Position=" (not "=="), a swizzle then
             * "=", or a compound assignment; CoreAnimation's shaders also read
             * it (texcoordD, texcoordN), which is fine */
            unsigned writes = 0;
            bool form = false;

            for (const char *at = strstr(squeezed, "gl_Position"); at != NULL;
                 at = strstr(at + 11, "gl_Position")) {
               const char *c = at + 11;

               if (*c == '.')
                  for (c++; (*c >= 'a' && *c <= 'z'); c++)
                     ;
               if ((c[0] == '=' && c[1] != '=') ||
                   ((c[0] == '+' || c[0] == '-' || c[0] == '*' || c[0] == '/') && c[1] == '=')) {
                  writes++;
                  form = strncmp(at, "gl_Position=vertex_matrix*vertex_position;", 42) == 0;
               }
            }
            plain = writes == 1 && form;
         }
         if (!plain && strstr(squeezed, "vertex_matrix") != NULL && bounds_odd_said < 3) {
            size_t len = strlen(squeezed);

            bounds_odd_said++;
            say("glca21b: program %u places its vertices its own way (%zu chars):", program, len);
            for (size_t at = 0; at < len && at < 3600; at += 160)
               say("glca21b: %u+%zu %.160s", program, at, squeezed + at);
         }
         free(text);
         free(squeezed);
      }
   }
   return plain;
}

struct bounds_state {
   GLboolean was_on;
   GLint box[4];
   bool active;
   GLint program;          /* glca23: the program, -1 when not asked */
   bool known;             /* glca23: drawn is the draw's box on a screen-sized target */
   GLint drawn[4];         /* x0, y0, x1, y1 */
   bool empty;             /* glca24: its vertices have no width or no height */
};

static bool
bounds_point(const char *base, GLint stride, GLint size, GLuint index, const GLfloat *m,
             const GLint *vp, float *x, float *y)
{
   const float *p = (const float *)(const void *)(base + (size_t)stride * index);
   float vx = p[0], vy = p[1], vz = size > 2 ? p[2] : 0.f, vw = size > 3 ? p[3] : 1.f;
   float cx = m[0] * vx + m[4] * vy + m[8] * vz + m[12] * vw;
   float cy = m[1] * vx + m[5] * vy + m[9] * vz + m[13] * vw;
   float cw = m[3] * vx + m[7] * vy + m[11] * vz + m[15] * vw;

   if (!(cw > 1e-6f))
      return false;
   *x = ((cx / cw) * .5f + .5f) * (float)vp[2] + (float)vp[0];
   *y = ((cy / cw) * .5f + .5f) * (float)vp[3] + (float)vp[1];
   return true;
}

static bool
bounds_begin(GLint first, GLsizei count, GLenum type, const void *indices, struct bounds_state *state)
{
   GLint program = 0, matrix, position, on = 0, size = 0, kind = 0, stride = 0, buffer = 0;
   GLint elements = 0, vp[4] = { 0, 0, 0, 0 };
   GLfloat m[16];
   void *pointer = NULL;
   float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;

   state->active = false;
   state->program = -1;
   state->known = false;
   state->empty = false;
   if (!bounds_on || count <= 0 || count > 4096 || gl.get_uniformfv == NULL ||
       gl.get_attrib_location == NULL || gl.scissor == NULL)
      return false;
   bounds_draws++;
   gl.get_integerv(0x8B8D /* CURRENT_PROGRAM */, &program);
   state->program = program;
   if (program <= 0)
      goto left;
   if (program < BOUNDS_PROGRAMS && bounds_prog[program].known == (GLuint)program) {
      matrix = bounds_prog[program].matrix;
      position = bounds_prog[program].position;
   } else {
      matrix = gl.get_uniform_location((GLuint)program, "vertex_matrix");
      position = gl.get_attrib_location((GLuint)program, "vertex_position");
      if (matrix >= 0 && position >= 0 && !bounds_program_plain((GLuint)program))
         matrix = -1;    /* glca21b: not the plain matrix-times-position form */
      if (program < BOUNDS_PROGRAMS) {
         bounds_prog[program].matrix = matrix;
         bounds_prog[program].position = position;
         bounds_prog[program].known = (GLuint)program;
      }
   }
   if (matrix < 0 || position < 0)
      goto left;
   gl.get_vertex_attribiv((GLuint)position, 0x8622 /* ENABLED */, &on);
   gl.get_vertex_attribiv((GLuint)position, 0x8623 /* SIZE */, &size);
   gl.get_vertex_attribiv((GLuint)position, 0x8625 /* TYPE */, &kind);
   gl.get_vertex_attribiv((GLuint)position, 0x8624 /* STRIDE */, &stride);
   gl.get_vertex_attribiv((GLuint)position, 0x889f /* BUFFER_BINDING */, &buffer);
   gl.get_vertex_attrib_pointerv((GLuint)position, 0x8645 /* POINTER */, &pointer);
   if (!on || buffer != 0 || pointer == NULL || kind != 0x1406 /* FLOAT */ || size < 2)
      goto left;
   if (stride == 0)
      stride = size * 4;
   if (indices != NULL || type != 0) {
      gl.get_integerv(0x8895 /* ELEMENT_ARRAY_BUFFER_BINDING */, &elements);
      if (elements != 0 || indices == NULL ||
          (type != 0x1401 && type != 0x1403 && type != 0x1405))
         goto left;
   }
   gl.get_uniformfv((GLuint)program, matrix, m);
   gl.get_integerv(0x0BA2 /* VIEWPORT */, vp);
   for (GLsizei i = 0; i < count; i++) {
      GLuint index;
      float x, y;

      if (indices == NULL)
         index = (GLuint)(first + i);
      else if (type == 0x1403)
         index = ((const GLushort *)indices)[i];
      else if (type == 0x1401)
         index = ((const GLubyte *)indices)[i];
      else
         index = ((const GLuint *)indices)[i];
      if (!bounds_point((const char *)pointer, stride, size, index, m, vp, &x, &y))
         goto left;
      minx = x < minx ? x : minx;
      maxx = x > maxx ? x : maxx;
      miny = y < miny ? y : miny;
      maxy = y > maxy ? y : maxy;
   }
   /* glca24: every vertex on one line (the Dynamic Island's capsules draw
    * their straight middle part with no height when they are round) */
   state->empty = count >= 3 && (maxx - minx == 0.f || maxy - miny == 0.f);
   /* glca21c: screen-sized targets only (the panel's buffers, CoreAnimation's
    * full-screen offscreen); small offscreen passes are small already, and
    * glca21 cut the Dynamic Island in one of them */
   if (vp[2] < 1000 || vp[3] < 2000)
      goto left;
   {
      GLint x0 = (GLint)__builtin_floorf(minx) - 2, y0 = (GLint)__builtin_floorf(miny) - 2;
      GLint x1 = (GLint)__builtin_ceilf(maxx) + 2, y1 = (GLint)__builtin_ceilf(maxy) + 2;

      if (minx < -1e8f || miny < -1e8f || maxx > 1e8f || maxy > 1e8f)
         goto left;
      x0 = x0 < vp[0] ? vp[0] : x0;
      y0 = y0 < vp[1] ? vp[1] : y0;
      x1 = x1 > vp[0] + vp[2] ? vp[0] + vp[2] : x1;
      y1 = y1 > vp[1] + vp[3] ? vp[1] + vp[3] : y1;
      state->was_on = gl.is_enabled(0x0C11 /* SCISSOR_TEST */);
      gl.get_integerv(0x0C10 /* SCISSOR_BOX */, state->box);
      if (state->was_on) {
         x0 = x0 < state->box[0] ? state->box[0] : x0;
         y0 = y0 < state->box[1] ? state->box[1] : y0;
         x1 = x1 > state->box[0] + state->box[2] ? state->box[0] + state->box[2] : x1;
         y1 = y1 > state->box[1] + state->box[3] ? state->box[1] + state->box[3] : y1;
      }
      if (x1 < x0)
         x1 = x0;
      if (y1 < y0)
         y1 = y0;
      state->known = true;    /* glca23 */
      state->drawn[0] = x0;
      state->drawn[1] = y0;
      state->drawn[2] = x1;
      state->drawn[3] = y1;
      if ((float)(x1 - x0) * (float)(y1 - y0) > .85f * (float)vp[2] * (float)vp[3])
         goto left;
      if (!state->was_on)
         gl.enable(0x0C11);
      gl.scissor(x0, y0, x1 - x0, y1 - y0);
      state->active = true;
      bounds_cut++;
      return true;
   }
left:
   bounds_left++;
   return false;
}

static void
bounds_end(struct bounds_state *state)
{
   if (!state->active)
      return;
   gl.scissor(state->box[0], state->box[1], state->box[2], state->box[3]);
   if (!state->was_on)
      gl.disable(0x0C11);
   if ((bounds_draws & 16383) == 0)
      say("glca21: %lu draws, %lu scissored to their vertices, %lu left alone; %lu no-program draws left out; "
          "%lu clears held, %lu dropped",
          bounds_draws, bounds_cut, bounds_left, skipped_np, clears_held, clears_dropped);
}

/*
 * glca23: a backdrop read that does not end the screen's pass.  Each Control
 * Center module (and each widget, the dock, a folder) is a backdrop layer:
 * CoreAnimation copies the screen under it into a texture (function 1 from
 * the panel buffer's texture), tints that, and draws it back.  The copy
 * reads the screen, so Mesa submits the screen's open batch first: 13 more
 * screen passes a frame in Control Center (trace 11-25 18:20: "r" runs in
 * "S82 r7 o11 S r2 S2 r2 S2 ..."), each a submit and a fragment job, and
 * the reason Control Center is the slowest screen (P3a).  The modules never
 * overlap, so the part of the screen a copy reads was finished by the last
 * pass already, and nothing in the open batch touches it.
 *
 * Here the screen is drawn with the boxes glca21 computes, kept per panel
 * buffer (up to 16 boxes) since its batch was last submitted.  A draw into
 * another target that samples a panel buffer, through texture coordinates
 * that the program makes as vertex_texcoordN * texmatN.xy + texmatN.zw (client
 * arrays), reads a box; when that box is clear of the open batch's boxes, the
 * draw samples a second texture of the same surface memory (made once, with
 * glEGLImageTargetTexture2DOES like the first) instead, which Mesa tracks as a
 * different resource, so it does not submit the screen's batch.  What the
 * copy read must not be drawn over before the copy runs: the target the copy
 * went into is remembered, a draw that samples it (Mesa submits it then)
 * forgets it, and a screen draw over a remembered box before that submits
 * everything first (glFlush).  A screen draw whose box is not known counts
 * as the whole screen; clears, blits, copies and uploads into a panel buffer
 * too; a flush, finish or fence starts again.  Switch a8gl-alias "0".
 */
static int alias_on = 1;
#define ALIAS_SCREENS  4
#define ALIAS_BOXES    16
#define ALIAS_PENDING  16
#define ALIAS_UNITS    8
/* alias_ctx: CoreAnimation's context (the one that draws a panel buffer), declared above */
static struct {
   GLuint texture, alias;               /* the panel buffer's texture and the second one */
   uint32_t surface;
   int width, height;
   bool failed;                         /* the second texture could not be made */
   unsigned boxes;                      /* drawn since the last submit we know of */
   bool full;                           /* ... or the whole buffer */
   GLint box[ALIAS_BOXES][4];
   GLint set[4];                        /* the second texture's filters and wraps */
} scr[ALIAS_SCREENS];
/* cur_scr (declared above): the panel buffer drawn into, or -1 */
static struct {
   GLuint reader;                       /* the target a copy went into */
   int screen;
   GLint box[4];
} pend[ALIAS_PENDING];
static unsigned npend;
static GLint unit_tex[ALIAS_UNITS];     /* GL_TEXTURE_2D of each unit (alias_ctx's) */
static GLint active_unit;
static unsigned long alias_draws, alias_reads, alias_kept, alias_flushes, alias_unknown;
#define ALIAS_PROGRAMS 2048
static struct {
   GLuint known;
   int8_t sampler[2];                   /* unit of texture0/texture1, -1 when unused */
   int8_t form[2];                      /* 1: texcoordN = vertex_texcoordN * texmatN.xy + texmatN.zw */
   GLint texcoord[2], texmat[2];
} alias_prog[ALIAS_PROGRAMS];
static unsigned alias_shader_said;
static void (*next_bind_texture)(void *, GLenum, GLuint);
static void (*next_active_texture)(void *, GLenum);
static void (*next_delete_textures)(void *, GLsizei, const GLuint *);
static void (*next_blit_framebuffer)(void *, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
static void (*next_copy_tex_sub_image_2d)(void *, GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei);
static void (*next_copy_tex_image_2d)(void *, GLenum, GLint, GLenum, GLint, GLint, GLsizei, GLsizei, GLint);
static void (*next_tex_sub_image_2d)(void *, GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
static void (*next_draw_range_elements)(void *, GLenum, GLuint, GLuint, GLsizei, GLenum, const void *);
static void (*next_draw_arrays_instanced)(void *, GLenum, GLint, GLsizei, GLsizei);
static void (*next_draw_elements_instanced)(void *, GLenum, GLsizei, GLenum, const void *, GLsizei);

static bool
alias_procs(void)
{
   return gl.bind_texture && gl.gen_textures && gl.delete_textures && gl.tex_parameteri &&
          gl.get_tex_parameteriv && gl.get_uniformiv && gl.real_flush && gl.get_integerv &&
          gl.active_texture && gl.get_uniformfv && gl.get_attrib_location && gl.get_uniform_location &&
          gl.get_vertex_attribiv && gl.get_vertex_attrib_pointerv;
}

static int
alias_screen_of(GLuint texture)
{
   if (texture == 0)
      return -1;
   for (int k = 0; k < ALIAS_SCREENS; k++)
      if (scr[k].texture == texture)
         return k;
   return -1;
}

static bool
alias_overlap(const GLint a[4], const GLint b[4])
{
   return a[0] < b[2] && b[0] < a[2] && a[1] < b[3] && b[1] < a[3];
}

/* every open batch went to the GPU (a flush, finish or fence) */
static void
alias_reset(void)
{
   for (int k = 0; k < ALIAS_SCREENS; k++) {
      scr[k].boxes = 0;
      scr[k].full = false;
   }
   npend = 0;
}

/* glFlush now: a screen draw is about to cover what a copy has yet to read */
static void
alias_flush_now(void)
{
   alias_flushes++;
   gl.real_flush();
   alias_reset();
}

/* the screen K is drawn into, over BOX (NULL: everywhere) */
static void
alias_drawn(int k, const GLint *box)
{
   if (k < 0)
      return;
   for (unsigned i = 0; i < npend; i++) {
      if (pend[i].screen == k && (box == NULL || alias_overlap(box, pend[i].box))) {
         alias_flush_now();
         break;
      }
   }
   if (scr[k].full)
      return;
   if (box == NULL) {
      scr[k].full = true;
      return;
   }
   if (scr[k].boxes == ALIAS_BOXES) {
      /* full: the boxes become their union */
      GLint *u = scr[k].box[0];

      for (unsigned i = 1; i < ALIAS_BOXES; i++) {
         u[0] = scr[k].box[i][0] < u[0] ? scr[k].box[i][0] : u[0];
         u[1] = scr[k].box[i][1] < u[1] ? scr[k].box[i][1] : u[1];
         u[2] = scr[k].box[i][2] > u[2] ? scr[k].box[i][2] : u[2];
         u[3] = scr[k].box[i][3] > u[3] ? scr[k].box[i][3] : u[3];
      }
      scr[k].boxes = 1;
   }
   memcpy(scr[k].box[scr[k].boxes++], box, sizeof(scr[k].box[0]));
}

/* the framebuffer drawn into changed: is it a panel buffer? (cur_tex is its texture) */
static void
alias_target(void *context)
{
   int width = 0, height = 0;
   uint32_t surface;
   int k;

   cur_scr = -1;
   if (!alias_on || cur_tex == 0)
      return;
   if (!alias_procs()) {
      alias_on = 0;
      say("glca23: the library lacks a function it needs: off");
      return;
   }
   if (alias_ctx != NULL && context != alias_ctx)
      return;
   surface = jackpotlte_eagl_target_surface(context, cur_tex, &width, &height);
   k = alias_screen_of(cur_tex);
   if (k >= 0 && scr[k].surface != surface) {
      /* the texture shows another surface now: its second texture goes */
      if (scr[k].alias != 0 && gl.delete_textures != NULL)
         gl.delete_textures(1, &scr[k].alias);
      memset(&scr[k], 0, sizeof(scr[k]));
      k = -1;
   }
   if (surface == 0 || width < 1000 || height < 2000)
      return;
   if (k < 0) {
      for (k = 0; k < ALIAS_SCREENS && scr[k].texture != 0; k++)
         ;
      if (k == ALIAS_SCREENS)
         return;
      memset(&scr[k], 0, sizeof(scr[k]));
      scr[k].texture = cur_tex;
      scr[k].surface = surface;
      scr[k].width = width;
      scr[k].height = height;
      scr[k].full = true;          /* not known yet */
      if (alias_ctx == NULL) {
         alias_ctx = context;
         say("glca23: panel buffer texture %u (surface %u, %dx%d): backdrop copies that read "
             "what this pass has not drawn over read a second texture of it", cur_tex, surface,
             width, height);
      }
   }
   cur_scr = k;
}

/* which units a program samples, and how its texture coordinates are made */
static void
alias_program(GLuint program)
{
   if (program >= ALIAS_PROGRAMS || alias_prog[program].known == program)
      return;
   alias_prog[program].known = program;
   for (int n = 0; n < 2; n++) {
      char name[24];
      GLint where, unit = -1;

      snprintf(name, sizeof(name), "texture%d", n);
      where = gl.get_uniform_location(program, name);
      if (where >= 0)
         gl.get_uniformiv(program, where, &unit);
      alias_prog[program].sampler[n] = (int8_t)(where >= 0 && unit >= 0 && unit < ALIAS_UNITS ? unit : -1);
      snprintf(name, sizeof(name), "vertex_texcoord%d", n);
      alias_prog[program].texcoord[n] = gl.get_attrib_location(program, name);
      snprintf(name, sizeof(name), "texmat%d", n);
      alias_prog[program].texmat[n] = gl.get_uniform_location(program, name);
      alias_prog[program].form[n] = 0;
   }
   /* the vertex shader's own words for the texture coordinates */
   if (gl.get_attached_shaders != NULL && gl.get_shader_source != NULL && gl.get_shaderiv != NULL) {
      GLuint shaders[4];
      GLsizei count = 0;

      gl.get_attached_shaders(program, 4, &count, shaders);
      for (GLsizei i = 0; i < count; i++) {
         GLint type = 0, length = 0;
         char *text, *squeezed;
         GLsizei got = 0;
         size_t k = 0;

         gl.get_shaderiv(shaders[i], 0x8B4F /* SHADER_TYPE */, &type);
         if (type != 0x8B31 /* VERTEX_SHADER */)
            continue;
         gl.get_shaderiv(shaders[i], 0x8B88 /* SHADER_SOURCE_LENGTH */, &length);
         if (length <= 0 || length > 65536)
            continue;
         text = malloc((size_t)length + 1);
         squeezed = malloc((size_t)length + 1);
         if (text != NULL && squeezed != NULL) {
            gl.get_shader_source(shaders[i], length + 1, &got, text);
            text[got < length ? got : length] = '\0';
            for (const char *c = text; *c != '\0'; c++)
               if (*c != ' ' && *c != '\t' && *c != '\n' && *c != '\r')
                  squeezed[k++] = *c;
            squeezed[k] = '\0';
            for (int n = 0; n < 2; n++) {
               char a[96], b[96];

               snprintf(a, sizeof(a), "=vertex_texcoord%d.xy*texmat%d.xy+texmat%d.zw;", n, n, n);
               snprintf(b, sizeof(b), "=vertex_texcoord%d*texmat%d.xy+texmat%d.zw;", n, n, n);
               if (strstr(squeezed, a) != NULL || strstr(squeezed, b) != NULL)
                  alias_prog[program].form[n] = 1;
            }
            if (alias_prog[program].sampler[0] >= 0 && alias_prog[program].texcoord[0] >= 0 &&
                !alias_prog[program].form[0] && alias_shader_said < 4) {
               size_t len = strlen(squeezed);

               alias_shader_said++;
               say("glca23: program %u: texture coordinates made another way (%zu chars):", program, len);
               for (size_t at = 0; at < len && at < 2400; at += 160)
                  say("glca23: %u+%zu %.160s", program, at, squeezed + at);
            }
         }
         free(text);
         free(squeezed);
      }
   }
}

/* the box (texels of a W x H texture) a draw's unit texture coordinates N reach */
static bool
alias_read_box(GLuint program, int n, GLint first, GLsizei count, GLenum type, const void *indices,
               int width, int height, GLint box[4])
{
   GLint on = 0, size = 0, kind = 0, stride = 0, buffer = 0, elements = 0;
   GLint attribute = alias_prog[program].texcoord[n];
   GLfloat m[4];
   void *pointer = NULL;
   float u0 = 1e30f, v0 = 1e30f, u1 = -1e30f, v1 = -1e30f;

   if (!alias_prog[program].form[n] || attribute < 0 || alias_prog[program].texmat[n] < 0 ||
       count <= 0 || count > 4096)
      return false;
   gl.get_vertex_attribiv((GLuint)attribute, 0x8622 /* ENABLED */, &on);
   gl.get_vertex_attribiv((GLuint)attribute, 0x8623 /* SIZE */, &size);
   gl.get_vertex_attribiv((GLuint)attribute, 0x8625 /* TYPE */, &kind);
   gl.get_vertex_attribiv((GLuint)attribute, 0x8624 /* STRIDE */, &stride);
   gl.get_vertex_attribiv((GLuint)attribute, 0x889f /* BUFFER_BINDING */, &buffer);
   gl.get_vertex_attrib_pointerv((GLuint)attribute, 0x8645 /* POINTER */, &pointer);
   if (!on || buffer != 0 || pointer == NULL || kind != 0x1406 /* FLOAT */ || size < 2)
      return false;
   if (stride == 0)
      stride = size * 4;
   if (indices != NULL || type != 0) {
      gl.get_integerv(0x8895 /* ELEMENT_ARRAY_BUFFER_BINDING */, &elements);
      if (elements != 0 || indices == NULL || (type != 0x1401 && type != 0x1403 && type != 0x1405))
         return false;
   }
   gl.get_uniformfv(program, alias_prog[program].texmat[n], m);
   for (GLsizei i = 0; i < count; i++) {
      GLuint index;
      const float *p;
      float u, v;

      if (indices == NULL)
         index = (GLuint)(first + i);
      else if (type == 0x1403)
         index = ((const GLushort *)indices)[i];
      else if (type == 0x1401)
         index = ((const GLubyte *)indices)[i];
      else
         index = ((const GLuint *)indices)[i];
      p = (const float *)(const void *)((const char *)pointer + (size_t)stride * index);
      u = p[0] * m[0] + m[2];
      v = p[1] * m[1] + m[3];
      u0 = u < u0 ? u : u0;
      u1 = u > u1 ? u : u1;
      v0 = v < v0 ? v : v0;
      v1 = v > v1 ? v : v1;
   }
   if (!(u0 > -1e3f && v0 > -1e3f && u1 < 1e3f && v1 < 1e3f))
      return false;
   /* two texels more each way: filtering, rounding */
   box[0] = (GLint)__builtin_floorf(u0 * (float)width) - 2;
   box[1] = (GLint)__builtin_floorf(v0 * (float)height) - 2;
   box[2] = (GLint)__builtin_ceilf(u1 * (float)width) + 2;
   box[3] = (GLint)__builtin_ceilf(v1 * (float)height) + 2;
   return true;
}

/* the second texture of screen K, made once */
static bool
alias_make(void *context, int k)
{
   GLint was = 0;

   if (scr[k].alias != 0)
      return true;
   if (scr[k].failed)
      return false;
   gl.get_integerv(GL_TEXTURE_BINDING_2D, &was);
   gl.gen_textures(1, &scr[k].alias);
   gl.bind_texture(GL_TEXTURE_2D, scr[k].alias);
   if (scr[k].alias == 0 || !jackpotlte_eagl_alias_texture(context, scr[k].surface)) {
      scr[k].failed = true;
      say("glca23: no second texture of surface %u: backdrop copies end the screen's pass as before",
          scr[k].surface);
      if (scr[k].alias != 0)
         gl.delete_textures(1, &scr[k].alias);
      scr[k].alias = 0;
      gl.bind_texture(GL_TEXTURE_2D, (GLuint)was);
      return false;
   }
   scr[k].set[0] = scr[k].set[1] = scr[k].set[2] = scr[k].set[3] = -1;
   gl.bind_texture(GL_TEXTURE_2D, (GLuint)was);
   say("glca23: texture %u is a second texture of surface %u (panel buffer texture %u)",
       scr[k].alias, scr[k].surface, scr[k].texture);
   return true;
}

struct alias_state {
   int unit;               /* the unit whose texture was swapped, or -1 */
   GLuint put_back;        /* what was bound there */
   int flushed;            /* a screen whose batch Mesa submits for this draw, or -1 */
};

static void
alias_begin(void *context, GLint first, GLsizei count, GLenum type, const void *indices,
            const struct bounds_state *bounds, struct alias_state *as)
{
   GLint program;
   int sampled[2];

   as->unit = -1;
   as->flushed = -1;
   if (!alias_on || alias_ctx == NULL || context != alias_ctx)
      return;
   alias_draws++;
   program = bounds->program;
   if (program < 0)
      gl.get_integerv(GL_CURRENT_PROGRAM, &program);
   if (program <= 0 || program >= ALIAS_PROGRAMS) {
      /* not a program of CoreAnimation's we know: it may sample anything */
      sampled[0] = 0;
      sampled[1] = 1;
   } else {
      alias_program((GLuint)program);
      sampled[0] = alias_prog[program].sampler[0];
      sampled[1] = alias_prog[program].sampler[1];
   }
   /* a target a copy went into, sampled now: Mesa submits it first */
   for (int n = 0; n < 2 && npend > 0; n++) {
      GLint bound = 0;

      if (sampled[n] < 0)
         continue;
      if (unit_tex[sampled[n]] == 0 && program > 0)
         continue;
      gl.active_texture(GL_TEXTURE0 + (GLenum)sampled[n]);
      gl.get_integerv(GL_TEXTURE_BINDING_2D, &bound);
      gl.active_texture(GL_TEXTURE0 + (GLenum)active_unit);
      for (unsigned i = 0; i < npend; i++) {
         if (pend[i].reader == (GLuint)bound) {
            pend[i] = pend[--npend];
            i--;
         }
      }
   }
   if (cur_scr >= 0) {
      /* a draw into a panel buffer */
      alias_drawn(cur_scr, bounds->known ? bounds->drawn : NULL);
      if (!bounds->known)
         alias_unknown++;
      return;
   }
   if (program <= 0 || program >= ALIAS_PROGRAMS || cur_tex == 0)
      return;
   for (int n = 0; n < 2; n++) {
      int unit = sampled[n], k;
      GLint box[4], bound = 0, value[4];
      bool clear = true;
      static const GLenum names[4] = { 0x2801 /* MIN_FILTER */, 0x2800 /* MAG_FILTER */,
                                       0x2802 /* WRAP_S */, 0x2803 /* WRAP_T */ };

      if (unit < 0 || (k = alias_screen_of((GLuint)unit_tex[unit])) < 0)
         continue;
      if (!scr[k].full && scr[k].boxes == 0)
         continue;              /* nothing open on it: Mesa submits nothing */
      alias_reads++;
      if (scr[k].full || as->unit >= 0 || npend == ALIAS_PENDING ||
          !alias_read_box((GLuint)program, n, first, count, type, indices, scr[k].width,
                          scr[k].height, box)) {
         as->flushed = k;
         continue;
      }
      for (unsigned i = 0; i < scr[k].boxes && clear; i++)
         clear = !alias_overlap(box, scr[k].box[i]);
      if (!clear || !alias_make(context, k)) {
         as->flushed = k;
         continue;
      }
      /* what is really bound there (the shim's own calls bind without the table) */
      gl.active_texture(GL_TEXTURE0 + (GLenum)unit);
      gl.get_integerv(GL_TEXTURE_BINDING_2D, &bound);
      if (bound != (GLint)scr[k].texture) {
         gl.active_texture(GL_TEXTURE0 + (GLenum)active_unit);
         unit_tex[unit] = bound;
         as->flushed = alias_screen_of((GLuint)bound);
         continue;
      }
      for (int i = 0; i < 4; i++)
         gl.get_tex_parameteriv(GL_TEXTURE_2D, names[i], &value[i]);
      gl.bind_texture(GL_TEXTURE_2D, scr[k].alias);
      for (int i = 0; i < 4; i++) {
         if (scr[k].set[i] != value[i]) {
            gl.tex_parameteri(GL_TEXTURE_2D, names[i], value[i]);
            scr[k].set[i] = value[i];
         }
      }
      gl.active_texture(GL_TEXTURE0 + (GLenum)active_unit);
      as->unit = unit;
      as->put_back = scr[k].texture;
      pend[npend].reader = cur_tex;
      pend[npend].screen = k;
      memcpy(pend[npend].box, box, sizeof(box));
      npend++;
      alias_kept++;
   }
}

static void
alias_end(struct alias_state *as)
{
   if (as->unit >= 0) {
      gl.active_texture(GL_TEXTURE0 + (GLenum)as->unit);
      gl.bind_texture(GL_TEXTURE_2D, as->put_back);
      gl.active_texture(GL_TEXTURE0 + (GLenum)active_unit);
   }
   if (as->flushed >= 0) {
      /* the copy read the screen as it is: Mesa submitted its batch */
      scr[as->flushed].boxes = 0;
      scr[as->flushed].full = false;
   }
   if ((alias_draws & 16383) == 0 && alias_draws != 0)
      say("glca23: %lu draws, %lu read an open panel buffer, %lu of them through the second texture; "
          "%lu flushes to keep a copy's box, %lu screen draws of unknown size",
          alias_draws, alias_reads, alias_kept, alias_flushes, alias_unknown);
}

/* ---- glca23's view of the state: texture units, and writes it cannot box ---- */

static void
my_glActiveTexture(void *context, GLenum unit)
{
   next_active_texture(context, unit);
   if (context == alias_ctx || alias_ctx == NULL)
      active_unit = (GLint)(unit - GL_TEXTURE0) < ALIAS_UNITS ? (GLint)(unit - GL_TEXTURE0) : 0;
}

static void
my_glBindTexture(void *context, GLenum target, GLuint texture)
{
   next_bind_texture(context, target, texture);
   if (target == GL_TEXTURE_2D && (context == alias_ctx || alias_ctx == NULL) && active_unit >= 0 &&
       active_unit < ALIAS_UNITS)
      unit_tex[active_unit] = (GLint)texture;
}

static void
my_glDeleteTextures(void *context, GLsizei n, const GLuint *textures)
{
   if (context == alias_ctx && textures != NULL) {
      jackpotlte_gl_use(context);
      load();
      for (GLsizei i = 0; i < n; i++) {
         int k = alias_screen_of(textures[i]);

         for (unsigned j = 0; j < npend; j++)
            if (pend[j].reader == textures[i]) {
               alias_flush_now();      /* its copy must not wait on a name used again */
               break;
            }
         if (k >= 0) {
            if (scr[k].alias != 0)
               gl.delete_textures(1, &scr[k].alias);
            memset(&scr[k], 0, sizeof(scr[k]));
            if (cur_scr == k)
               cur_scr = -1;
         }
      }
   }
   next_delete_textures(context, n, textures);
}

/* the texture bound on the active unit, as a panel buffer index */
static int
alias_bound_screen(void)
{
   GLint bound = 0;

   gl.get_integerv(GL_TEXTURE_BINDING_2D, &bound);
   return alias_screen_of((GLuint)bound);
}

static void
my_glBlitFramebuffer(void *context, GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0,
                     GLint dx1, GLint dy1, GLbitfield mask, GLenum filter)
{
   if (context == alias_ctx) {
      jackpotlte_gl_use(context);
      alias_drawn(cur_scr, NULL);
   }
   next_blit_framebuffer(context, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
}

static void
my_glCopyTexSubImage2D(void *context, GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x,
                       GLint y, GLsizei width, GLsizei height)
{
   if (context == alias_ctx) {
      jackpotlte_gl_use(context);
      load();
      alias_drawn(alias_bound_screen(), NULL);
   }
   next_copy_tex_sub_image_2d(context, target, level, xoffset, yoffset, x, y, width, height);
}

static void
my_glCopyTexImage2D(void *context, GLenum target, GLint level, GLenum internal, GLint x, GLint y,
                    GLsizei width, GLsizei height, GLint border)
{
   if (context == alias_ctx) {
      jackpotlte_gl_use(context);
      load();
      alias_drawn(alias_bound_screen(), NULL);
   }
   next_copy_tex_image_2d(context, target, level, internal, x, y, width, height, border);
}

static void
my_glTexSubImage2D(void *context, GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                   GLsizei height, GLenum format, GLenum type, const void *pixels)
{
   if (context == alias_ctx) {
      jackpotlte_gl_use(context);
      load();
      alias_drawn(alias_bound_screen(), NULL);
   }
   next_tex_sub_image_2d(context, target, level, xoffset, yoffset, width, height, format, type, pixels);
}

static void
my_glDrawRangeElements(void *context, GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                       const void *indices)
{
   if (context == alias_ctx) {
      jackpotlte_gl_use(context);
      if (npend > 0)
         alias_flush_now();     /* what it samples is not looked at */
      alias_drawn(cur_scr, NULL);
   }
   next_draw_range_elements(context, mode, start, end, count, type, indices);
}

static void
my_glDrawArraysInstanced(void *context, GLenum mode, GLint first, GLsizei count, GLsizei instances)
{
   if (context == alias_ctx) {
      jackpotlte_gl_use(context);
      if (npend > 0)
         alias_flush_now();
      alias_drawn(cur_scr, NULL);
   }
   next_draw_arrays_instanced(context, mode, first, count, instances);
}

static void
my_glDrawElementsInstanced(void *context, GLenum mode, GLsizei count, GLenum type, const void *indices,
                           GLsizei instances)
{
   if (context == alias_ctx) {
      jackpotlte_gl_use(context);
      if (npend > 0)
         alias_flush_now();
      alias_drawn(cur_scr, NULL);
   }
   next_draw_elements_instanced(context, mode, count, type, indices, instances);
}


/*
 * glca24: a triangle draw whose vertices all lie on one line covers no pixel,
 * yet costs a Mesa draw (state, descriptors, a job entry): the Dynamic
 * Island's capsules draw 7 of their ~41 draws a frame like that (prog 9, the
 * straight middle part, no height when the capsule is round).  Such a draw
 * is left out after any held clear is done; switch a8gl-zero "0".
 */
static int zero_on = 1;
static unsigned long zero_skipped;

static bool
zero_skip(bool ours, GLenum mode, struct bounds_state *bounds, bool cut)
{
   if (!zero_on || ours || !bounds->empty || (mode != 4 && mode != 5 && mode != 6))
      return false;
   zero_skipped++;
   if (cut)
      bounds_end(bounds);
   if ((zero_skipped & 4095) == 1)
      say("glca24: %lu draws with no area left out", zero_skipped);
   return true;
}
static void
my_glDrawArrays(void *context, GLenum mode, GLint first, GLsizei count)
{
   struct plain_state state;
   struct bounds_state bounds;
   struct alias_state as;
   bool ours = plain_begin(context, &state);
   bool cut;

   if (!ours && plain_skip && skipnp_on) {
      skipped_np++;
      if (held.active && plain_reads_target) {
         held.active = false;   /* glca22d: Apple's GPU never writes this clear */
         clears_dropped++;
      }
      return;           /* glca22b: a no-program draw we cannot draw is left out */
   }
   if (held.active)
      held_clear_now();
   cut = bounds_begin(first, count, 0, NULL, &bounds);
   if (zero_skip(ours, mode, &bounds, cut))
      return;           /* glca24 */
   alias_begin(context, first, count, 0, NULL, &bounds, &as);   /* glca23 */
   next_draw_arrays(context, mode, first, count);
   alias_end(&as);
   draws_since_bind++;
   if (cur_tex != 0 && cur_tex < TEX_NAMES)
      tex_drawn[cur_tex] = 1;
   if (cut)
      bounds_end(&bounds);
   if (ours)
      plain_end(&state);
}

static void
my_glDrawElements(void *context, GLenum mode, GLsizei count, GLenum type, const void *indices)
{
   struct plain_state state;
   struct bounds_state bounds;
   struct alias_state as;
   bool ours = plain_begin(context, &state);
   bool cut;

   if (!ours && plain_skip && skipnp_on) {
      skipped_np++;
      if (held.active && plain_reads_target) {
         held.active = false;   /* glca22d: as in my_glDrawArrays */
         clears_dropped++;
      }
      return;           /* glca22b: as in my_glDrawArrays */
   }
   if (held.active)
      held_clear_now();
   cut = bounds_begin(0, count, type, indices, &bounds);
   if (zero_skip(ours, mode, &bounds, cut))
      return;           /* glca24 */
   alias_begin(context, 0, count, type, indices, &bounds, &as);  /* glca23 */
   next_draw_elements(context, mode, count, type, indices);
   alias_end(&as);
   draws_since_bind++;
   if (cur_tex != 0 && cur_tex < TEX_NAMES)
      tex_drawn[cur_tex] = 1;
   if (cut)
      bounds_end(&bounds);
   if (ours)
      plain_end(&state);
}

static void
my_glUniformMatrix4fv(void *context, GLint location, GLsizei count, GLboolean transpose,
                      const GLfloat *value)
{
   ca_matrix = value;
   next_uniform_matrix4fv(context, location, count, transpose, value);
}

/* ---- the table ---------------------------------------------------------- */

/* Offsets from eagl_slots.txt (gl_slots.c): index = offset / 8. */
void
ca_fixes_install(void *table_entries[JACKPOTLTE_GL_SLOTS])
{
   static int read_switches;

   if (!read_switches) {
      read_switches = 1;
      glsl_on = access("/private/var/mobile/Library/a8gl-noglsl", F_OK) != 0;
      plain_on = access("/private/var/mobile/Library/a8gl-noplain", F_OK) != 0;
      {
         /* "1" in the file turns it on (the console can write a file, not delete one) */
         FILE *f = fopen("/private/var/mobile/Library/a8gl-dbg-shape", "r");

         debug_shape = f == NULL ? 0 : fgetc(f) - '0';
         if (debug_shape < 0 || debug_shape > 1)
            debug_shape = 0;
         if (f != NULL)
            fclose(f);
         f = fopen("/private/var/mobile/Library/a8gl-shapes", "r");
         shapes_on = !(f != NULL && fgetc(f) == '0');
         if (f != NULL)
            fclose(f);
         f = fopen("/private/var/mobile/Library/a8gl-wide", "r");
         wide_on = !(f != NULL && fgetc(f) == '0');
         if (f != NULL)
            fclose(f);
         f = fopen("/private/var/mobile/Library/a8gl-bounds", "r");         /* glca21 */
         bounds_on = !(f != NULL && fgetc(f) == '0');
         if (f != NULL)
            fclose(f);
         f = fopen("/private/var/mobile/Library/a8gl-vblur", "r");          /* glca22 */
         if (f != NULL) {
            int v = 0;

            if (fscanf(f, "%d", &v) == 1)
               vblur_px = v < 0 ? 0 : v > 64 ? 64 : v;
            fclose(f);
         }
         f = fopen("/private/var/mobile/Library/a8gl-skipnp", "r");         /* glca22b */
         skipnp_on = !(f != NULL && fgetc(f) == '0');
         if (f != NULL)
            fclose(f);
         f = fopen("/private/var/mobile/Library/a8gl-deferclear", "r");     /* glca22d */
         deferclear_on = !(f != NULL && fgetc(f) == '0');
         if (f != NULL)
            fclose(f);
         f = fopen("/private/var/mobile/Library/a8gl-alias", "r");         /* glca23 */
         alias_on = !(f != NULL && fgetc(f) == '0');
         if (f != NULL)
            fclose(f);
         f = fopen("/private/var/mobile/Library/a8gl-zero", "r");          /* glca24 */
         zero_on = !(f != NULL && fgetc(f) == '0');
         if (f != NULL)
            fclose(f);
         f = fopen("/private/var/mobile/Library/a8gl-alt", "r");            /* glca22 */
         alt_on = f != NULL && fgetc(f) == '1';
         if (f != NULL)
            fclose(f);
      }
      say("ca fixes: glsl rewrite %s, no-program draws %s, shapes without derivatives %s, wide downsample %s, "
          "draws scissored to their vertices %s, variable blur %d px, alpha threshold %s, "
          "no-program draws we cannot draw %s, clears before a pass that reads its own target %s%s",
          glsl_on ? "on" : "off", plain_on ? "on" : "off", shapes_on ? "on" : "off", wide_on ? "on" : "off",
          bounds_on ? "on" : "off", vblur_px, alt_on ? "on" : "off", skipnp_on ? "left out" : "handed on",
          deferclear_on ? "held back" : "handed on",
          debug_shape ? ", SHAPE DEBUG (a8gl-dbg-shape)" : "");
      if (access("/private/var/mobile/Library/a8gl-nohooks", F_OK) != 0)
         hook_coreanimation();
   }
   /* (memcpy: a void * is not a function pointer to ISO C) */
   memcpy(&next_draw_arrays, &table_entries[0x042], sizeof(next_draw_arrays));
   memcpy(&next_draw_elements, &table_entries[0x044], sizeof(next_draw_elements));
   memcpy(&next_uniform_matrix4fv, &table_entries[0x26d], sizeof(next_uniform_matrix4fv));

   table_entries[0x042] = (void *)my_glDrawArrays;        /* 0x0210 */
   table_entries[0x044] = (void *)my_glDrawElements;      /* 0x0220 */
   table_entries[0x250] = (void *)my_glDeleteProgram;     /* 0x1280 glDeleteProgram glDeleteShader */
   table_entries[0x254] = (void *)my_glShaderSource;      /* 0x12a0 */
   table_entries[0x255] = (void *)my_glCompileShader;     /* 0x12a8 */
   table_entries[0x258] = (void *)my_glLinkProgram;       /* 0x12c0 */
   table_entries[0x26d] = (void *)my_glUniformMatrix4fv;  /* 0x1368 */

   /* glca22d: clears held back (see held_clear_now) */
   memcpy(&next_clear, &table_entries[0x00b], sizeof(next_clear));
   memcpy(&next_bind_framebuffer, &table_entries[0x2a1], sizeof(next_bind_framebuffer));
   memcpy(&next_framebuffer_texture_2d, &table_entries[0x2a6], sizeof(next_framebuffer_texture_2d));
   memcpy(&next_read_pixels, &table_entries[0x0ee], sizeof(next_read_pixels));
   memcpy(&next_flush, &table_entries[0x05b], sizeof(next_flush));
   memcpy(&next_finish, &table_entries[0x05a], sizeof(next_finish));
   memcpy(&next_fence_sync, &table_entries[0x30c], sizeof(next_fence_sync));
   if (next_clear != NULL && next_bind_framebuffer != NULL && next_framebuffer_texture_2d != NULL &&
       next_read_pixels != NULL && next_flush != NULL && next_finish != NULL && next_fence_sync != NULL) {
      table_entries[0x00b] = (void *)my_glClear;                 /* 0x0058 */
      table_entries[0x2a1] = (void *)my_glBindFramebuffer;       /* 0x1508 */
      table_entries[0x2a6] = (void *)my_glFramebufferTexture2D;  /* 0x1530 */
      table_entries[0x0ee] = (void *)my_glReadPixels;            /* 0x0770 */
      table_entries[0x05b] = (void *)my_glFlush;                 /* 0x02d8 */
      table_entries[0x05a] = (void *)my_glFinish;                /* 0x02d0 */
      table_entries[0x30c] = (void *)my_glFenceSync;             /* 0x1860 */
   } else {
      deferclear_on = 0;
   }

   /* glca23: texture units, and the writes into a panel buffer it cannot box */
   memcpy(&next_active_texture, &table_entries[0x157], sizeof(next_active_texture));
   memcpy(&next_bind_texture, &table_entries[0x006], sizeof(next_bind_texture));
   memcpy(&next_delete_textures, &table_entries[0x03c], sizeof(next_delete_textures));
   memcpy(&next_blit_framebuffer, &table_entries[0x2f4], sizeof(next_blit_framebuffer));
   memcpy(&next_copy_tex_sub_image_2d, &table_entries[0x039], sizeof(next_copy_tex_sub_image_2d));
   memcpy(&next_copy_tex_image_2d, &table_entries[0x037], sizeof(next_copy_tex_image_2d));
   memcpy(&next_tex_sub_image_2d, &table_entries[0x134], sizeof(next_tex_sub_image_2d));
   memcpy(&next_draw_range_elements, &table_entries[0x196], sizeof(next_draw_range_elements));
   memcpy(&next_draw_arrays_instanced, &table_entries[0x2fb], sizeof(next_draw_arrays_instanced));
   memcpy(&next_draw_elements_instanced, &table_entries[0x2fc], sizeof(next_draw_elements_instanced));
   if (alias_on && deferclear_on && next_active_texture && next_bind_texture &&
       next_delete_textures && next_blit_framebuffer && next_copy_tex_sub_image_2d && next_copy_tex_image_2d &&
       next_tex_sub_image_2d && next_draw_range_elements && next_draw_arrays_instanced &&
       next_draw_elements_instanced) {
      table_entries[0x157] = (void *)my_glActiveTexture;          /* 0x0ab8 */
      table_entries[0x006] = (void *)my_glBindTexture;            /* 0x0030 */
      table_entries[0x03c] = (void *)my_glDeleteTextures;         /* 0x01e0 */
      table_entries[0x2f4] = (void *)my_glBlitFramebuffer;        /* 0x17a0 */
      table_entries[0x039] = (void *)my_glCopyTexSubImage2D;      /* 0x01c8 */
      table_entries[0x037] = (void *)my_glCopyTexImage2D;         /* 0x01b8 */
      table_entries[0x134] = (void *)my_glTexSubImage2D;          /* 0x09a0 */
      table_entries[0x196] = (void *)my_glDrawRangeElements;      /* 0x0cb0 */
      table_entries[0x2fb] = (void *)my_glDrawArraysInstanced;    /* 0x17d8 */
      table_entries[0x2fc] = (void *)my_glDrawElementsInstanced;  /* 0x17e0 */
      say("glca23: backdrop copies may read a second texture of a panel buffer (a8gl-alias \"0\" off)");
   } else {
      alias_on = 0;
   }
   say("glca24: triangle draws with no area are %s (a8gl-zero \"0\" keeps them)", zero_on ? "left out" : "kept");
}
