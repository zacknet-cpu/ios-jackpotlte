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
   void (*enable_vertex_attrib_array)(GLuint);
   void (*disable_vertex_attrib_array)(GLuint);
   int loaded;
} gl;

/* the table's entries before ours (they keep gl_slots.c's after-draw submit) */
static void (*next_draw_arrays)(void *, GLenum, GLint, GLsizei);
static void (*next_draw_elements)(void *, GLenum, GLsizei, GLenum, const void *);
static void (*next_uniform_matrix4fv)(void *, GLint, GLsizei, GLboolean, const GLfloat *);

static int glsl_on = 1;
static int plain_on = 1;

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
   PROC(enable_vertex_attrib_array, "glEnableVertexAttribArray");
   PROC(disable_vertex_attrib_array, "glDisableVertexAttribArray");
}

/* ---- shaders ------------------------------------------------------------ */

static const char all_highp[] =
   "#define lowp highp\n"
   "#define mediump highp\n";

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
   gl.shader_source(shader, 6, parts, NULL);
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

static const GLfloat identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

/* the matrix CoreAnimation last gave a program of its own (always from the
 * same place of its, so the pointer stays good) */
static const GLfloat *ca_matrix;

static struct {
   GLuint program;
   GLint matrix;
   int tried;
} plain, table;

static void
make_program(const char *fragment, const char *what, GLuint *program, GLint *matrix, int *tried)
{
   GLuint p, shader;
   const GLchar *text;
   GLint good = 0;

   *tried = 1;
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
   if (!good)
      return;
   *program = p;
   *matrix = gl.get_uniform_location(p, "vertex_matrix");
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
          gl.enable_vertex_attrib_array && gl.disable_vertex_attrib_array;
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
};

/* true: our program is in use for this draw (plain_end puts things back) */
static bool
plain_begin(void *context, struct plain_state *state)
{
   static unsigned said_plain, said_skip;
   GLint program = 0, texture0 = 0, texture1 = 0, rows = 0;
   bool use_table;

   if (!plain_on)
      return false;
   jackpotlte_gl_use(context);
   load();
   if (!procs_for_plain())
      return false;
   gl.get_integerv(GL_CURRENT_PROGRAM, &program);
   if (program != 0)
      return false;

   gl.get_integerv(GL_ACTIVE_TEXTURE, &state->active);
   gl.active_texture(GL_TEXTURE0 + 1);
   gl.get_integerv(GL_TEXTURE_BINDING_2D, &texture1);
   if (texture1 != 0 && gl.get_tex_level_parameteriv != NULL)
      gl.get_tex_level_parameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &rows);
   gl.active_texture(GL_TEXTURE0);
   gl.get_integerv(GL_TEXTURE_BINDING_2D, &texture0);

   /* no texture, or one that is also what is drawn into: left alone, as
    * before (Scion: such draws painted white or read unwritten memory) */
   if (texture0 == 0 || texture0 == colour_texture()) {
      gl.active_texture((GLenum)state->active);
      if (said_skip++ < 4)
         say("a draw with no program left alone (texture %d)", texture0);
      return false;
   }

   use_table = texture1 != 0 && rows == 1;
   if (use_table && !table.tried)
      make_program(table_fragment, "through a table", &table.program, &table.matrix, &table.tried);
   if (!use_table && !plain.tried)
      make_program(plain_fragment, "as they are", &plain.program, &plain.matrix, &plain.tried);
   if ((use_table ? table.program : plain.program) == 0) {
      gl.use_program(0);
      gl.active_texture((GLenum)state->active);
      return false;
   }

   gl.use_program(use_table ? table.program : plain.program);
   gl.uniform_matrix4fv(use_table ? table.matrix : plain.matrix, 1, 0,
                        ca_matrix != NULL ? ca_matrix : identity);
   for (GLuint i = 0; i < 3; i++) {
      gl.get_vertex_attribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &state->enabled[i]);
      if (!state->enabled[i])
         gl.enable_vertex_attrib_array(i);
   }
   if (said_plain++ < 6)
      say("a draw with no program drawn with ours (%s): texture %d, unit-1 texture %d",
          use_table ? "table" : "plain", texture0, texture1);
   return true;
}

static void
plain_end(struct plain_state *state)
{
   for (GLuint i = 0; i < 3; i++) {
      if (!state->enabled[i])
         gl.disable_vertex_attrib_array(i);
   }
   gl.use_program(0);
   gl.active_texture((GLenum)state->active);
}

static void
my_glDrawArrays(void *context, GLenum mode, GLint first, GLsizei count)
{
   struct plain_state state;
   bool ours = plain_begin(context, &state);

   next_draw_arrays(context, mode, first, count);
   if (ours)
      plain_end(&state);
}

static void
my_glDrawElements(void *context, GLenum mode, GLsizei count, GLenum type, const void *indices)
{
   struct plain_state state;
   bool ours = plain_begin(context, &state);

   next_draw_elements(context, mode, count, type, indices);
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
      say("ca fixes: glsl rewrite %s, no-program draws %s", glsl_on ? "on" : "off",
          plain_on ? "on" : "off");
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
