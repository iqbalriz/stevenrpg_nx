/* su_cursor.c -- the pointer cursor: an arrow drawn over the game's frame
 * (from the Costume Quest port, costumequest_nx's cq_cursor.c).
 *
 * Steven Universe: Attack the Light is made for a finger: menus, the battle's
 * character / action / target choices, chests and switches on the map. The
 * right stick moves this cursor and A taps with it (dcr_input.c turns that
 * into a touch). It shows when the stick moves and fades a few seconds after
 * the last move or tap, so it is never in the way.
 *
 * The runtime calls dcr_present_hook just before each swap, on the engine's
 * thread with its OpenGL ES context current. The game's own state (program,
 * vertex array and buffer, blending and the tests, the framebuffer) is read
 * first and put back after: the arrow has its own program and vertex array. MIT.
 */
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <switch.h>

#include "su_cursor.h"
#include "gl_layer.h"
#include "util.h"

#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_FLOAT 0x1406
#define GL_TRIANGLES 0x0004
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#define GL_ARRAY_BUFFER_BINDING 0x8894
#define GL_FRAMEBUFFER 0x8D40
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_BLEND 0x0BE2
#define GL_DEPTH_TEST 0x0B71
#define GL_CULL_FACE 0x0B44
#define GL_SCISSOR_TEST 0x0C11
#define GL_STENCIL_TEST 0x0B90
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_BLEND_SRC_RGB 0x80C9
#define GL_BLEND_DST_RGB 0x80C8
#define GL_BLEND_SRC_ALPHA 0x80CB
#define GL_BLEND_DST_ALPHA 0x80CA
#define GL_COLOR_WRITEMASK 0x0C23
#define GL_VIEWPORT 0x0BA2

#define HIDE_NS 4000000000ull

/* what the game thread tells the render thread (floats and ints: tearing is harmless) */
static volatile float g_x, g_y;       /* pixels of the surface */
static volatile int g_w, g_h, g_down; /* the surface's size, 1 while a tap is held */
static volatile u64 g_seen;           /* tick of the last move or tap; 0: never */
static volatile int g_enabled;

void su_cursor_set(float x, float y, int w, int h, int down, int touched) {
  g_x = x, g_y = y, g_w = w, g_h = h, g_down = down;
  if (touched)
    g_seen = armGetSystemTick();
  g_enabled = 1;
}

/* ------------------------------------------------------------------ GL */
static struct {
  void (*GetIntegerv)(unsigned, int *);
  void (*GetBooleanv)(unsigned, unsigned char *);
  unsigned char (*IsEnabled)(unsigned);
  void (*Enable)(unsigned);
  void (*Disable)(unsigned);
  unsigned (*CreateShader)(unsigned);
  void (*ShaderSource)(unsigned, int, const char *const *, const int *);
  void (*CompileShader)(unsigned);
  void (*GetShaderiv)(unsigned, unsigned, int *);
  unsigned (*CreateProgram)(void);
  void (*AttachShader)(unsigned, unsigned);
  void (*BindAttribLocation)(unsigned, unsigned, const char *);
  void (*LinkProgram)(unsigned);
  void (*GetProgramiv)(unsigned, unsigned, int *);
  void (*DeleteShader)(unsigned);
  int (*GetUniformLocation)(unsigned, const char *);
  void (*UseProgram)(unsigned);
  void (*Uniform2f)(int, float, float);
  void (*Uniform4f)(int, float, float, float, float);
  void (*GenBuffers)(int, unsigned *);
  void (*BindBuffer)(unsigned, unsigned);
  void (*BufferData)(unsigned, long, const void *, unsigned);
  void (*GenVertexArrays)(int, unsigned *);
  void (*BindVertexArray)(unsigned);
  void (*EnableVertexAttribArray)(unsigned);
  void (*VertexAttribPointer)(unsigned, int, unsigned, unsigned char, int, const void *);
  void (*DrawArrays)(unsigned, int, int);
  void (*BlendFuncSeparate)(unsigned, unsigned, unsigned, unsigned);
  void (*BindFramebuffer)(unsigned, unsigned);
  int ok;
} gl;

static unsigned g_prog, g_vao, g_vbo;
static int g_u_res, g_u_col;

static const char *const k_vs =
    "attribute vec2 p;\n"
    "uniform vec2 res;\n"
    "void main() { gl_Position = vec4(p.x / res.x * 2.0 - 1.0, 1.0 - p.y / res.y * 2.0, 0.0, 1.0); }\n";
static const char *const k_fs =
    "precision mediump float;\n"
    "uniform vec4 col;\n"
    "void main() { gl_FragColor = col; }\n";

static int gl_setup(void) {
#define L(n) gl.n = (void *)dcr_gl_lookup("gl" #n)
  L(GetIntegerv); L(GetBooleanv); L(IsEnabled); L(Enable); L(Disable); L(CreateShader); L(ShaderSource);
  L(CompileShader); L(GetShaderiv); L(CreateProgram); L(AttachShader); L(BindAttribLocation); L(LinkProgram);
  L(GetProgramiv); L(DeleteShader); L(GetUniformLocation); L(UseProgram); L(Uniform2f); L(Uniform4f);
  L(GenBuffers); L(BindBuffer); L(BufferData); L(GenVertexArrays); L(BindVertexArray); L(EnableVertexAttribArray);
  L(VertexAttribPointer); L(DrawArrays); L(BlendFuncSeparate); L(BindFramebuffer);
#undef L
  /* an OpenGL ES 2 context (Unity 5.3 may ask for one): the OES names */
  if (!gl.GenVertexArrays)
    gl.GenVertexArrays = (void *)dcr_gl_lookup("glGenVertexArraysOES");
  if (!gl.BindVertexArray)
    gl.BindVertexArray = (void *)dcr_gl_lookup("glBindVertexArrayOES");
  if (!gl.GetIntegerv || !gl.CreateShader || !gl.GenVertexArrays || !gl.BindVertexArray || !gl.BlendFuncSeparate ||
      !gl.BindFramebuffer || !gl.UseProgram || !gl.DrawArrays) {
    debugPrintf("[cursor] a GL function is missing: no cursor\n");
    return -1;
  }
  unsigned sh[2];
  const char *src[2] = {k_vs, k_fs};
  const unsigned type[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
  g_prog = gl.CreateProgram();
  for (int i = 0; i < 2; i++) {
    sh[i] = gl.CreateShader(type[i]);
    gl.ShaderSource(sh[i], 1, &src[i], NULL);
    gl.CompileShader(sh[i]);
    int ok = 0;
    gl.GetShaderiv(sh[i], GL_COMPILE_STATUS, &ok);
    if (!ok) {
      debugPrintf("[cursor] the shader does not compile: no cursor\n");
      return -1;
    }
    gl.AttachShader(g_prog, sh[i]);
  }
  gl.BindAttribLocation(g_prog, 0, "p");
  gl.LinkProgram(g_prog);
  int ok = 0;
  gl.GetProgramiv(g_prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    debugPrintf("[cursor] the program does not link: no cursor\n");
    return -1;
  }
  gl.DeleteShader(sh[0]);
  gl.DeleteShader(sh[1]);
  g_u_res = gl.GetUniformLocation(g_prog, "res");
  g_u_col = gl.GetUniformLocation(g_prog, "col");
  gl.GenVertexArrays(1, &g_vao);
  gl.GenBuffers(1, &g_vbo);
  int old_vao = 0, old_buf = 0;
  gl.GetIntegerv(GL_VERTEX_ARRAY_BINDING, &old_vao);
  gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_buf);
  gl.BindVertexArray(g_vao);
  gl.BindBuffer(GL_ARRAY_BUFFER, g_vbo);
  gl.BufferData(GL_ARRAY_BUFFER, 12 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
  gl.EnableVertexAttribArray(0);
  gl.VertexAttribPointer(0, 2, GL_FLOAT, 0, 0, NULL);
  gl.BindVertexArray((unsigned)old_vao);
  gl.BindBuffer(GL_ARRAY_BUFFER, (unsigned)old_buf);
  return 0;
}

/* an arrow, 22 x 32 pixels at 1280x720: black outline, white face */
static void cursor_draw(void) {
  if (!g_enabled || !g_seen)
    return;
  const u64 now = armGetSystemTick();
  const u64 age = armTicksToNs(now - g_seen);
  if (age >= HIDE_NS)
    return;
  if (!gl.ok) {
    gl.ok = gl_setup() == 0 ? 1 : -1;
  }
  if (gl.ok != 1)
    return;

  /* the game's state, to be put back */
  int prog = 0, vao = 0, buf = 0, fbo = 0, vp[4], bsrc_rgb = 1, bdst_rgb = 0, bsrc_a = 1, bdst_a = 0;
  unsigned char mask[4];
  gl.GetIntegerv(GL_CURRENT_PROGRAM, &prog);
  gl.GetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
  gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &buf);
  gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
  gl.GetIntegerv(GL_BLEND_SRC_RGB, &bsrc_rgb);
  gl.GetIntegerv(GL_BLEND_DST_RGB, &bdst_rgb);
  gl.GetIntegerv(GL_BLEND_SRC_ALPHA, &bsrc_a);
  gl.GetIntegerv(GL_BLEND_DST_ALPHA, &bdst_a);
  gl.GetIntegerv(GL_VIEWPORT, vp);
  gl.GetBooleanv(GL_COLOR_WRITEMASK, mask);
  const unsigned char blend = gl.IsEnabled(GL_BLEND), depth = gl.IsEnabled(GL_DEPTH_TEST),
                      cull = gl.IsEnabled(GL_CULL_FACE), scissor = gl.IsEnabled(GL_SCISSOR_TEST),
                      stencil = gl.IsEnabled(GL_STENCIL_TEST);

  const float w = (float)(g_w ? g_w : 1280), h = (float)(g_h ? g_h : 720);
  const float s = h / 720.0f * (g_down ? 0.85f : 1.0f);
  const float x = g_x, y = g_y;
  const float fade = age > 3000000000ull ? 1.0f - (float)(age - 3000000000ull) / 1e9f : 1.0f;
  const float out[6] = {x, y, x, y + 32 * s, x + 22 * s, y + 23 * s};
  const float in[6] = {x + 2.5f * s, y + 6.5f * s, x + 2.5f * s, y + 25 * s, x + 17 * s, y + 21 * s};

  gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
  gl.Disable(GL_DEPTH_TEST);
  gl.Disable(GL_CULL_FACE);
  gl.Disable(GL_SCISSOR_TEST);
  gl.Disable(GL_STENCIL_TEST);
  gl.Enable(GL_BLEND);
  gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  gl.UseProgram(g_prog);
  gl.Uniform2f(g_u_res, w, h);
  gl.BindVertexArray(g_vao);
  gl.BindBuffer(GL_ARRAY_BUFFER, g_vbo);
  gl.BufferData(GL_ARRAY_BUFFER, sizeof out, out, GL_DYNAMIC_DRAW);
  gl.Uniform4f(g_u_col, 0.0f, 0.0f, 0.0f, 0.9f * fade);
  gl.DrawArrays(GL_TRIANGLES, 0, 3);
  gl.BufferData(GL_ARRAY_BUFFER, sizeof in, in, GL_DYNAMIC_DRAW);
  gl.Uniform4f(g_u_col, 1.0f, 1.0f, 1.0f, 0.95f * fade);
  gl.DrawArrays(GL_TRIANGLES, 0, 3);

  /* put back */
  gl.BindFramebuffer(GL_FRAMEBUFFER, (unsigned)fbo);
  gl.UseProgram((unsigned)prog);
  gl.BindVertexArray((unsigned)vao);
  gl.BindBuffer(GL_ARRAY_BUFFER, (unsigned)buf);
  gl.BlendFuncSeparate((unsigned)bsrc_rgb, (unsigned)bdst_rgb, (unsigned)bsrc_a, (unsigned)bdst_a);
  blend ? gl.Enable(GL_BLEND) : gl.Disable(GL_BLEND);
  depth ? gl.Enable(GL_DEPTH_TEST) : gl.Disable(GL_DEPTH_TEST);
  cull ? gl.Enable(GL_CULL_FACE) : gl.Disable(GL_CULL_FACE);
  scissor ? gl.Enable(GL_SCISSOR_TEST) : gl.Disable(GL_SCISSOR_TEST);
  stencil ? gl.Enable(GL_STENCIL_TEST) : gl.Disable(GL_STENCIL_TEST);
  (void)vp;
  (void)mask;
}

void su_cursor_install(void) { dcr_present_hook = cursor_draw; }
