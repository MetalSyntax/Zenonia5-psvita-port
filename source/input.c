// Touch frontal + botones de la Vita -> toques de UIKit (uikit.c) en coordenadas de la vista.
//
// La pantalla muestra el drawable girado/escalado (gles_present); acá se hace el mapeo inverso:
// punto de pantalla -> normalizado en el rectángulo destino -> coordenadas de la vista de iOS.
// Botones: cada uno "toca" un punto configurable en ux0:data/<slug>/controls.txt, en coordenadas
// lógicas de lo que se ve (p. ej. 480x320 para un juego apaisado):
//     CROSS 420 260
//     RTRIGGER 440 160
// Se edita por FTP sin recompilar. SELECT+START (2 s) cambia la rotación y la guarda.
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/touch.h>
#include <stdio.h>
#include <string.h>

#include "gles.h"
#include "hle.h"
#include "input.h"
#include "log.h"

#define FINGERS 6
#define BTN_FINGER0 2 // los botones usan dedos virtuales 2..7 (los reales, 0..1)
extern int port_rotation;

static struct {
    int id, down;
} fingers[2];
static uint32_t prev_buttons;
static float stick_x, stick_y;
static uint64_t combo_since;

static const struct {
    const char *name;
    uint32_t mask;
} names[] = {
    {"CROSS", SCE_CTRL_CROSS},       {"CIRCLE", SCE_CTRL_CIRCLE}, {"SQUARE", SCE_CTRL_SQUARE},
    {"TRIANGLE", SCE_CTRL_TRIANGLE}, {"LTRIGGER", SCE_CTRL_LTRIGGER}, {"RTRIGGER", SCE_CTRL_RTRIGGER},
    {"START", SCE_CTRL_START},       {"SELECT", SCE_CTRL_SELECT}, {"UP", SCE_CTRL_UP},
    {"DOWN", SCE_CTRL_DOWN},         {"LEFT", SCE_CTRL_LEFT},     {"RIGHT", SCE_CTRL_RIGHT},
};
#define MAX_BIND 6
static struct {
    uint32_t mask;
    float x, y; // lógicas (lo que se ve)
} binds[MAX_BIND];
static int nbinds;
// STICK x y r: stick izquierdo / cruceta = dedo virtual arrastrado alrededor de (x,y) lógicos (pad táctil).
static float pad_x, pad_y, pad_r;
static int pad_down;
#define PAD_FINGER (BTN_FINGER0 + MAX_BIND)

static void load_controls(void) {
    FILE *f = fopen(DATA_PATH "controls.txt", "r");
    if (!f) {
        l_info("sin controls.txt: solo touch + stick (acelerómetro)");
        return;
    }
    char line[96], n[32];
    float x, y, r;
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "STICK %f %f %f", &x, &y, &r) == 3)
            pad_x = x, pad_y = y, pad_r = r;
        else if (line[0] != '#' && nbinds < MAX_BIND && sscanf(line, "%31s %f %f", n, &x, &y) == 3)
            for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
                if (!strcmp(n, names[i].name))
                    binds[nbinds++] = (typeof(binds[0])){names[i].mask, x, y};
    fclose(f);
    l_info("controls.txt: %d botones%s", nbinds, pad_r > 0 ? " + STICK" : "");
}

void input_init(void) {
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    FILE *f = fopen(DATA_PATH "rotation.txt", "r");
    if (f) {
        int r;
        if (fscanf(f, "%d", &r) == 1 && r >= 0 && r <= 2)
            port_rotation = r;
        fclose(f);
    }
    load_controls();
}

void input_stick(float *x, float *y) {
    *x = stick_x;
    *y = stick_y;
}

// Normalizado [0,1] en lo que se ve -> coordenadas de la vista (ver uv_rot en gles.c).
static void to_view(float nx, float ny, float *vx, float *vy) {
    switch (gles_rotation()) {
    case 1:
        *vx = (1 - ny) * fb_w, *vy = nx * fb_h;
        break;
    case 2:
        *vx = ny * fb_w, *vy = (1 - nx) * fb_h;
        break;
    default:
        *vx = nx * fb_w, *vy = ny * fb_h;
    }
}

static int screen_to_view(int sx, int sy, float *vx, float *vy) {
    int dx, dy, dw, dh;
    gles_dest_rect(&dx, &dy, &dw, &dh);
    float nx = (sx - dx) / (float)dw, ny = (sy - dy) / (float)dh;
    if (nx < 0 || nx > 1 || ny < 0 || ny > 1)
        return 0;
    to_view(nx, ny, vx, vy);
    return 1;
}

static void logical_size(float *w, float *h) {
    *w = gles_rotation() ? fb_h : fb_w;
    *h = gles_rotation() ? fb_w : fb_h;
}

void input_poll(void) {
    SceTouchData t;
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &t, 1) < 1)
        t.reportNum = 0;
    // dos dedos como máximo (los juegos de 2009 rara vez usan más)
    for (int k = 0; k < 2; k++) {
        int found = -1;
        for (int i = 0; i < (int)t.reportNum; i++)
            if (fingers[k].down && t.report[i].id == fingers[k].id)
                found = i;
        if (fingers[k].down && found < 0) {
            fingers[k].down = 0;
            ui_touch_event(k, 3, -1, -1);
        }
    }
    for (int i = 0; i < (int)t.reportNum && i < 2; i++) {
        float vx, vy;
        if (!screen_to_view(t.report[i].x / 2, t.report[i].y / 2, &vx, &vy))
            continue;
        int k = -1;
        for (int j = 0; j < 2; j++)
            if (fingers[j].down && fingers[j].id == t.report[i].id)
                k = j;
        if (k < 0) {
            for (int j = 0; j < 2 && k < 0; j++)
                if (!fingers[j].down)
                    k = j;
            if (k < 0)
                continue;
            fingers[k].down = 1;
            fingers[k].id = t.report[i].id;
            ui_touch_event(k, 0, vx, vy);
        } else {
            ui_touch_event(k, 1, vx, vy);
        }
    }

    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    stick_x = (pad.lx - 128) / 127.0f;
    stick_y = (pad.ly - 128) / 127.0f;
    if (stick_x > -0.12f && stick_x < 0.12f)
        stick_x = 0;
    if (stick_y > -0.12f && stick_y < 0.12f)
        stick_y = 0;
    uint32_t b = pad.buttons, changed = b ^ prev_buttons;
    for (int i = 0; i < nbinds; i++) {
        if (!(changed & binds[i].mask))
            continue;
        float lw, lh, vx, vy;
        logical_size(&lw, &lh);
        to_view(binds[i].x / lw, binds[i].y / lh, &vx, &vy);
        ui_touch_event(BTN_FINGER0 + i, (b & binds[i].mask) ? 0 : 3, vx, vy);
    }
    if (pad_r > 0) {
        float px = stick_x, py = stick_y;
        int dx = !!(b & SCE_CTRL_RIGHT) - !!(b & SCE_CTRL_LEFT), dy = !!(b & SCE_CTRL_DOWN) - !!(b & SCE_CTRL_UP);
        if (px * px + py * py < 0.35f * 0.35f) // zona muerta grande: un dedo fantasma bloquea el toque real
            px = py = 0;
        if (dx || dy)
            px = dx * (dy ? 0.707f : 1), py = dy * (dx ? 0.707f : 1);
        int on = px != 0 || py != 0;
        if (on || pad_down) {
            float lw, lh, vx, vy;
            logical_size(&lw, &lh);
            to_view((pad_x + px * pad_r) / lw, (pad_y + py * pad_r) / lh, &vx, &vy);
            ui_touch_event(PAD_FINGER, on ? (pad_down ? 1 : 0) : 3, vx, vy);
            pad_down = on;
        }
    }
    // SELECT+START sostenidos 2 s: rotación siguiente (se guarda).
    uint64_t now = sceKernelGetProcessTimeWide();
    if ((b & (SCE_CTRL_SELECT | SCE_CTRL_START)) == (SCE_CTRL_SELECT | SCE_CTRL_START)) {
        if (!combo_since)
            combo_since = now;
        else if (now - combo_since > 2000000) {
            port_rotation = (port_rotation + 1) % 3;
            combo_since = now + 60000000; // una vez por pulsación
            FILE *f = fopen(DATA_PATH "rotation.txt", "w");
            if (f) {
                fprintf(f, "%d\n", port_rotation);
                fclose(f);
            }
            l_info("rotación -> %d", port_rotation);
        }
    } else {
        combo_since = 0;
    }
    prev_buttons = b;
}
