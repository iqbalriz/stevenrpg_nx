/* su_cursor.h -- the pointer cursor drawn over the frame (su_cursor.c). MIT. */
#ifndef SU_CURSOR_H
#define SU_CURSOR_H

/* the cursor at x, y (pixels of a w x h surface); down: a tap is held;
 * touched: it moved or tapped this frame (shows it, restarts the fade) */
void su_cursor_set(float x, float y, int w, int h, int down, int touched);
/* draw it before each swap (dcr_present_hook) */
void su_cursor_install(void);

#endif
