/* su_ndk_stubs.c -- what Unity 5.3's libunity imports and Unity 5.6's did not.
 *
 * Unity 5.3 still carries the NativeActivity input path (AInputQueue and the
 * AMotionEvent getters): this port gives input through UnityPlayer's
 * nativeInjectEvent (dcr_input.c), as dcr_sea_nx does, so no input queue is
 * ever attached and these answer "nothing". ANativeWindow_lock is the CPU
 * blit path, which a GL player does not take. The wide-character functions
 * newlib has, with bionic's 4-byte wchar_t (devkitARM's too), pass through in
 * tools/imports.cfg; these are the rest. Each logs its first call: one that
 * shows up is a path the engine did take after all. MIT.
 */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <wchar.h>

#include "util.h"

#define FIRST(name)                                                   \
  do {                                                                \
    static int seen_;                                                 \
    if (!seen_++)                                                     \
      debugPrintf("[ndk] %s called (stub: nothing there)\n", name);   \
  } while (0)

/* -------------------------------------------------------- AInputQueue */
void b_AInputQueue_attachLooper(void *q, void *looper, int ident, void *cb, void *data) {
  (void)q, (void)looper, (void)ident, (void)cb, (void)data;
  FIRST("AInputQueue_attachLooper");
}
void b_AInputQueue_detachLooper(void *q) { (void)q; }
int32_t b_AInputQueue_getEvent(void *q, void **out) {
  (void)q;
  if (out)
    *out = 0;
  return -1; /* no event */
}
int32_t b_AInputQueue_preDispatchEvent(void *q, void *ev) {
  (void)q, (void)ev;
  return 0;
}
void b_AInputQueue_finishEvent(void *q, void *ev, int handled) { (void)q, (void)ev, (void)handled; }

/* -------------------------------------------------------- AInputEvent / AMotionEvent */
int32_t b_AInputEvent_getSource(const void *ev) {
  (void)ev;
  FIRST("AInputEvent_getSource");
  return 0;
}

int32_t b_AMotionEvent_getAction(const void *ev) {
  (void)ev;
  FIRST("AMotionEvent_getAction");
  return 0;
}

int32_t b_AMotionEvent_getEdgeFlags(const void *ev) {
  (void)ev;
  FIRST("AMotionEvent_getEdgeFlags");
  return 0;
}

int32_t b_AMotionEvent_getFlags(const void *ev) {
  (void)ev;
  FIRST("AMotionEvent_getFlags");
  return 0;
}

int32_t b_AMotionEvent_getMetaState(const void *ev) {
  (void)ev;
  FIRST("AMotionEvent_getMetaState");
  return 0;
}

int64_t b_AMotionEvent_getDownTime(const void *ev) {
  (void)ev;
  FIRST("AMotionEvent_getDownTime");
  return 0;
}

int64_t b_AMotionEvent_getEventTime(const void *ev) {
  (void)ev;
  FIRST("AMotionEvent_getEventTime");
  return 0;
}

float b_AMotionEvent_getXPrecision(const void *ev) {
  (void)ev;
  FIRST("AMotionEvent_getXPrecision");
  return 0.0f;
}

float b_AMotionEvent_getYPrecision(const void *ev) {
  (void)ev;
  FIRST("AMotionEvent_getYPrecision");
  return 0.0f;
}

float b_AMotionEvent_getOrientation(const void *ev, size_t idx) {
  (void)ev, (void)idx;
  FIRST("AMotionEvent_getOrientation");
  return 0.0f;
}

float b_AMotionEvent_getPressure(const void *ev, size_t idx) {
  (void)ev, (void)idx;
  FIRST("AMotionEvent_getPressure");
  return 0.0f;
}

float b_AMotionEvent_getSize(const void *ev, size_t idx) {
  (void)ev, (void)idx;
  FIRST("AMotionEvent_getSize");
  return 0.0f;
}

float b_AMotionEvent_getToolMajor(const void *ev, size_t idx) {
  (void)ev, (void)idx;
  FIRST("AMotionEvent_getToolMajor");
  return 0.0f;
}

float b_AMotionEvent_getToolMinor(const void *ev, size_t idx) {
  (void)ev, (void)idx;
  FIRST("AMotionEvent_getToolMinor");
  return 0.0f;
}

float b_AMotionEvent_getTouchMajor(const void *ev, size_t idx) {
  (void)ev, (void)idx;
  FIRST("AMotionEvent_getTouchMajor");
  return 0.0f;
}

float b_AMotionEvent_getTouchMinor(const void *ev, size_t idx) {
  (void)ev, (void)idx;
  FIRST("AMotionEvent_getTouchMinor");
  return 0.0f;
}

float b_AMotionEvent_getX(const void *ev, size_t idx) {
  (void)ev, (void)idx;
  FIRST("AMotionEvent_getX");
  return 0.0f;
}

float b_AMotionEvent_getY(const void *ev, size_t idx) {
  (void)ev, (void)idx;
  FIRST("AMotionEvent_getY");
  return 0.0f;
}

float b_AMotionEvent_getHistoricalPressure(const void *ev, size_t idx, size_t hist) {
  (void)ev, (void)idx, (void)hist;
  FIRST("AMotionEvent_getHistoricalPressure");
  return 0.0f;
}

float b_AMotionEvent_getHistoricalSize(const void *ev, size_t idx, size_t hist) {
  (void)ev, (void)idx, (void)hist;
  FIRST("AMotionEvent_getHistoricalSize");
  return 0.0f;
}

float b_AMotionEvent_getHistoricalX(const void *ev, size_t idx, size_t hist) {
  (void)ev, (void)idx, (void)hist;
  FIRST("AMotionEvent_getHistoricalX");
  return 0.0f;
}

float b_AMotionEvent_getHistoricalY(const void *ev, size_t idx, size_t hist) {
  (void)ev, (void)idx, (void)hist;
  FIRST("AMotionEvent_getHistoricalY");
  return 0.0f;
}

size_t b_AMotionEvent_getPointerCount(const void *ev) {
  (void)ev;
  FIRST("AMotionEvent_getPointerCount");
  return 0;
}
int32_t b_AMotionEvent_getPointerId(const void *ev, size_t idx) {
  (void)ev, (void)idx;
  return 0;
}
size_t b_AMotionEvent_getHistorySize(const void *ev) {
  (void)ev;
  return 0;
}
int64_t b_AMotionEvent_getHistoricalEventTime(const void *ev, size_t hist) {
  (void)ev, (void)hist;
  return 0;
}

/* -------------------------------------------------------- ANativeWindow */
int32_t b_ANativeWindow_lock(void *win, void *buf, void *dirty) {
  (void)win, (void)buf, (void)dirty;
  FIRST("ANativeWindow_lock");
  return -1;
}
int32_t b_ANativeWindow_unlockAndPost(void *win) {
  (void)win;
  return -1;
}

/* -------------------------------------------------------- libc */
/* bionic's isnan(double) is a function (newlib's is a macro) */
int b_isnan(double x) { return isnan(x); }

/* SCHED_OTHER only: Horizon threads have priorities, not policies */
int b_pthread_attr_setschedpolicy(void *attr, int policy) {
  (void)attr, (void)policy;
  return 0;
}

int b_wprintf(const wchar_t *fmt, ...) {
  (void)fmt;
  FIRST("wprintf");
  return 0;
}

int b_swscanf(const wchar_t *s, const wchar_t *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  const int n = vswscanf(s, fmt, ap);
  va_end(ap);
  return n;
}
