#pragma once
void input_init(void);
void input_poll(void);                  // lo llama ui_runloop_tick
void input_stick(float *x, float *y);   // stick izquierdo en [-1, 1] (acelerómetro)
void ui_touch_event(int finger, int phase, float x, float y); // uikit.c (0 began, 1 moved, 3 ended)
