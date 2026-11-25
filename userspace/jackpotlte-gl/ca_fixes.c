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
const float *ca_fixes_matrix(void);

unsigned
ca_fixes_function(void)
{
   return current_function();
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
static struct our_program plain, table, vibrant, masking, blur4, box, wide;
static GLint wide_reach = -1;          /* its "reach" uniform */
static int wide_on = 1;                /* a8gl-wide "0": the 16-sample stand-in instead */

static void
make_program(struct our_program *our, const char *fragment, const char *numbers, const char *what)
{
   GLuint p, shader;
   const GLchar *text;
   GLint good = 0;

   our->tried = 1;
   p = gl.create_program();
   shader = gl.create_shader(GL_VERTEX_SHADER);
   text = plain_vertex;
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
   } else if (function == FUNCTION_VIBRANT_MATRIX || function == FUNCTION_MATRIX_PREMULTIPLIED ||
              function == FUNCTION_SHADOW_BLUR || function == FUNCTION_DOWNSAMPLE_8 ||
              (function < FUNCTIONS && function != FUNCTION_CUSTOM &&
               function != FUNCTION_HOME_AFFORDANCE && !(texture1 != 0 && rows == 1))) {
      /* a function we have no stand-in for: drawn as a picture it made a
       * red veil (vcm) or blocks (the blur chain), so it is left out */
      gl.active_texture((GLenum)state->active);
      census(function, "no stand-in, not drawn");
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
                    our == &wide ? "drawn with our wide Gaussian downsample" : "drawn with our 8x downsample");
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
   if (!bounds_on || count <= 0 || count > 4096 || gl.get_uniformfv == NULL ||
       gl.get_attrib_location == NULL || gl.scissor == NULL)
      return false;
   bounds_draws++;
   gl.get_integerv(0x8B8D /* CURRENT_PROGRAM */, &program);
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
   /* glca21c: screen-sized targets only (the panel's buffers, CoreAnimation's
    * full-screen offscreen); small offscreen passes are small already, and
    * glca21 cut the Dynamic Island in one of them */
   if (vp[2] < 1000 || vp[3] < 2000)
      goto left;
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
      say("glca21: %lu draws, %lu scissored to their vertices, %lu left alone",
          bounds_draws, bounds_cut, bounds_left);
}

static void
my_glDrawArrays(void *context, GLenum mode, GLint first, GLsizei count)
{
   struct plain_state state;
   struct bounds_state bounds;
   bool ours = plain_begin(context, &state);
   bool cut = bounds_begin(first, count, 0, NULL, &bounds);

   next_draw_arrays(context, mode, first, count);
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
   bool ours = plain_begin(context, &state);
   bool cut = bounds_begin(0, count, type, indices, &bounds);

   next_draw_elements(context, mode, count, type, indices);
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
      }
      say("ca fixes: glsl rewrite %s, no-program draws %s, shapes without derivatives %s, wide downsample %s, "
          "draws scissored to their vertices %s%s",
          glsl_on ? "on" : "off", plain_on ? "on" : "off", shapes_on ? "on" : "off", wide_on ? "on" : "off",
          bounds_on ? "on" : "off",
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
}
