/* su_video.c -- Handheld.PlayFullScreenMovie: the logos, the intro and the
 * outro.
 *
 * Unity 5.3 hands a full-screen movie to UnityPlayer.showVideoPlayer(file,
 * bgColor, controlMode, scalingMode, isURL, offset, length), which on Android
 * pauses the player, plays the file in a MediaPlayer view over it and resumes
 * the player when the movie is over. The game's movies are in its expansion
 * file, stored: the engine gives the OBB's path with the entry's offset and
 * length. Here the JNI call only notes the movie (jni_android.c); the frame
 * loop (dcr_boot.c) then pauses the engine as the Java side did, plays it
 * (su_video_play) and resumes it.
 *
 * FFmpeg (ffmpeg32 with the mov demuxer and the H.264 and AAC decoders)
 * decodes on a thread of its own; the pictures go to the GPU as three planes
 * (Y, U, V) and a small program turns them into colour, so the CPU only
 * decodes. The sound is mixed into the audio output by its pump (dcr_audio.c,
 * which plays only this while a movie runs). Pictures follow a clock started
 * with the first one; all of the sound is decoded first, and each audio
 * buffer is mixed from where the clock says it will be heard. Taken over from
 * the Assassin's Creed port (ac_video.c), itself from aks796's pvz_touch_nx
 * (pvz_video.c, MIT).
 *
 * Skipping: the control mode Unity gives (FullScreenMovieControlMode): 3,
 * Hidden, cannot be skipped (the outro); any other can, with a button or a
 * touch. Compiled with -fno-short-enums, as FFmpeg is. MIT.
 */
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "dcr_path.h"
#include "dcr_time.h"
#include "gl_layer.h"
#include "rt_applet.h"
#include "util.h"

static char g_req_path[512];
static int g_req_pending, g_req_mode;
static uint32_t g_req_bg;
static int64_t g_req_off, g_req_len;

/* UnityPlayer.showVideoPlayer, from the engine's thread */
void su_video_request(const char *file, int bg, int mode, int url, int64_t offset, int64_t length) {
  snprintf(g_req_path, sizeof g_req_path, "%s", file ? file : "");
  g_req_bg = (uint32_t)bg;
  g_req_mode = mode;
  g_req_off = offset;
  g_req_len = length;
  g_req_pending = !url;
  debugPrintf("[video] the game asks for %s (offset %lld, %lld bytes, control mode %d%s)\n", g_req_path,
              (long long)offset, (long long)length, mode, url ? ", a URL: not played" : "");
}

int su_video_pending(void) { return g_req_pending; }

#if !DCR_VIDEO /* built without FFmpeg: the movies are skipped */
void su_video_play(void *dpy, void *surf, void *ctx) {
  (void)dpy, (void)surf, (void)ctx;
  g_req_pending = 0;
}
int su_video_audio_active(void) { return 0; }
void su_video_mix(int16_t *out, int frames, int out_rate) { (void)out, (void)frames, (void)out_rate; }
#else

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

void dcr_window_size(int *w, int *h);
void dcr_boost_hold(int on);
void dcr_audio_movie(int on); /* dcr_audio.c */
EGLBoolean b_eglSwapBuffers(EGLDisplay d, EGLSurface s);
EGLBoolean b_eglMakeCurrent(EGLDisplay d, EGLSurface dr, EGLSurface rd, EGLContext c);

/* ffmpeg32's H.264 without HEVC calls this (its README); nothing fills the
 * film grain parameters without HEVC, so there is nothing to free */
void ff_aom_uninit_film_grain_params(void *p) { (void)p; }

#define NSLOT 4 /* pictures decoded ahead */
#define AAC_PRIMING 1024

static struct {
  Mutex lock;
  int open, playing;
  volatile int stop, skip, eof;
  uint8_t *file;
  size_t file_len;
  int64_t file_pos;
  AVFormatContext *fmt;
  AVIOContext *io;
  AVCodecContext *vdec, *adec;
  int vs, as;
  double vtb;
  Thread thread;
  int thread_on;
  int w, h;
  uint8_t *yuv[NSLOT]; /* Y (w*h), then U and V (w/2 * h/2 each) */
  double pts[NSLOT];
  volatile int ready[NSLOT];
  int shown;
  double shown_pts;
  int started;
  u64 t0; /* ns of dcr_monotonic_ns() at the movie's time 0 */
  int16_t *pcm;
  size_t pcm_cap, pcm_frames;
  int arate;
  double atb, apts0, apos;
  int amixed, resyncs;
  volatile int audio_go;
} V = {.vs = -1, .as = -1, .shown = -1};

/* ------------------------------------------------------------ the file */
static int io_read(void *opaque, uint8_t *buf, int n) {
  (void)opaque;
  const int64_t left = (int64_t)V.file_len - V.file_pos;
  if (left <= 0)
    return AVERROR_EOF;
  if (n > left)
    n = (int)left;
  memcpy(buf, V.file + V.file_pos, (size_t)n);
  V.file_pos += n;
  return n;
}

static int64_t io_seek(void *opaque, int64_t off, int whence) {
  (void)opaque;
  if (whence == AVSEEK_SIZE)
    return (int64_t)V.file_len;
  whence &= ~AVSEEK_FORCE;
  int64_t p = whence == SEEK_SET ? off : whence == SEEK_CUR ? V.file_pos + off : (int64_t)V.file_len + off;
  if (p < 0 || p > (int64_t)V.file_len)
    return -1;
  V.file_pos = p;
  return p;
}

/* The movie's bytes: [offset, offset + length) of the file the engine named
 * (the OBB), or the whole file when no length is given. */
static uint8_t *read_movie(const char *guest, int64_t off, int64_t len, size_t *out_len) {
  char real[DCR_PATH_MAX];
  const char *host = dcr_translate_path(guest, real, sizeof real);
  FILE *f = fopen(host, "rb");
  if (!f) {
    debugPrintf("[video] %s: cannot open %s\n", guest, host);
    return NULL;
  }
  if (len <= 0) {
    fseek(f, 0, SEEK_END);
    len = ftell(f) - off;
  }
  uint8_t *p = len > 0 && len < (256 << 20) ? malloc((size_t)len) : NULL;
  if (p && (fseek(f, (long)off, SEEK_SET) != 0 || fread(p, 1, (size_t)len, f) != (size_t)len)) {
    free(p);
    p = NULL;
  }
  fclose(f);
  *out_len = p ? (size_t)len : 0;
  if (!p)
    debugPrintf("[video] %s: cannot read %lld bytes at %lld\n", host, (long long)len, (long long)off);
  return p;
}

/* ------------------------------------------------------------ pictures */
static void put_picture(const AVFrame *f) {
  if ((f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P) || f->width != V.w ||
      f->height != V.h)
    return;
  int slot = -1;
  while (!V.stop) {
    mutexLock(&V.lock);
    for (int i = 0; i < NSLOT && slot < 0; i++)
      if (!V.ready[i] && i != V.shown)
        slot = i;
    mutexUnlock(&V.lock);
    if (slot >= 0)
      break;
    svcSleepThread(3000000ll);
  }
  if (slot < 0)
    return;
  uint8_t *d = V.yuv[slot];
  const int cw = V.w / 2, ch = V.h / 2;
  for (int y = 0; y < V.h; y++)
    memcpy(d + (size_t)y * V.w, f->data[0] + (size_t)y * f->linesize[0], (size_t)V.w);
  d += (size_t)V.w * V.h;
  for (int p = 1; p <= 2; p++, d += (size_t)cw * ch)
    for (int y = 0; y < ch; y++)
      memcpy(d + (size_t)y * cw, f->data[p] + (size_t)y * f->linesize[p], (size_t)cw);
  const int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
  mutexLock(&V.lock);
  V.pts[slot] = ts == AV_NOPTS_VALUE ? 0 : (double)ts * V.vtb;
  V.ready[slot] = 1;
  mutexUnlock(&V.lock);
}

/* ------------------------------------------------------------ sound */
static void put_sound(const AVFrame *f) {
  if (!V.pcm || f->nb_samples <= 0)
    return;
  const int ch = f->ch_layout.nb_channels;
  size_t n = (size_t)f->nb_samples;
  if (!V.pcm_frames) {
    const int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
    V.apts0 = ts == AV_NOPTS_VALUE ? 0 : (double)ts * V.atb;
  }
  mutexLock(&V.lock);
  if (V.pcm_frames + n > V.pcm_cap)
    n = V.pcm_cap - V.pcm_frames;
  int16_t *d = V.pcm + V.pcm_frames * 2;
  mutexUnlock(&V.lock);
  for (size_t i = 0; i < n; i++)
    for (int c = 0; c < 2; c++) {
      const int k = c < ch ? c : 0;
      float s;
      switch (f->format) {
      case AV_SAMPLE_FMT_FLTP: s = ((const float *)f->extended_data[k])[i]; break;
      case AV_SAMPLE_FMT_FLT: s = ((const float *)f->extended_data[0])[i * ch + k]; break;
      case AV_SAMPLE_FMT_S16P: s = ((const int16_t *)f->extended_data[k])[i] / 32768.0f; break;
      case AV_SAMPLE_FMT_S16: s = ((const int16_t *)f->extended_data[0])[i * ch + k] / 32768.0f; break;
      default: s = 0; break;
      }
      int v = (int)(s * 32767.0f);
      d[i * 2 + c] = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
    }
  mutexLock(&V.lock);
  V.pcm_frames += n;
  mutexUnlock(&V.lock);
}

/* The movie's clock: dcr_monotonic_ns, which leaves out the time the process
 * was held (HOME, sleep), and play() moves its start on by any time spent
 * without focus -- so the movie goes on where it was rather than racing to
 * catch up (hardware 2026-10-30). */
static double now_s(void) { return (double)(dcr_monotonic_ns() - V.t0) / 1e9; }

int su_video_audio_active(void) { return V.playing; }

/* Into the output (48 kHz stereo s16), from dcr_audio.c's pump while a movie
 * plays: the sound due when this buffer is heard (after the 2.5 buffers
 * queued before it, the picture a frame or two after its draw). */
void su_video_mix(int16_t *out, int frames, int out_rate) {
  if (!V.audio_go || !V.pcm || V.arate <= 0)
    return;
  mutexLock(&V.lock);
  if (!V.audio_go || !V.pcm) {
    mutexUnlock(&V.lock);
    return;
  }
  const double step = (double)V.arate / (double)out_rate;
  const double ahead = 2.5 * frames / out_rate - 1.5 / 60.0;
  const double want = (now_s() + ahead - V.apts0) * V.arate;
  if (!V.amixed || fabs(V.apos - want) > 0.06 * V.arate) {
    if (V.amixed && V.resyncs++ < 8)
      debugPrintf("[video] sound %.0f ms off the pictures: back in step\n", (V.apos - want) * 1000.0 / V.arate);
    V.apos = want;
    V.amixed = 1;
  }
  const double have = (double)V.pcm_frames;
  for (int i = 0; i < frames; i++, V.apos += step) {
    if (V.apos < 0 || V.apos + 1 >= have)
      continue;
    const int i0 = (int)V.apos;
    const double t = V.apos - i0;
    for (int c = 0; c < 2; c++) {
      const double s = V.pcm[i0 * 2 + c] * (1 - t) + V.pcm[(i0 + 1) * 2 + c] * t;
      int v = out[i * 2 + c] + (int)s;
      out[i * 2 + c] = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
    }
  }
  mutexUnlock(&V.lock);
}

/* ------------------------------------------------------------ decoding */
static void only_stream(int keep) {
  for (unsigned i = 0; i < V.fmt->nb_streams; i++)
    V.fmt->streams[i]->discard = (int)i == keep ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
}

static int decode_sound(AVPacket *pkt, AVFrame *fr) {
  const u64 t = armGetSystemTick();
  only_stream(V.as);
  while (!V.stop && av_read_frame(V.fmt, pkt) >= 0) {
    if (pkt->stream_index == V.as && avcodec_send_packet(V.adec, pkt) >= 0)
      while (avcodec_receive_frame(V.adec, fr) == 0)
        put_sound(fr);
    av_packet_unref(pkt);
  }
  avcodec_send_packet(V.adec, NULL);
  while (avcodec_receive_frame(V.adec, fr) == 0)
    put_sound(fr);
  if (V.adec->codec_id == AV_CODEC_ID_AAC)
    V.apts0 -= (double)AAC_PRIMING / V.arate;
  only_stream(V.vs);
  const int r = av_seek_frame(V.fmt, V.vs, 0, AVSEEK_FLAG_BACKWARD);
  debugPrintf("[video] the sound first: %.2f s in %.0f ms; back to the start: %s\n", (double)V.pcm_frames / V.arate,
              (double)armTicksToNs(armGetSystemTick() - t) / 1e6, r >= 0 ? "ok" : av_err2str(r));
  return r >= 0 ? 0 : -1;
}

static void decode_thread(void *arg) {
  (void)arg;
  AVPacket *pkt = av_packet_alloc();
  AVFrame *fr = av_frame_alloc();
  int got_video = 0;
  const u64 t0 = armGetSystemTick();
  if (pkt && fr && V.adec && decode_sound(pkt, fr) < 0)
    V.stop = 1;
  while (pkt && fr && !V.stop) {
    const int r = av_read_frame(V.fmt, pkt);
    if (r < 0) {
      avcodec_send_packet(V.vdec, NULL);
      while (!V.stop && avcodec_receive_frame(V.vdec, fr) == 0)
        put_picture(fr), got_video++;
      break;
    }
    if (pkt->stream_index == V.vs && avcodec_send_packet(V.vdec, pkt) >= 0)
      while (!V.stop && avcodec_receive_frame(V.vdec, fr) == 0)
        put_picture(fr), got_video++;
    av_packet_unref(pkt);
  }
  av_frame_free(&fr);
  av_packet_free(&pkt);
  debugPrintf("[video] decoded %d pictures in %.1f s, %u sound frames%s\n", got_video,
              (double)armTicksToNs(armGetSystemTick() - t0) / 1e9, (unsigned)V.pcm_frames,
              V.stop ? " (stopped)" : "");
  V.eof = 1;
}

static void close_all(void) {
  V.stop = 1;
  if (V.thread_on) {
    threadWaitForExit(&V.thread);
    threadClose(&V.thread);
    V.thread_on = 0;
  }
  mutexLock(&V.lock);
  V.audio_go = 0;
  mutexUnlock(&V.lock);
  avcodec_free_context(&V.vdec);
  avcodec_free_context(&V.adec);
  if (V.fmt)
    avformat_close_input(&V.fmt);
  if (V.io) {
    av_freep(&V.io->buffer);
    avio_context_free(&V.io);
  }
  for (int i = 0; i < NSLOT; i++) {
    free(V.yuv[i]);
    V.yuv[i] = NULL;
    V.ready[i] = 0;
  }
  mutexLock(&V.lock);
  free(V.pcm);
  V.pcm = NULL;
  mutexUnlock(&V.lock);
  free(V.file);
  V.file = NULL;
  V.open = V.playing = 0;
  V.shown = -1;
  V.vs = V.as = -1;
}

static AVCodecContext *open_decoder(AVStream *st) {
  const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
  AVCodecContext *ctx = c ? avcodec_alloc_context3(c) : NULL;
  if (!ctx || avcodec_parameters_to_context(ctx, st->codecpar) < 0) {
    avcodec_free_context(&ctx);
    return NULL;
  }
  /* one thread for 720p H.264: the loop filter on reference pictures only */
  if (c->id == AV_CODEC_ID_H264)
    ctx->skip_loop_filter = AVDISCARD_NONREF;
  if (avcodec_open2(ctx, c, NULL) < 0) {
    avcodec_free_context(&ctx);
    return NULL;
  }
  return ctx;
}

static int open_movie(void) {
  if (V.open)
    close_all();
  mutexInit(&V.lock);
  V.stop = V.skip = V.eof = 0;
  V.started = 0;
  V.apos = V.apts0 = 0;
  V.amixed = V.resyncs = 0;
  V.pcm_frames = 0;
  V.file = read_movie(g_req_path, g_req_off, g_req_len, &V.file_len);
  if (!V.file)
    return 0;
  V.file_pos = 0;
  av_log_set_level(AV_LOG_ERROR);
  unsigned char *buf = av_malloc(32768);
  V.io = buf ? avio_alloc_context(buf, 32768, 0, NULL, io_read, NULL, io_seek) : NULL;
  V.fmt = avformat_alloc_context();
  if (!V.io || !V.fmt) {
    close_all();
    return 0;
  }
  V.fmt->pb = V.io;
  if (avformat_open_input(&V.fmt, NULL, NULL, NULL) < 0 || avformat_find_stream_info(V.fmt, NULL) < 0) {
    debugPrintf("[video] %s: not a video FFmpeg reads here\n", g_req_path);
    close_all();
    return 0;
  }
  V.vs = av_find_best_stream(V.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
  V.as = av_find_best_stream(V.fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
  if (V.vs >= 0)
    V.vdec = open_decoder(V.fmt->streams[V.vs]);
  if (V.as >= 0)
    V.adec = open_decoder(V.fmt->streams[V.as]);
  if (!V.vdec || (V.vdec->width & 1) || (V.vdec->height & 1)) {
    debugPrintf("[video] %s: no picture decoder for it\n", g_req_path);
    close_all();
    return 0;
  }
  V.w = V.vdec->width;
  V.h = V.vdec->height;
  V.vtb = av_q2d(V.fmt->streams[V.vs]->time_base);
  for (int i = 0; i < NSLOT; i++)
    if (!(V.yuv[i] = memalign(64, (size_t)V.w * V.h * 3 / 2))) {
      close_all();
      return 0;
    }
  if (V.adec) {
    V.arate = V.adec->sample_rate;
    V.atb = av_q2d(V.fmt->streams[V.as]->time_base);
    const double secs = V.fmt->duration > 0 ? (double)V.fmt->duration / AV_TIME_BASE : 120.0;
    V.pcm_cap = (size_t)((secs + 2.0) * V.arate);
    V.pcm = malloc(V.pcm_cap * 4);
  }
  V.open = 1;
  debugPrintf("[video] %s: %dx%d %s, %s, %.1f s\n", g_req_path, V.w, V.h, avcodec_get_name(V.vdec->codec_id),
              V.adec ? avcodec_get_name(V.adec->codec_id) : "no sound",
              V.fmt->duration > 0 ? (double)V.fmt->duration / AV_TIME_BASE : 0.0);
  return 1;
}

/* ------------------------------------------------------------ drawing */
static struct {
  int ok; /* 0 not set up, 1 ready, -1 no */
  GLuint prog, tex[3], vbo, vao;
  GLint a_pos, u_tex[3];
  int tw, th;
} G;

static const char *const k_vs =
    "attribute vec2 p;\n"
    "varying vec2 uv;\n"
    "void main() { uv = vec2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5); gl_Position = vec4(p, 0.0, 1.0); }\n";
/* BT.709, video range (the movies are HD) */
static const char *const k_fs =
    "precision mediump float;\n"
    "varying vec2 uv;\n"
    "uniform sampler2D ty, tu, tv;\n"
    "void main() {\n"
    "  float y = (texture2D(ty, uv).r - 0.0625) * 1.164;\n"
    "  float u = texture2D(tu, uv).r - 0.5, v = texture2D(tv, uv).r - 0.5;\n"
    "  gl_FragColor = vec4(y + 1.793 * v, y - 0.213 * u - 0.533 * v, y + 2.112 * u, 1.0);\n"
    "}\n";

static GLuint shader(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  return ok ? s : 0;
}

static int gl_setup(void) {
  GLuint vs = shader(GL_VERTEX_SHADER, k_vs), fs = shader(GL_FRAGMENT_SHADER, k_fs);
  if (!vs || !fs) {
    debugPrintf("[video] the picture program does not compile: no pictures\n");
    return -1;
  }
  G.prog = glCreateProgram();
  glAttachShader(G.prog, vs);
  glAttachShader(G.prog, fs);
  glBindAttribLocation(G.prog, 0, "p");
  glLinkProgram(G.prog);
  GLint ok = 0;
  glGetProgramiv(G.prog, GL_LINK_STATUS, &ok);
  glDeleteShader(vs);
  glDeleteShader(fs);
  if (!ok) {
    debugPrintf("[video] the picture program does not link: no pictures\n");
    return -1;
  }
  static const char *const names[3] = {"ty", "tu", "tv"};
  for (int i = 0; i < 3; i++)
    G.u_tex[i] = glGetUniformLocation(G.prog, names[i]);
  glGenTextures(3, G.tex);
  for (int i = 0; i < 3; i++) {
    glBindTexture(GL_TEXTURE_2D, G.tex[i]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  static const GLfloat quad[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
  /* the quad's attribute in a vertex array of its own: the engine's (and its
   * idea of attribute 0) stay as they were */
  glGenVertexArrays(1, &G.vao);
  glBindVertexArray(G.vao);
  glGenBuffers(1, &G.vbo);
  glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
  glBindVertexArray(0);
  return 1;
}

static void upload(int slot) {
  const uint8_t *p = V.yuv[slot];
  const int cw = V.w / 2, ch = V.h / 2;
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  const int fresh = G.tw != V.w || G.th != V.h;
  for (int i = 0; i < 3; i++) {
    const int w = i ? cw : V.w, h = i ? ch : V.h;
    glActiveTexture(GL_TEXTURE0 + i);
    glBindTexture(GL_TEXTURE_2D, G.tex[i]);
    if (fresh)
      glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w, h, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, p);
    else
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_LUMINANCE, GL_UNSIGNED_BYTE, p);
    p += (size_t)w * h;
  }
  G.tw = V.w, G.th = V.h;
  glActiveTexture(GL_TEXTURE0);
}

/* the picture due, fitted to the window over the background colour */
static void draw(int vw, int vh) {
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_BLEND);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glViewport(0, 0, vw, vh);
  glClearColor((float)((g_req_bg >> 16) & 0xff) / 255.0f, (float)((g_req_bg >> 8) & 0xff) / 255.0f,
               (float)(g_req_bg & 0xff) / 255.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  if (V.shown < 0)
    return;
  /* aspect kept: the movie's width or height fills the window */
  int dw = vw, dh = (int)((int64_t)vw * V.h / V.w);
  if (dh > vh)
    dh = vh, dw = (int)((int64_t)vh * V.w / V.h);
  glViewport((vw - dw) / 2, (vh - dh) / 2, dw, dh);
  glUseProgram(G.prog);
  for (int i = 0; i < 3; i++) {
    glActiveTexture(GL_TEXTURE0 + i);
    glBindTexture(GL_TEXTURE_2D, G.tex[i]);
    glUniform1i(G.u_tex[i], i);
  }
  glActiveTexture(GL_TEXTURE0);
  glBindVertexArray(G.vao);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glViewport(0, 0, vw, vh);
}

/* The engine's GL state, as it left it when it paused: put back exactly after
 * the movie. Unity keeps its own copy of the state and does not ask GL again,
 * so anything left changed (program, textures, buffers, viewport, tests) made
 * its every later frame black (hardware 2026-10-30). */
static const GLenum k_caps[] = {GL_DEPTH_TEST, GL_STENCIL_TEST, GL_SCISSOR_TEST, GL_CULL_FACE, GL_BLEND, GL_DITHER};
#define NCAPS (sizeof k_caps / sizeof k_caps[0])
static struct {
  GLint prog, active, tex[3], abuf, vao, fbo, rbo, vp[4], unpack;
  GLfloat clear[4];
  GLboolean mask[4], caps[NCAPS];
} S;

static void gl_save(void) {
  glGetIntegerv(GL_CURRENT_PROGRAM, &S.prog);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &S.active);
  for (int i = 0; i < 3; i++) {
    glActiveTexture(GL_TEXTURE0 + i);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &S.tex[i]);
  }
  glActiveTexture((GLenum)S.active);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &S.abuf);
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &S.vao);
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &S.fbo);
  glGetIntegerv(GL_RENDERBUFFER_BINDING, &S.rbo);
  glGetIntegerv(GL_VIEWPORT, S.vp);
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &S.unpack);
  glGetFloatv(GL_COLOR_CLEAR_VALUE, S.clear);
  glGetBooleanv(GL_COLOR_WRITEMASK, S.mask);
  for (unsigned i = 0; i < NCAPS; i++)
    S.caps[i] = glIsEnabled(k_caps[i]);
}

static void gl_restore(void) {
  for (unsigned i = 0; i < NCAPS; i++)
    S.caps[i] ? glEnable(k_caps[i]) : glDisable(k_caps[i]);
  glColorMask(S.mask[0], S.mask[1], S.mask[2], S.mask[3]);
  glClearColor(S.clear[0], S.clear[1], S.clear[2], S.clear[3]);
  glPixelStorei(GL_UNPACK_ALIGNMENT, S.unpack);
  glViewport(S.vp[0], S.vp[1], S.vp[2], S.vp[3]);
  glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)S.rbo);
  glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)S.fbo);
  glBindVertexArray((GLuint)S.vao);
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)S.abuf);
  for (int i = 0; i < 3; i++) {
    glActiveTexture(GL_TEXTURE0 + i);
    glBindTexture(GL_TEXTURE_2D, (GLuint)S.tex[i]);
  }
  glActiveTexture((GLenum)S.active);
  glUseProgram((GLuint)S.prog);
  while (glGetError() != GL_NO_ERROR)
    ;
}

/* ------------------------------------------------------------ playing */
/* A press or a new touch since the last frame: the player's "skip". */
static int skip_pressed(void) {
  static PadState pad;
  static int init;
  static s32 touching;
  if (!init) {
    init = 1;
    padInitializeWithMask(&pad, 0x1000000FFull); /* players 1-8 and handheld */
  }
  padUpdate(&pad);
  int hit = (padGetButtonsDown(&pad) & (HidNpadButton_A | HidNpadButton_B | HidNpadButton_X | HidNpadButton_Y |
                                        HidNpadButton_Plus | HidNpadButton_Minus | HidNpadButton_ZR)) != 0;
  HidTouchScreenState ts = {0};
  if (hidGetTouchScreenStates(&ts, 1)) {
    if (ts.count > touching)
      hit = 1;
    touching = ts.count;
  }
  return hit;
}

static void play(EGLDisplay dpy, EGLSurface surf);

/* Plays the movie the game asked for, on the engine's thread, until it ends
 * (or is skipped); then returns. The engine, paused, has let go of its GL
 * context (hardware 2026-10-30: eglMakeCurrent(0, 0) in pause): the movie
 * takes the engine's context and surface (dpy, surf, ctx: what was current
 * before the pause) and lets go of them again, for the engine's resume. */
void su_video_play(void *dpy, void *surf, void *ctx) {
  if (!g_req_pending)
    return;
  g_req_pending = 0;
  if (!dpy || !surf || !ctx || !b_eglMakeCurrent(dpy, surf, surf, ctx)) {
    debugPrintf("[video] no GL context to draw with (display %p, surface %p, context %p): not played\n", dpy, surf,
                ctx);
    return;
  }
  gl_save();
  play(dpy, surf);
  gl_restore();
  b_eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

static void play(EGLDisplay dpy, EGLSurface surf) {
  if (!open_movie())
    return;
  if (!G.ok)
    G.ok = gl_setup();
  if (G.ok < 0) {
    close_all();
    return;
  }
  const int skippable = g_req_mode != 3; /* FullScreenMovieControlMode.Hidden */
  V.stop = 0;
  if (R_FAILED(threadCreate(&V.thread, decode_thread, NULL, NULL, 0x40000, 0x2C, -2)) ||
      R_FAILED(threadStart(&V.thread))) {
    debugPrintf("[video] no thread for the decoder\n");
    close_all();
    return;
  }
  V.thread_on = 1;
  V.playing = 1;
  dcr_boost_hold(1);
  dcr_audio_movie(1);
  skip_pressed(); /* the press that started it does not count */
  const char *why = "the end";
  unsigned frames = 0, late = 0;
  while (appletMainLoop()) {
    rt_applet_poll();
    if (!rt_focused()) {
      /* the HOME menu or sleep: hold the movie (pictures and sound) until
       * the game has the focus again */
      const uint64_t held = dcr_monotonic_ns();
      mutexLock(&V.lock);
      const int sound = V.audio_go;
      V.audio_go = 0;
      mutexUnlock(&V.lock);
      while (appletMainLoop() && !rt_focused()) {
        svcSleepThread(50000000ll);
        rt_applet_poll();
      }
      mutexLock(&V.lock);
      V.t0 += dcr_monotonic_ns() - held;
      V.amixed = 0; /* the sound starts again from the clock */
      V.audio_go = sound;
      mutexUnlock(&V.lock);
      skip_pressed(); /* nothing pressed meanwhile counts */
      if (!rt_focused())
        break; /* the program is closing */
      continue;
    }
    if (skippable && skip_pressed()) {
      why = "skipped";
      break;
    }
    /* the picture due: the newest ready whose time has come */
    mutexLock(&V.lock);
    int due = -1;
    if (!V.started) {
      double first = 1e9;
      for (int i = 0; i < NSLOT; i++)
        if (V.ready[i] && V.pts[i] < first)
          first = V.pts[i], due = i;
      if (due >= 0) {
        V.started = 1;
        V.t0 = dcr_monotonic_ns() - (uint64_t)(first * 1e9);
        V.audio_go = 1;
      }
    } else {
      const double t = now_s();
      for (int i = 0; i < NSLOT; i++)
        if (V.ready[i] && V.pts[i] <= t && (due < 0 || V.pts[i] > V.pts[due]))
          due = i;
    }
    int any_left = 0;
    if (due >= 0) {
      for (int i = 0; i < NSLOT; i++)
        if (V.ready[i] && i != due && V.pts[i] < V.pts[due])
          V.ready[i] = 0, late++;
      if (V.shown >= 0 && V.shown != due)
        V.ready[V.shown] = 0;
    }
    for (int i = 0; i < NSLOT; i++)
      any_left |= V.ready[i] && i != due && i != V.shown;
    const int nothing = !V.started && due < 0 && V.eof;
    mutexUnlock(&V.lock);
    if (nothing) {
      why = "no pictures";
      break;
    }
    if (due >= 0 && due != V.shown) {
      upload(due);
      V.shown = due;
      V.shown_pts = V.pts[due];
    }
    int vw, vh;
    dcr_window_size(&vw, &vh);
    draw(vw, vh);
    b_eglSwapBuffers(dpy, surf);
    frames++;
    if (V.eof && !any_left && V.started && now_s() > V.shown_pts + 0.1)
      break;
  }
  debugPrintf("[video] %s: ended (%s), %u frames drawn, %u pictures dropped (late)\n", g_req_path, why, frames,
              late);
  dcr_audio_movie(0);
  close_all();
  dcr_boost_hold(0);
}

#endif /* DCR_VIDEO */
