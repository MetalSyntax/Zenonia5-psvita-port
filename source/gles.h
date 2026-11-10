#pragma once
#include <stdint.h>

extern int fb_w, fb_h; // tamaño del drawable de iOS (renderbuffer de la CAEAGLLayer)
void gles_init(void);
void gles_set_size(int w, int h);
void gles_drawable_storage(int w, int h); // renderbufferStorage:fromDrawable:
void gles_present(void);
void gles_bind_screen(void);                // GLKView bindDrawable                   // presentRenderbuffer:
int gles_rotation(void);                   // 0 sin girar, 1 landscape-right, 2 landscape-left
void gles_dest_rect(int *x, int *y, int *w, int *h);
uint32_t gles_get_draw_calls(void);
int ime_active(void);
