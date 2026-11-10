// OpenGL ES 1.1 de iOS -> vitaGL.
//
// Base: gles.c de Inotia2-vita. El juego dibuja en el renderbuffer que EAGLContext le da para su
// CAEAGLLayer (320x480 vertical en los juegos de esta época, aunque se jueguen apaisados: rotan
// la proyección ellos mismos). Acá ese renderbuffer es un FBO de vitaGL con textura + profundidad;
// viewport, scissor y glCopyTexImage2D siguen en el espacio de iOS. gles_present lo dibuja girado
// (según port_config.h) y escalado a 960x544.
//
// FBOs OES del juego: el que tiene el renderbuffer del drawable como color = nuestro FBO de pantalla;
// los demás (render-to-texture) se crean de verdad en vitaGL.
// GL_FIXED: vitaGL lo acepta nativo en vértices/normales/texcoords (lo escala en el shader); solo el color se
// convierte a float en CPU al dibujar (también desde VBOs, con una copia en CPU de cada buffer).
// vitaGL lo acepta nativo en vértice/normal/texcoord, pero la corrida 011 de Real Soccer colgó la GPU con eso
// activado (junto con el desborde de pila de abajo): no reactivar sin probarlo solo.
#include <math.h>
#include <psp2/kernel/processmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vitaGL.h>

#include "gles.h"
#include "hle.h"
#include "imports.h"
#include "log.h"

#define SCR_W 960
#define SCR_H 544
#define GL_FRAMEBUFFER_COMPLETE_OES 0x8CD5
#define GL_RENDERBUFFER_WIDTH_OES 0x8D42
#define GL_RENDERBUFFER_HEIGHT_OES 0x8D43
#define GL_RENDERBUFFER_INTERNAL_FORMAT_OES 0x8D44
#define GL_FRAMEBUFFER_BINDING_OES 0x8CA6
#define GL_RENDERBUFFER_BINDING_OES 0x8CA7
#define GL_COLOR_ATTACHMENT0_OES 0x8CE0
#define GL_DEPTH_ATTACHMENT_OES 0x8D00
#define GL_MATRIX_PALETTE_OES 0x8840
#ifndef PORT_GL_DIAG
#define PORT_GL_DIAG 1 // port_config.h: 0 = sin capturas TGA ni trazados de frame (escribir 600 KB a ux0 traba
#endif                 // el hilo principal varios segundos: Real Soccer, corrida 017)
#ifndef PORT_TRACE_EVERY
#define PORT_TRACE_EVERY 0 // port_config.h: trazar además un frame cada N presentaciones (8 veces)
#endif

int fb_w = 320, fb_h = 480;
static GLuint scr_fbo, scr_tex, scr_depth;

// ---------------------------------------------------------------- arrays (GL_FIXED -> float)

typedef struct {
    GLint size;
    GLenum type;
    GLsizei stride;
    const void *ptr;
    GLuint vbo; // buffer ligado al fijar el puntero (ptr es un offset)
} array_ptr;
enum { A_VTX, A_NRM, A_COL, A_TC0, A_TC1, A_N };
static array_ptr arr[A_N];
static int client_unit;
// GL_TEXTURE_ENV_MODE por unidad de textura: gles_present pisa la 0 y la restaura (el césped usa COMBINE en la 1).
static GLint env_mode[2] = {GL_MODULATE, GL_MODULATE};
static int server_unit;
static GLfixed color[4] = {0x10000, 0x10000, 0x10000, 0x10000};
static GLuint bound_array_buffer, bound_element_buffer;

// ponytail: perfil por frame (corrida 020: 1.6 fps en carrera). Quitar al llegar a 25-30 fps.
enum { P_CONV, P_SKIN, P_DRAW, P_BUF, P_TEX, P_SWAP, P_N };
static uint64_t prof[P_N], prof_n[P_N], prof_bytes;
static uint64_t now_us(void) { return sceKernelGetProcessTimeWide(); }
#define PROF(k, stmt) do { uint64_t _t = now_us(); stmt; prof[k] += now_us() - _t; prof_n[k]++; } while (0)

// Copias en CPU de los VBO (para convertir GL_FIXED y para índices en glDrawElements).
#define MAX_VBO 4096
static struct {
    GLuint id;
    void *data;
    int size;
} vbo_copy[MAX_VBO];
static void *vbo_data(GLuint id) {
    for (int i = 0; i < MAX_VBO; i++)
        if (vbo_copy[i].id == id)
            return vbo_copy[i].data;
    return NULL;
}
static void g_glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage) {
    GLuint id = target == GL_ARRAY_BUFFER ? bound_array_buffer : bound_element_buffer;
    int free_slot = -1;
    for (int i = 0; i < MAX_VBO; i++) {
        if (vbo_copy[i].id == id) {
            free_slot = i;
            break;
        }
        if (free_slot < 0 && !vbo_copy[i].id)
            free_slot = i;
    }
    if (free_slot >= 0 && id) {
        vbo_copy[free_slot].id = id;
        vbo_copy[free_slot].data = realloc(vbo_copy[free_slot].data, size);
        vbo_copy[free_slot].size = size;
        if (data)
            memcpy(vbo_copy[free_slot].data, data, size);
    }
    PROF(P_BUF, glBufferData(target, size, data, usage));
    prof_bytes += size;
}
static void g_glBufferSubData(GLenum target, GLintptr off, GLsizeiptr size, const void *data) {
    GLuint id = target == GL_ARRAY_BUFFER ? bound_array_buffer : bound_element_buffer;
    for (int i = 0; i < MAX_VBO; i++)
        if (vbo_copy[i].id == id && off + size <= vbo_copy[i].size)
            memcpy((uint8_t *)vbo_copy[i].data + off, data, size);
    PROF(P_BUF, glBufferSubData(target, off, size, data));
    prof_bytes += size;
}
static void g_glBindBuffer(GLenum target, GLuint id) {
    if (target == GL_ARRAY_BUFFER)
        bound_array_buffer = id;
    else
        bound_element_buffer = id;
    glBindBuffer(target, id);
}
static void g_glDeleteBuffers(GLsizei n, const GLuint *ids) {
    for (int k = 0; k < n; k++)
        for (int i = 0; i < MAX_VBO; i++)
            if (vbo_copy[i].id == ids[k]) {
                free(vbo_copy[i].data);
                vbo_copy[i].data = NULL;
                vbo_copy[i].id = 0;
            }
    glDeleteBuffers(n, ids);
}

static float *conv_buf[A_N];
static int conv_cap[A_N];

// Solo convierte los vértices [lo, n) que usa el draw; el resto del búfer queda sin tocar.
static const void *fixed_to_float(const array_ptr *a, int lo, int n, int slot) {
    int need = n * a->size;
    if (need > conv_cap[slot]) {
        conv_buf[slot] = realloc(conv_buf[slot], need * sizeof(float));
        conv_cap[slot] = need;
    }
    const uint8_t *base = a->vbo ? (const uint8_t *)vbo_data(a->vbo) + (uintptr_t)a->ptr : a->ptr;
    if (!base)
        return NULL;
    int stride = a->stride ? a->stride : a->size * 4;
    float *dst = conv_buf[slot];
    for (int i = lo; i < n; i++) {
        const int32_t *src = (const int32_t *)(base + i * stride);
        for (int c = 0; c < a->size; c++)
            dst[i * a->size + c] = src[c] * (1.0f / 65536.0f);
    }
    return dst;
}

// Corrida 021: convertir todo en CPU costaba 1.3 s por frame en carrera.
#ifndef PORT_NATIVE_FIXED
#define PORT_NATIVE_FIXED 1 // port_config.h: 0 = convertir todo en CPU (Real Soccer 2010: destellos y saltos de color
#endif                      // con el nativo; allá la conversión cuesta < 2 ms/frame)
#define NEEDS_CONV(k) (arr[k].size && arr[k].type == GL_FIXED && (!PORT_NATIVE_FIXED || (k) == A_COL))
// Arrays habilitados (glEnableClientState): convertir uno deshabilitado es trabajo perdido y lee punteros viejos.
static uint8_t cl_on[A_N];
#define DRAW_CONV(k) (cl_on[k] && NEEDS_CONV(k))
static void set_pointer(int k, int lo, int n) {
    array_ptr *a = &arr[k];
    if (!a->size)
        return;
    const void *p = a->ptr;
    GLenum type = a->type;
    GLsizei stride = a->stride;
    int unbind = 0;
    if (NEEDS_CONV(k)) {
        if (!n)
            return; // se convierte al dibujar
        p = fixed_to_float(a, lo, n, k);
        type = GL_FLOAT;
        stride = 0;
        unbind = 1; // puntero de CPU: sin VBO ligado
    } else if (a->vbo != bound_array_buffer) {
        glBindBuffer(GL_ARRAY_BUFFER, a->vbo);
    }
    if (unbind && bound_array_buffer)
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    switch (k) {
    case A_VTX:
        glVertexPointer(a->size, type, stride, p);
        break;
    case A_NRM:
        glNormalPointer(type, stride, p);
        break;
    case A_COL:
        glColorPointer(a->size, type, stride, p);
        break;
    default:
        glClientActiveTexture(GL_TEXTURE0 + (k - A_TC0));
        glTexCoordPointer(a->size, type, stride, p);
        glClientActiveTexture(GL_TEXTURE0 + client_unit);
    }
    if (unbind && bound_array_buffer)
        glBindBuffer(GL_ARRAY_BUFFER, bound_array_buffer);
    else if (!unbind && a->vbo != bound_array_buffer)
        glBindBuffer(GL_ARRAY_BUFFER, bound_array_buffer);
}

static void g_glVertexPointer(GLint size, GLenum type, GLsizei stride, const void *ptr) {
    arr[A_VTX] = (array_ptr){size, type, stride, ptr, bound_array_buffer};
    set_pointer(A_VTX, 0, 0);
}
static void g_glNormalPointer(GLenum type, GLsizei stride, const void *ptr) {
    arr[A_NRM] = (array_ptr){3, type, stride, ptr, bound_array_buffer};
    set_pointer(A_NRM, 0, 0);
}
static void g_glColorPointer(GLint size, GLenum type, GLsizei stride, const void *ptr) {
    arr[A_COL] = (array_ptr){size, type, stride, ptr, bound_array_buffer};
    set_pointer(A_COL, 0, 0);
}
static void g_glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void *ptr) {
    arr[A_TC0 + client_unit] = (array_ptr){size, type, stride, ptr, bound_array_buffer};
    set_pointer(A_TC0 + client_unit, 0, 0);
}
static void g_glClientActiveTexture(GLenum unit) {
    client_unit = (unit - GL_TEXTURE0) & 1;
    glClientActiveTexture(unit);
}

static void convert_fixed(int lo, int n) {
    for (int k = 0; k < A_N; k++)
        if (DRAW_CONV(k))
            set_pointer(k, lo, n);
}

static uint32_t draw_calls;
uint32_t gles_get_draw_calls(void) { return draw_calls; }

static int skin(int lo, int hi);
static void unskin(void);
// ponytail: diagnóstico corrida 013 (INVALID_ENUM pendiente en cada draw): qué llamada lo genera. Quitar junto al trazado.
static void chk(const char *fn, unsigned a, unsigned b) {
    static int n;
    GLenum e = glGetError();
    if (e && n++ < 40)
        l_info("GLERR %s(0x%X, 0x%X) -> 0x%X", fn, a, b, e);
}
static void e_glTexParameteri(GLenum t, GLenum p, GLint v) { glTexParameteri(t, p, v); chk("glTexParameteri", p, v); }
static void e_glPixelStorei(GLenum p, GLint v) { glPixelStorei(p, v); chk("glPixelStorei", p, v); }
static void e_glBlendFunc(GLenum a, GLenum b) { glBlendFunc(a, b); chk("glBlendFunc", a, b); }
static void e_glAlphaFunc(GLenum a, GLfloat r) { glAlphaFunc(a, r); chk("glAlphaFunc", a, 0); }
static void e_glDepthFunc(GLenum a) { glDepthFunc(a); chk("glDepthFunc", a, 0); }
static void e_glCullFace(GLenum a) { glCullFace(a); chk("glCullFace", a, 0); }
static void e_glFrontFace(GLenum a) { glFrontFace(a); chk("glFrontFace", a, 0); }
static void e_glFogf(GLenum p, GLfloat v) { glFogf(p, v); chk("glFogf", p, 0); }
static void e_glFogfv(GLenum p, const GLfloat *v) { glFogfv(p, v); chk("glFogfv", p, 0); }
static void e_glMaterialf(GLenum f, GLenum p, GLfloat v) { glMaterialf(f, p, v); chk("glMaterialf", f, p); }
static void e_glMaterialfv(GLenum f, GLenum p, const GLfloat *v) { glMaterialfv(f, p, v); chk("glMaterialfv", f, p); }
static void e_glLightfv(GLenum l, GLenum p, const GLfloat *v) { glLightfv(l, p, v); chk("glLightfv", l, p); }
// ponytail: rango del alfa de TEXTURE_ENV_COLOR (césped de Real Soccer 2010: INTERPOLATE por CONSTANT.a; fuera de
// [0,1] extrapola → ¿destellos?, corrida 022). Sale en la línea PERF. Quitar al resolver los destellos.
static float envc_min = 1e9f, envc_max = -1e9f;
static unsigned envc_n;
static void e_glTexEnvfv(GLenum t, GLenum p, const GLfloat *v) {
    if (p == GL_TEXTURE_ENV_COLOR) {
        envc_n++;
        if (v[3] < envc_min || v[3] != v[3])
            envc_min = v[3];
        if (v[3] > envc_max)
            envc_max = v[3];
    }
    glTexEnvfv(t, p, v);
    chk("glTexEnvfv", t, p);
}
static void e_glGetFloatv(GLenum p, GLfloat *v) { glGetFloatv(p, v); chk("glGetFloatv", p, 0); }
static void e_glBindTexture(GLenum t, GLuint id) { glBindTexture(t, id); chk("glBindTexture", t, id); }
static void e_glActiveTexture(GLenum t) { server_unit = (t - GL_TEXTURE0) & 1; glActiveTexture(t); chk("glActiveTexture", t, 0); }

// ponytail: trazado de UN frame (corrida 011: pantalla entera blanca con alfa 0). Quitar al verse bien.
static int trace_frame, trace_n, trace_max = 40;
// Trazado del primer frame 3D (primer draw con luces y proyección en perspectiva; el menú de Real Soccer activa
// GL_LIGHTING sin dibujar 3D) y de 2 más (+150, +300), con matrices completas y captura frame_lit_N.tga del FBO
// al terminar (Real Soccer, corridas 013-015: partido negro).
static int lit_state, lit_full;
static void lit_check(void) {
    if (!PORT_GL_DIAG || lit_state || !glIsEnabled(GL_LIGHTING))
        return;
    GLfloat p[16];
    glGetFloatv(GL_PROJECTION_MATRIX, p);
    if (p[11] < -0.5f && p[15] == 0) {
        lit_state = 1;
        l_info("TRACE primer draw 3D (P %.3f %.3f %.3f)", p[0], p[5], p[10]);
    }
}
static void trace_draw(const char *fn, GLenum mode, int count, int first) {
    lit_check();
    if (!trace_frame || trace_n++ > trace_max)
        return;
    GLenum err0 = glGetError(); // antes de nuestras consultas
    const array_ptr *v = &arr[A_VTX];
    float f[3] = {0};
    const uint8_t *base = v->vbo ? (const uint8_t *)vbo_data(v->vbo) + (uintptr_t)v->ptr : v->ptr;
    if (base) {
        int st = v->stride ? v->stride : v->size * (v->type == GL_SHORT ? 2 : v->type == GL_BYTE ? 1 : 4);
        const uint8_t *e = base + first * st;
        for (int c = 0; c < 3 && c < v->size; c++)
            f[c] = v->type == GL_FLOAT ? ((const float *)e)[c] : v->type == GL_FIXED ? ((const int32_t *)e)[c] / 65536.0f
                 : v->type == GL_SHORT ? ((const int16_t *)e)[c] : ((const int8_t *)e)[c];
    }
    GLfloat pm[16], mv[16];
    GLint tex = 0;
    glGetFloatv(GL_PROJECTION_MATRIX, pm);
    glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    if (lit_state && glIsEnabled(GL_LIGHTING) && lit_full++ < 6) {
        GLfloat dr[2] = {0}, cc[4] = {0};
        GLint df = 0;
        GLboolean dm = 0, cm[4] = {0};
        glGetFloatv(GL_DEPTH_RANGE, dr);
        glGetIntegerv(GL_DEPTH_FUNC, &df);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &dm);
        glGetBooleanv(GL_COLOR_WRITEMASK, cm);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
        l_info("TRACE3D P [%g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g]", pm[0], pm[1], pm[2], pm[3], pm[4],
               pm[5], pm[6], pm[7], pm[8], pm[9], pm[10], pm[11], pm[12], pm[13], pm[14], pm[15]);
        l_info("TRACE3D M [%g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g]", mv[0], mv[1], mv[2], mv[3], mv[4],
               mv[5], mv[6], mv[7], mv[8], mv[9], mv[10], mv[11], mv[12], mv[13], mv[14], mv[15]);
        l_info("TRACE3D depth range %g %g func 0x%X mask %d | colormask %d%d%d%d | clear %.2f %.2f %.2f %.2f | fog %d "
               "nrm %d x 0x%X",
               dr[0], dr[1], df, dm, cm[0], cm[1], cm[2], cm[3], cc[0], cc[1], cc[2], cc[3], glIsEnabled(GL_FOG),
               arr[A_NRM].size, arr[A_NRM].type);
    }
    GLint vp[4], sc[4], fb = 0;
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_SCISSOR_BOX, sc);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
    l_info("TRACE   vp %d %d %d %d | scissor %d (%d %d %d %d) | fb %d | col %d x 0x%X st %d | env 0x%X", vp[0], vp[1],
           vp[2], vp[3], glIsEnabled(GL_SCISSOR_TEST), sc[0], sc[1], sc[2], sc[3], fb, arr[A_COL].size, arr[A_COL].type,
           arr[A_COL].stride, env_mode[server_unit]);
    l_info("TRACE %s mode %d n %d | vtx %d x 0x%X st %d vbo %u v0 (%.2f %.2f %.2f) | col %s tc0 %d x 0x%X | tex %d "
           "T%d B%d A%d D%d C%d L%d | rgba %.2f %.2f %.2f %.2f | P %.3f %.3f %.3f %.3f | M %.2f %.2f %.2f %.2f "
           "| err 0x%X",
           fn, mode, count, v->size, v->type, v->stride, v->vbo, f[0], f[1], f[2], arr[A_COL].size ? "arr" : "-",
           arr[A_TC0].size, arr[A_TC0].type, tex, glIsEnabled(GL_TEXTURE_2D), glIsEnabled(GL_BLEND),
           glIsEnabled(GL_ALPHA_TEST), glIsEnabled(GL_DEPTH_TEST), glIsEnabled(GL_CULL_FACE), glIsEnabled(GL_LIGHTING),
           color[0] / 65536.0f, color[1] / 65536.0f, color[2] / 65536.0f, color[3] / 65536.0f, pm[0], pm[5], pm[10],
           pm[12], mv[0], mv[5], mv[12], mv[13], err0);
}

static void g_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    trace_draw("DrawArrays", mode, count, first);
    draw_calls++;
    PROF(P_CONV, convert_fixed(first, first + count));
    int skinned;
    PROF(P_SKIN, skinned = skin(first, first + count));
    PROF(P_DRAW, glDrawArrays(mode, first, count));
    if (skinned)
        unskin();
}

static void g_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *idx) {
    trace_draw("DrawElements", mode, count, 0);
    draw_calls++;
    extern int palette_active(void);
    int need_fixed = palette_active();
    for (int k = 0; k < A_N; k++)
        need_fixed |= DRAW_CONV(k);
    int maxi = 0, mini = 0xffff;
    if (need_fixed) {
        const void *ip = bound_element_buffer ? (const uint8_t *)vbo_data(bound_element_buffer) + (uintptr_t)idx : idx;
        for (int i = 0; ip && i < count; i++) {
            int v = type == GL_UNSIGNED_BYTE ? ((const uint8_t *)ip)[i] : ((const uint16_t *)ip)[i];
            if (v > maxi)
                maxi = v;
            if (v < mini)
                mini = v;
        }
        PROF(P_CONV, convert_fixed(mini > maxi ? 0 : mini, maxi + 1));
    }
    int skinned;
    PROF(P_SKIN, skinned = skin(mini > maxi ? 0 : mini, maxi + 1));
    PROF(P_DRAW, glDrawElements(mode, count, type, idx));
    if (skinned)
        unskin();
}

// ---------------------------------------------------------------- estado *x / variantes

static void g_glColor4x(GLfixed r, GLfixed g, GLfixed b, GLfixed a) {
    color[0] = r, color[1] = g, color[2] = b, color[3] = a;
    glColor4x(r, g, b, a);
}
static void g_glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    color[0] = r * 65536, color[1] = g * 65536, color[2] = b * 65536, color[3] = a * 65536;
    glColor4f(r, g, b, a);
}
static void g_glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a) {
    g_glColor4f(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}
static void g_glTexEnvi(GLenum target, GLenum pname, GLint v) {
    if (pname == GL_TEXTURE_ENV_MODE)
        env_mode[server_unit] = v;
    glTexEnvi(target, pname, v);
    chk("glTexEnvi", pname, v);
}
static void g_glTexEnvf(GLenum target, GLenum pname, GLfloat v) {
    // los enums llegan como float
    if (pname == GL_TEXTURE_ENV_MODE || pname == GL_COMBINE_RGB || pname == GL_COMBINE_ALPHA ||
        (pname >= GL_SRC0_RGB && pname <= GL_OPERAND2_ALPHA))
        g_glTexEnvi(target, pname, (GLint)v);
    else
        glTexEnvf(target, pname, v);
}
static void g_glTexEnvx(GLenum target, GLenum pname, GLfixed v) {
    if (pname == GL_RGB_SCALE || pname == GL_ALPHA_SCALE)
        glTexEnvf(target, pname, v / 65536.0f);
    else
        g_glTexEnvi(target, pname, v); // enums sin escalar
}
static void g_glTexEnvxv(GLenum target, GLenum pname, const GLfixed *v) {
    if (pname == GL_TEXTURE_ENV_COLOR) {
        float f[4] = {v[0] / 65536.0f, v[1] / 65536.0f, v[2] / 65536.0f, v[3] / 65536.0f};
        glTexEnvfv(target, pname, f);
    } else {
        g_glTexEnvx(target, pname, v[0]);
    }
}
static void g_glTexParameterx(GLenum target, GLenum pname, GLfixed v) { glTexParameteri(target, pname, v); }
static void g_glTexParameterf(GLenum target, GLenum pname, GLfloat v) {
    if (pname == GL_TEXTURE_MAX_ANISOTROPY_EXT)
        glTexParameterf(target, pname, v);
    else
        glTexParameteri(target, pname, (GLint)v);
}
static void g_glLightModelx(GLenum pname, GLfixed v) { glLightModelxv(pname, &v); }
static void g_glLightModelf(GLenum pname, GLfloat v) { glLightModelfv(pname, &v); }
static void g_glLightf(GLenum l, GLenum pname, GLfloat v) { glLightfv(l, pname, &v); }
static void g_glLightx(GLenum l, GLenum pname, GLfixed v) { g_glLightf(l, pname, v / 65536.0f); }
static void g_glMaterialxv(GLenum f, GLenum p, const GLfixed *v) {
    float x[4] = {v[0] / 65536.0f, v[1] / 65536.0f, v[2] / 65536.0f, v[3] / 65536.0f};
    glMaterialfv(f, p, x);
}
static void g_glGetFixedv(GLenum pname, GLfixed *out) {
    float f[16];
    int n = 1;
    if (pname == GL_MODELVIEW_MATRIX || pname == GL_PROJECTION_MATRIX || pname == GL_TEXTURE_MATRIX)
        n = 16;
    else if (pname == GL_CURRENT_COLOR) {
        memcpy(out, color, sizeof(color));
        return;
    }
    glGetFloatv(pname, f);
    for (int i = 0; i < n; i++)
        out[i] = (GLfixed)(f[i] * 65536.0f);
}
static void g_glGetLightxv(GLenum light, GLenum pname, GLfixed *out) { memset(out, 0, 4 * sizeof(GLfixed)); }
static void g_glGetPointerv(GLenum pname, void **out) {
    if (pname == GL_VERTEX_ARRAY_POINTER)
        *out = (void *)arr[A_VTX].ptr;
    else if (pname == GL_TEXTURE_COORD_ARRAY_POINTER)
        *out = (void *)arr[A_TC0 + client_unit].ptr;
    else
        glGetPointerv(pname, out);
}

// ---------------------------------------------------------------- GL_OES_matrix_palette (skinning)
//
// vitaGL no lo tiene. Se emula en CPU: las matrices de paleta se llevan acá (glMatrixMode
// MATRIX_PALETTE + glCurrentPaletteMatrixOES + glLoad*/Mult*/Translate/...); al dibujar con la paleta
// activa, cada vértice (y normal) se transforma como sum(w_k * P[i_k] * v) y se dibuja con modelview
// identidad (en OES_matrix_palette la paleta reemplaza a la modelview; la proyección se aplica igual).
#define MAX_PALETTE 32
static float palette[MAX_PALETTE][16];
static int cur_palette, palette_mode, palette_on, mi_on, wt_on;
static GLenum real_matrix_mode = GL_MODELVIEW;
static array_ptr mi_arr, wt_arr;

static void mat_mul(float *r, const float *a, const float *b) { // r = a * b (column-major)
    float t[16];
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++)
            t[c * 4 + rr] = a[rr] * b[c * 4] + a[4 + rr] * b[c * 4 + 1] + a[8 + rr] * b[c * 4 + 2] + a[12 + rr] * b[c * 4 + 3];
    memcpy(r, t, sizeof(t));
}
static void pal_mult(const float *m) { mat_mul(palette[cur_palette], palette[cur_palette], m); }
static void fx_to_f(const GLfixed *x, float *f) {
    for (int i = 0; i < 16; i++)
        f[i] = x[i] / 65536.0f;
}

static void g_glMatrixMode(GLenum m) {
    palette_mode = m == GL_MATRIX_PALETTE_OES;
    if (!palette_mode) {
        real_matrix_mode = m;
        glMatrixMode(m);
        chk("glMatrixMode", m, 0);
    }
}
static void g_glLoadIdentity(void) {
    if (!palette_mode)
        return glLoadIdentity();
    static const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    memcpy(palette[cur_palette], id, sizeof(id));
}
static void g_glLoadMatrixf(const GLfloat *m) {
    if (trace_frame && trace_n < 40)
        l_info("TRACE LoadMatrixf [%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]",
               m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
    if (!palette_mode)
        return glLoadMatrixf(m);
    memcpy(palette[cur_palette], m, 16 * sizeof(float));
}
static void g_glLoadMatrixx(const GLfixed *m) {
    if (!palette_mode)
        return glLoadMatrixx(m);
    fx_to_f(m, palette[cur_palette]);
}
static void g_glMultMatrixf(const GLfloat *m) {
    if (!palette_mode)
        return glMultMatrixf(m);
    pal_mult(m);
}
static void g_glMultMatrixx(const GLfixed *m) {
    if (!palette_mode)
        return glMultMatrixx(m);
    float f[16];
    fx_to_f(m, f);
    pal_mult(f);
}
static void g_glTranslatef(GLfloat x, GLfloat y, GLfloat z) {
    if (!palette_mode)
        return glTranslatef(x, y, z);
    const float t[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1};
    pal_mult(t);
}
static void g_glTranslatex(GLfixed x, GLfixed y, GLfixed z) { g_glTranslatef(x / 65536.0f, y / 65536.0f, z / 65536.0f); }
static void g_glScalef(GLfloat x, GLfloat y, GLfloat z) {
    if (!palette_mode)
        return glScalef(x, y, z);
    const float t[16] = {x, 0, 0, 0, 0, y, 0, 0, 0, 0, z, 0, 0, 0, 0, 1};
    pal_mult(t);
}
static void g_glScalex(GLfixed x, GLfixed y, GLfixed z) { g_glScalef(x / 65536.0f, y / 65536.0f, z / 65536.0f); }
static void g_glRotatef(GLfloat a, GLfloat x, GLfloat y, GLfloat z) {
    if (!palette_mode)
        return glRotatef(a, x, y, z);
    float l = sqrtf(x * x + y * y + z * z);
    if (l == 0)
        return;
    x /= l, y /= l, z /= l;
    float c = cosf(a * 3.14159265f / 180), s_ = sinf(a * 3.14159265f / 180), k = 1 - c;
    const float t[16] = {x * x * k + c,     y * x * k + z * s_, x * z * k - y * s_, 0, x * y * k - z * s_, y * y * k + c,
                         y * z * k + x * s_, 0, x * z * k + y * s_, y * z * k - x * s_, z * z * k + c, 0, 0, 0, 0, 1};
    pal_mult(t);
}
static void g_glRotatex(GLfixed a, GLfixed x, GLfixed y, GLfixed z) {
    g_glRotatef(a / 65536.0f, x / 65536.0f, y / 65536.0f, z / 65536.0f);
}
// vitaGL compila con SKIP_ERROR_HANDLING y sus pilas son modelview 32 / proyección y textura 2 (contador uint8):
// un push de más escribe fuera del arreglo y pisa sus globales (matrices basura, partido negro, cuelgue).
// Gameloft (Real Soccer 2009/2010, GlRenderer::Init) hace 100 glPushMatrix seguidos en modelview. Las tres pilas
// se llevan acá (128 niveles; push lleno / pop vacío = no-op). vitaGL solo ve el push/pop del skinning.
// ponytail: una sola pila de textura (no por unidad); alcanza mientras nadie apile matrices de textura en 2 unidades.
#define MSTACK 128
static float mstack[3][MSTACK][16];
static int mdepth[3];
static int mstack_idx(void) { return real_matrix_mode == GL_PROJECTION ? 1 : real_matrix_mode == GL_TEXTURE ? 2 : 0; }
static const GLenum mstack_get[3] = {GL_MODELVIEW_MATRIX, GL_PROJECTION_MATRIX, GL_TEXTURE_MATRIX};
static void g_glPushMatrix(void) {
    if (palette_mode)
        return;
    int s = mstack_idx();
    if (mdepth[s] < MSTACK)
        glGetFloatv(mstack_get[s], mstack[s][mdepth[s]++]);
}
static void g_glPopMatrix(void) {
    if (palette_mode)
        return;
    int s = mstack_idx();
    if (mdepth[s] > 0)
        glLoadMatrixf(mstack[s][--mdepth[s]]);
}
static void g_glCurrentPaletteMatrixOES(GLuint i) { cur_palette = i < MAX_PALETTE ? i : 0; }
static void g_glLoadPaletteFromModelViewMatrixOES(void) { glGetFloatv(GL_MODELVIEW_MATRIX, palette[cur_palette]); }
static void g_glMatrixIndexPointerOES(GLint size, GLenum type, GLsizei stride, const void *p) {
    mi_arr = (array_ptr){size, type, stride, p, bound_array_buffer};
}
static void g_glWeightPointerOES(GLint size, GLenum type, GLsizei stride, const void *p) {
    wt_arr = (array_ptr){size, type, stride, p, bound_array_buffer};
}

static const uint8_t *arr_base(const array_ptr *a) {
    return a->vbo ? (const uint8_t *)vbo_data(a->vbo) + (uintptr_t)a->ptr : (const uint8_t *)a->ptr;
}
static int type_size(GLenum t) {
    return t == GL_BYTE || t == GL_UNSIGNED_BYTE ? 1 : t == GL_SHORT || t == GL_UNSIGNED_SHORT ? 2 : 4;
}
static float read_comp(const uint8_t *p, GLenum t, int i) {
    switch (t) {
    case GL_FLOAT:
        return ((const float *)p)[i];
    case GL_FIXED:
        return ((const int32_t *)p)[i] / 65536.0f;
    case GL_SHORT:
        return ((const int16_t *)p)[i];
    case GL_UNSIGNED_SHORT:
        return ((const uint16_t *)p)[i];
    case GL_BYTE:
        return ((const int8_t *)p)[i];
    default:
        return ((const uint8_t *)p)[i];
    }
}
// Atributo -> floats contiguos (comps por vértice) para los vértices [lo, hi): un switch por dibujo, no
// por componente (era el 90% del tiempo de skin).
static void attr_floats(const uint8_t *b, GLenum t, int stride, int comps, int lo, int hi, float *dst) {
#define ATTR_LOOP(T, SCALE)                                                                                   \
    for (int i = lo; i < hi; i++) {                                                                           \
        const T *s = (const T *)(b + i * stride);                                                             \
        for (int c = 0; c < comps; c++)                                                                       \
            *dst++ = s[c] SCALE;                                                                              \
    }                                                                                                         \
    break;
    switch (t) {
    case GL_FLOAT: ATTR_LOOP(float, )
    case GL_FIXED: ATTR_LOOP(int32_t, * (1.0f / 65536.0f))
    case GL_SHORT: ATTR_LOOP(int16_t, )
    case GL_UNSIGNED_SHORT: ATTR_LOOP(uint16_t, )
    case GL_BYTE: ATTR_LOOP(int8_t, )
    default: ATTR_LOOP(uint8_t, )
    }
#undef ATTR_LOOP
}
static float *skin_v, *skin_n, *skin_tmp;
static int skin_cap;
static uint64_t skin_vtx; // vértices transformados (PERF)
// Transforma los vértices [lo, hi) con la paleta; deja los punteros de vértice/normal apuntando al resultado
// (indexado igual que el original, así que el dibujo no cambia).
static int skin(int lo, int hi) {
    if (!palette_on || !mi_on || !wt_on || !arr[A_VTX].size || lo >= hi)
        return 0;
    const uint8_t *vb = arr_base(&arr[A_VTX]), *nb = arr[A_NRM].size ? arr_base(&arr[A_NRM]) : NULL;
    const uint8_t *ib = arr_base(&mi_arr), *wb = arr_base(&wt_arr);
    if (!vb || !ib || !wb)
        return 0;
    int nk = mi_arr.size < wt_arr.size ? mi_arr.size : wt_arr.size, vc = arr[A_VTX].size > 3 ? 3 : arr[A_VTX].size;
    if (hi > skin_cap) {
        skin_cap = hi;
        skin_v = realloc(skin_v, hi * 3 * sizeof(float));
        skin_n = realloc(skin_n, hi * 3 * sizeof(float));
        skin_tmp = realloc(skin_tmp, hi * (3 + 3 + 4 + 4) * sizeof(float));
    }
    int n = hi - lo;
    float *tv = skin_tmp, *tn = tv + n * 3, *ti = tn + n * 3, *tw = ti + n * 4;
    int vs = arr[A_VTX].stride ? arr[A_VTX].stride : arr[A_VTX].size * type_size(arr[A_VTX].type);
    int ns = nb ? (arr[A_NRM].stride ? arr[A_NRM].stride : 3 * type_size(arr[A_NRM].type)) : 0;
    int is = mi_arr.stride ? mi_arr.stride : mi_arr.size * type_size(mi_arr.type);
    int ws = wt_arr.stride ? wt_arr.stride : wt_arr.size * type_size(wt_arr.type);
    attr_floats(vb, arr[A_VTX].type, vs, vc, lo, hi, tv);
    if (nb)
        attr_floats(nb, arr[A_NRM].type, ns, 3, lo, hi, tn);
    attr_floats(ib, mi_arr.type, is, nk, lo, hi, ti);
    attr_floats(wb, wt_arr.type, ws, nk, lo, hi, tw);
    skin_vtx += n;
    for (int j = 0; j < n; j++) {
        const float *v = tv + j * vc, *q = tn + j * 3, *ix = ti + j * nk, *wt = tw + j * nk;
        float x = v[0], y = v[1], z = vc > 2 ? v[2] : 0;
        float ox = 0, oy = 0, oz = 0, onx = 0, ony = 0, onz = 0;
        for (int k = 0; k < nk; k++) {
            int idx = (int)ix[k];
            float w = wt[k];
            if (w == 0 || idx < 0 || idx >= MAX_PALETTE)
                continue;
            const float *m = palette[idx];
            ox += w * (m[0] * x + m[4] * y + m[8] * z + m[12]);
            oy += w * (m[1] * x + m[5] * y + m[9] * z + m[13]);
            oz += w * (m[2] * x + m[6] * y + m[10] * z + m[14]);
            if (nb) {
                onx += w * (m[0] * q[0] + m[4] * q[1] + m[8] * q[2]);
                ony += w * (m[1] * q[0] + m[5] * q[1] + m[9] * q[2]);
                onz += w * (m[2] * q[0] + m[6] * q[1] + m[10] * q[2]);
            }
        }
        float *o = skin_v + (lo + j) * 3, *on = skin_n + (lo + j) * 3;
        o[0] = ox, o[1] = oy, o[2] = oz;
        on[0] = onx, on[1] = ony, on[2] = onz;
    }
    if (bound_array_buffer)
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexPointer(3, GL_FLOAT, 0, skin_v);
    if (nb)
        glNormalPointer(GL_FLOAT, 0, skin_n);
    if (bound_array_buffer)
        glBindBuffer(GL_ARRAY_BUFFER, bound_array_buffer);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    return 1;
}
static void unskin(void) {
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(real_matrix_mode);
    set_pointer(A_VTX, 0, 0);
    set_pointer(A_NRM, 0, 0);
}

int palette_active(void) { return palette_on && mi_on && wt_on; }
static void g_glEnable(GLenum cap) {
    if (cap == GL_MATRIX_PALETTE_OES) {
        palette_on = 1;
        return;
    }
    glEnable(cap);
    chk("glEnable", cap, 0);
}
static void g_glDisable(GLenum cap) {
    if (cap == GL_MATRIX_PALETTE_OES) {
        palette_on = 0;
        return;
    }
    glDisable(cap);
    chk("glDisable", cap, 0);
}
static int client_slot(GLenum a) {
    return a == GL_VERTEX_ARRAY ? A_VTX : a == GL_NORMAL_ARRAY ? A_NRM : a == GL_COLOR_ARRAY ? A_COL
         : a == GL_TEXTURE_COORD_ARRAY ? A_TC0 + client_unit : -1;
}
static void g_glEnableClientState(GLenum a) {
    if (a == 0x8844 /* MATRIX_INDEX_ARRAY */ || a == 0x86AD /* WEIGHT_ARRAY */) {
        *(a == 0x8844 ? &mi_on : &wt_on) = 1;
        return;
    }
    if (client_slot(a) >= 0)
        cl_on[client_slot(a)] = 1;
    glEnableClientState(a);
    chk("glEnableClientState", a, 0);
}
static void g_glDisableClientState(GLenum a) {
    if (a == 0x8844 || a == 0x86AD) {
        *(a == 0x8844 ? &mi_on : &wt_on) = 0;
        return;
    }
    if (client_slot(a) >= 0)
        cl_on[client_slot(a)] = 0;
    glDisableClientState(a);
    chk("glDisableClientState", a, 0);
}

static const GLubyte *g_glGetString(GLenum name) {
    switch (name) {
    case GL_VENDOR:
        return (const GLubyte *)"Imagination Technologies";
    case GL_RENDERER:
        return (const GLubyte *)"PowerVR SGX 535";
    case GL_VERSION:
        return (const GLubyte *)"OpenGL ES-CM 1.1 IMGSGX535-63.14.2";
    case GL_EXTENSIONS:
        return (const GLubyte *)"GL_OES_blend_subtract GL_OES_compressed_paletted_texture GL_OES_depth24 "
                                "GL_OES_draw_texture GL_OES_framebuffer_object GL_OES_mapbuffer GL_OES_point_size_array "
                                "GL_OES_point_sprite GL_OES_read_format GL_OES_rgb8_rgba8 GL_OES_texture_mirrored_repeat "
                                "GL_EXT_texture_filter_anisotropic GL_EXT_texture_lod_bias GL_IMG_read_format "
                                "GL_IMG_texture_compression_pvrtc GL_IMG_texture_format_BGRA8888 "
                                "GL_APPLE_texture_2D_limited_npot";
    }
    return glGetString(name);
}

static void g_glGetIntegerv(GLenum pname, GLint *v) {
    if (pname == GL_MAX_TEXTURE_SIZE) {
        *v = 2048;
        return;
    }
    glGetIntegerv(pname, v);
    chk("glGetIntegerv", pname, 0);
}

// ---------------------------------------------------------------- framebuffers OES

#define MAX_NAMES 256
static struct {
    int is_drawable; // color del CAEAGLLayer
    GLuint vgl;      // renderbuffer de vitaGL (profundidad de FBOs propios)
    int w, h;
    GLenum fmt;
} rbs[MAX_NAMES];
static struct {
    int screen;  // tiene el drawable como color
    GLuint vgl;  // FBO real de vitaGL (render-to-texture)
    GLuint color_tex;
} fbs[MAX_NAMES];
static GLuint next_rb = 1, next_fb = 1, cur_rb, cur_fb;

static void g_glGenRenderbuffersOES(GLsizei n, GLuint *ids) {
    for (int i = 0; i < n; i++) {
        ids[i] = next_rb < MAX_NAMES ? next_rb++ : 0;
        memset(&rbs[ids[i]], 0, sizeof(rbs[0]));
    }
}
static void g_glGenFramebuffersOES(GLsizei n, GLuint *ids) {
    for (int i = 0; i < n; i++) {
        ids[i] = next_fb < MAX_NAMES ? next_fb++ : 0;
        memset(&fbs[ids[i]], 0, sizeof(fbs[0]));
    }
}
static void g_glBindRenderbufferOES(GLenum target, GLuint rb) { cur_rb = rb < MAX_NAMES ? rb : 0; }
static void g_glRenderbufferStorageOES(GLenum target, GLenum fmt, GLsizei w, GLsizei h) {
    rbs[cur_rb].w = w, rbs[cur_rb].h = h, rbs[cur_rb].fmt = fmt;
}
// EAGLContext renderbufferStorage:fromDrawable: (uikit.c)
void gles_drawable_storage(int w, int h) {
    rbs[cur_rb].is_drawable = 1;
    rbs[cur_rb].w = w, rbs[cur_rb].h = h;
    gles_set_size(w, h);
}
static void bind_fb(GLuint fb) {
    if (!fb || fbs[fb].screen || !fbs[fb].vgl)
        glBindFramebuffer(GL_FRAMEBUFFER, scr_fbo);
    else
        glBindFramebuffer(GL_FRAMEBUFFER, fbs[fb].vgl);
}
void gles_bind_screen(void) { // GLKView bindDrawable
    cur_fb = 0;
    glBindFramebuffer(GL_FRAMEBUFFER, scr_fbo);
}
static void g_glBindFramebufferOES(GLenum target, GLuint fb) {
    cur_fb = fb < MAX_NAMES ? fb : 0;
    bind_fb(cur_fb);
}
static void g_glFramebufferRenderbufferOES(GLenum target, GLenum att, GLenum rbt, GLuint rb) {
    if (att == GL_COLOR_ATTACHMENT0_OES && rbs[rb].is_drawable) {
        fbs[cur_fb].screen = 1;
        bind_fb(cur_fb);
    }
    // profundidad del FBO de pantalla: ya la tiene el nuestro. FBOs propios con renderbuffer de
    // color que no es el drawable: no aparecieron en estos juegos (se registra).
    else if (att == GL_COLOR_ATTACHMENT0_OES)
        l_error("FBO %u con renderbuffer de color propio: sin soporte", cur_fb);
}
static void g_glFramebufferTexture2DOES(GLenum target, GLenum att, GLenum textarget, GLuint tex, GLint level) {
    if (!fbs[cur_fb].vgl) {
        glGenFramebuffers(1, &fbs[cur_fb].vgl);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbs[cur_fb].vgl);
    if (att == GL_COLOR_ATTACHMENT0_OES) {
        fbs[cur_fb].color_tex = tex;
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, textarget, tex, level);
    }
}
static GLenum g_glCheckFramebufferStatusOES(GLenum target) { return GL_FRAMEBUFFER_COMPLETE_OES; }
static void g_glGetRenderbufferParameterivOES(GLenum target, GLenum pname, GLint *v) {
    *v = pname == GL_RENDERBUFFER_WIDTH_OES ? rbs[cur_rb].w : pname == GL_RENDERBUFFER_HEIGHT_OES ? rbs[cur_rb].h
         : pname == GL_RENDERBUFFER_INTERNAL_FORMAT_OES ? (GLint)GL_RGBA : 0;
}
static void g_glGetFramebufferAttachmentParameterivOES(GLenum t, GLenum a, GLenum p, GLint *v) { *v = 0; }
static void g_glDeleteFramebuffersOES(GLsizei n, const GLuint *ids) {
    for (int i = 0; i < n; i++)
        if (ids[i] < MAX_NAMES && fbs[ids[i]].vgl) {
            glDeleteFramebuffers(1, &fbs[ids[i]].vgl);
            fbs[ids[i]].vgl = 0;
        }
}
static void g_glNoop(void) {}
static GLboolean g_glIsFramebufferOES(GLuint fb) { return fb && fb < next_fb; }

// Color de borrado del juego: el present usa negro para la pantalla y después lo restaura (el juego lo fija una
// vez y borra cada frame confiando en el estado: Shrek Kart, fondo blanco del logo de Gameloft).
static GLfloat game_cc[4];
static void t_glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    if (trace_frame)
        l_info("TRACE ClearColor %.2f %.2f %.2f %.2f", r, g, b, a);
    game_cc[0] = r, game_cc[1] = g, game_cc[2] = b, game_cc[3] = a;
    glClearColor(r, g, b, a);
}
static void t_glClearColorx(GLfixed r, GLfixed g, GLfixed b, GLfixed a) {
    t_glClearColor(r / 65536.0f, g / 65536.0f, b / 65536.0f, a / 65536.0f);
}
static void t_glClear(GLbitfield m) {
    if (trace_frame)
        l_info("TRACE Clear 0x%X", m);
    glClear(m);
}
static void t_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    if (trace_frame)
        l_info("TRACE Viewport %d %d %d %d", x, y, w, h);
    glViewport(x, y, w, h);
}
static void t_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    if (trace_frame)
        l_info("TRACE ColorMask %d %d %d %d", r, g, b, a);
    glColorMask(r, g, b, a);
}

// ---------------------------------------------------------------- init / present

void gles_set_size(int w, int h) {
    if (w <= 0 || h <= 0)
        return;
    if (scr_fbo && w == fb_w && h == fb_h)
        return;
    fb_w = w, fb_h = h;
    if (!scr_tex)
        glGenTextures(1, &scr_tex);
    glBindTexture(GL_TEXTURE_2D, scr_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fb_w, fb_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (!scr_fbo)
        glGenFramebuffers(1, &scr_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, scr_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, scr_tex, 0);
    if (!scr_depth)
        glGenRenderbuffers(1, &scr_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, scr_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, fb_w, fb_h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, scr_depth);
    glViewport(0, 0, fb_w, fb_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glClearColor(game_cc[0], game_cc[1], game_cc[2], game_cc[3]);
    l_info("vitaGL: drawable %dx%d -> pantalla %dx%d (rotación %d)", fb_w, fb_h, SCR_W, SCR_H, gles_rotation());
}

void gles_init(void) {
    vglUseTripleBuffering(GL_FALSE);
    vglInitExtended(0, SCR_W, SCR_H, GLES_POOL_MB * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
    gles_set_size(fb_w, fb_h);
}

// Rotación del drawable a la pantalla: 0 = sin girar (drawable apaisado o juego vertical
// centrado), 1 = landscape-right (borde superior del drawable a la izquierda), 2 = landscape-left.
int port_rotation = PORT_ROTATION; // input.c puede cambiarla (SELECT+START) y la guarda en rotation.txt
extern int ui_status_portrait(void);
// ponytail: retrato según la barra de estado; si otro juego la pone en retrato y dibuja girado, condicionar por juego.
int gles_rotation(void) { return fb_w > fb_h || ui_status_portrait() ? 0 : port_rotation; }

// Rectángulo destino en pantalla (para input.c: mismo mapeo inverso).
void gles_dest_rect(int *x, int *y, int *w, int *h) {
    int rw = gles_rotation() ? fb_h : fb_w, rh = gles_rotation() ? fb_w : fb_h; // tamaño visto apaisado
#if PORT_STRETCH
    (void)rw, (void)rh;
    *x = 0, *y = 0, *w = SCR_W, *h = SCR_H;
#else
    int dw = SCR_H * rw / rh;
    if (dw > SCR_W)
        dw = SCR_W;
    *w = dw, *h = SCR_H;
    *x = (SCR_W - dw) / 2, *y = 0;
#endif
}

// ---------------------------------------------------------------- diagnóstico (pantalla blanca, corrida 010)
// Primeras subidas de textura con su glGetError, y captura del FBO del juego a LOGS_PATH/frame_N.tga.
// ponytail: quitar cuando la imagen se vea bien.
static int tex_logs;
static void g_glTexImage2D(GLenum t, GLint lv, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum fmt, GLenum ty,
                           const void *px) {
    PROF(P_TEX, glTexImage2D(t, lv, ifmt, w, h, b, fmt, ty, px));
    if (tex_logs < 60) {
        tex_logs++;
        l_info("glTexImage2D lv%d %dx%d ifmt 0x%X fmt 0x%X type 0x%X px %p -> err 0x%X", lv, w, h, ifmt, fmt, ty, px,
               glGetError());
    }
}
static void g_glCompressedTexImage2D(GLenum t, GLint lv, GLenum ifmt, GLsizei w, GLsizei h, GLint b, GLsizei sz,
                                     const void *px) {
    PROF(P_TEX, glCompressedTexImage2D(t, lv, ifmt, w, h, b, sz, px));
    if (tex_logs < 60) {
        tex_logs++;
        l_info("glCompressedTexImage2D lv%d %dx%d ifmt 0x%X size %d -> err 0x%X", lv, w, h, ifmt, sz, glGetError());
    }
}
static void dump_frame(int lit) {
    if (!PORT_GL_DIAG)
        return;
    static int n, nlit;
    static uint64_t next = 10000000;
    uint64_t now = sceKernelGetProcessTimeWide();
    if (!lit && (n >= (PORT_TRACE_EVERY ? 12 : 3) || now < next))
        return;
    if (!lit)
        next = now + 10000000;
    uint8_t *px = malloc(fb_w * fb_h * 4);
    if (!px)
        return;
    glBindFramebuffer(GL_FRAMEBUFFER, scr_fbo);
    glReadPixels(0, 0, fb_w, fb_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    char path[128];
    if (lit)
        snprintf(path, sizeof(path), LOGS_PATH "frame_lit_%d.tga", nlit++);
    else
        snprintf(path, sizeof(path), LOGS_PATH "frame_%d.tga", n++);
    FILE *f = fopen(path, "wb");
    if (f) {
        uint8_t h[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, fb_w & 255, fb_w >> 8, fb_h & 255, fb_h >> 8, 32, 8};
        fwrite(h, 1, 18, f); // TGA de abajo hacia arriba = mismo orden que glReadPixels
        for (int i = 0; i < fb_w * fb_h; i++) {
            uint8_t *p = px + i * 4, t = p[0];
            p[0] = p[2], p[2] = t; // RGBA -> BGRA
        }
        fwrite(px, 4, fb_w * fb_h, f);
        fclose(f);
        l_info("captura %s", path);
    }
    free(px);
}

void gles_present(void) {
    static int presents, lit_at;
    dump_frame(trace_frame && lit_state > 1);
    if (trace_frame)
        l_info("TRACE fin del frame (%d draws)", trace_n), trace_frame = 0;
    if (lit_state == 1 || ((lit_state == 2 || lit_state == 3) && presents == lit_at + 150)) { // traza el siguiente
        lit_at = presents, lit_state++, lit_full = 0;
        trace_frame = 1, trace_n = 0, trace_max = 120, l_info("TRACE inicio del frame 3D %d", presents + 1);
    }
    if (++presents == 450 * PORT_GL_DIAG // ~33 s a 14 fps: ya en el menú
#if PORT_TRACE_EVERY
        || (presents > 450 && presents % PORT_TRACE_EVERY == 0 && presents <= 450 + 8 * PORT_TRACE_EVERY)
#endif
    )
        trace_frame = 1, trace_n = 0, l_info("TRACE inicio del frame %d", presents);
    static const GLenum caps[] = {GL_BLEND, GL_ALPHA_TEST, GL_SCISSOR_TEST, GL_DEPTH_TEST, GL_CULL_FACE, GL_FOG,
                                  GL_LIGHTING, GL_TEXTURE_2D};
    enum { NCAPS = sizeof(caps) / sizeof(caps[0]) };
    GLboolean on[NCAPS], tca, va, ca, na;
    GLint tex, mode, active, vp[4];
    for (int i = 0; i < NCAPS; i++)
        on[i] = glIsEnabled(caps[i]);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glActiveTexture(GL_TEXTURE0);
    glClientActiveTexture(GL_TEXTURE0);
    tca = glIsEnabled(GL_TEXTURE_COORD_ARRAY);
    va = glIsEnabled(GL_VERTEX_ARRAY);
    ca = glIsEnabled(GL_COLOR_ARRAY);
    na = glIsEnabled(GL_NORMAL_ARRAY);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    glGetIntegerv(GL_MATRIX_MODE, &mode);
    glGetIntegerv(GL_VIEWPORT, vp);
    if (bound_array_buffer)
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    if (bound_element_buffer)
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, SCR_W, SCR_H);
    for (int i = 0; i < NCAPS - 1; i++)
        glDisable(caps[i]);
    glEnable(GL_TEXTURE_2D);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glColor4f(1, 1, 1, 1);
    glBindTexture(GL_TEXTURE_2D, scr_tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    // matrices guardadas a mano: un glPushMatrix acá desborda la pila de 2 niveles de vitaGL (ver g_glPushMatrix)
    GLfloat saved_tm[16], saved_pm[16], saved_mv[16];
    glGetFloatv(GL_TEXTURE_MATRIX, saved_tm);
    glGetFloatv(GL_PROJECTION_MATRIX, saved_pm);
    glGetFloatv(GL_MODELVIEW_MATRIX, saved_mv);
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(0, SCR_W, SCR_H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(game_cc[0], game_cc[1], game_cc[2], game_cc[3]);
    int dx, dy, dw, dh;
    gles_dest_rect(&dx, &dy, &dw, &dh);
    // Esquinas de pantalla (sup-izq, sup-der, inf-izq, inf-der) -> UV del FBO (v = 0 abajo en GL).
    static const float uv_rot[3][8] = {
        {0, 1, 1, 1, 0, 0, 1, 0}, // sin girar
        {1, 1, 1, 0, 0, 1, 0, 0}, // landscape-right (probado en Inotia2)
        {0, 0, 0, 1, 1, 0, 1, 1}, // landscape-left
    };
    const float xy[] = {dx, dy, dx + dw, dy, dx, dy + dh, dx + dw, dy + dh};
    glVertexPointer(2, GL_FLOAT, 0, xy);
    glTexCoordPointer(2, GL_FLOAT, 0, uv_rot[gles_rotation()]);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    PROF(P_SWAP, vglSwapBuffers(ime_active()));
    static uint64_t prof_t0, prof_frames;
    prof_frames++;
    if (!prof_t0)
        prof_t0 = now_us();
    else if (now_us() - prof_t0 > 5000000) {
        double f = prof_frames, tot = (now_us() - prof_t0) / 1000.0 / f;
        l_info("PERF %.1f fps | ms/frame: total %.1f conv %.1f skin %.1f(%u, %u vtx) draw %.1f(%u) buf %.1f(%u, %u KB) tex %.1f(%u) "
               "swap %.1f | resto (juego) %.1f",
               1000.0 / tot, tot, prof[P_CONV] / 1e3 / f, prof[P_SKIN] / 1e3 / f, (unsigned)(prof_n[P_SKIN] / f), (unsigned)(skin_vtx / f),
               prof[P_DRAW] / 1e3 / f, (unsigned)(prof_n[P_DRAW] / f), prof[P_BUF] / 1e3 / f, (unsigned)(prof_n[P_BUF] / f),
               (unsigned)(prof_bytes / 1024 / f), prof[P_TEX] / 1e3 / f, (unsigned)(prof_n[P_TEX] / f), prof[P_SWAP] / 1e3 / f,
               tot - (prof[P_CONV] + prof[P_SKIN] + prof[P_DRAW] + prof[P_BUF] + prof[P_TEX] + prof[P_SWAP]) / 1e3 / f);
        if (envc_n)
            l_info("ENVCOLOR alfa %g .. %g (%u llamadas)", envc_min, envc_max, envc_n);
        envc_min = 1e9f, envc_max = -1e9f, envc_n = 0;
        memset(prof, 0, sizeof(prof)), memset(prof_n, 0, sizeof(prof_n)), prof_bytes = 0, skin_vtx = 0;
        prof_frames = 0, prof_t0 = now_us();
    }

    glLoadMatrixf(saved_mv);
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(saved_pm);
    glMatrixMode(GL_TEXTURE);
    glLoadMatrixf(saved_tm);
    glMatrixMode(mode);
    bind_fb(cur_fb);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, env_mode[0]);
    for (int i = 0; i < NCAPS; i++)
        on[i] ? glEnable(caps[i]) : glDisable(caps[i]);
    if (!va)
        glDisableClientState(GL_VERTEX_ARRAY);
    if (!tca)
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    if (ca)
        glEnableClientState(GL_COLOR_ARRAY);
    if (na)
        glEnableClientState(GL_NORMAL_ARRAY);
    glColor4x(color[0], color[1], color[2], color[3]);
    if (bound_array_buffer)
        glBindBuffer(GL_ARRAY_BUFFER, bound_array_buffer);
    if (bound_element_buffer)
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bound_element_buffer);
    for (int k = 0; k < A_N; k++)
        if (!NEEDS_CONV(k))
            set_pointer(k, 0, 0);
    glClientActiveTexture(GL_TEXTURE0 + client_unit);
    glActiveTexture(active);
}

// ---------------------------------------------------------------- tabla

const import_entry gles_imports[] = {
    F(glActiveTexture, e_glActiveTexture), F(glAlphaFunc, e_glAlphaFunc), D(glAlphaFuncx), F(glBindTexture, e_glBindTexture), F(glBlendFunc, e_glBlendFunc),
    F(glClearColor, t_glClearColor), F(glClearColorx, t_glClearColorx), D(glClearDepthf), D(glClearDepthx), D(glClearStencil), F(glColorMask, t_glColorMask),
    F(glCompressedTexImage2D, g_glCompressedTexImage2D), D(glCopyTexImage2D), D(glCopyTexSubImage2D),
    F(glCullFace, e_glCullFace), D(glDeleteTextures), F(glDepthFunc, e_glDepthFunc), D(glDepthMask), D(glDepthRangef), D(glDepthRangex),
    D(glFinish), D(glFlush), F(glFogf, e_glFogf), F(glFogfv, e_glFogfv), D(glFogx), D(glFogxv), F(glFrontFace, e_glFrontFace), D(glFrustumf),
    D(glFrustumx), D(glGenBuffers), D(glGenTextures), D(glGetError), F(glGetFloatv, e_glGetFloatv), D(glGetBooleanv),
    D(glHint), D(glIsEnabled), D(glIsTexture), D(glLightModelfv), D(glLightModelxv),
    F(glLightf, g_glLightf), F(glLightfv, e_glLightfv), D(glLightxv), D(glLineWidth), D(glLineWidthx), F(glLoadIdentity, g_glLoadIdentity), F(glLoadMatrixf, g_glLoadMatrixf),
    F(glLoadMatrixx, g_glLoadMatrixx), F(glMaterialf, e_glMaterialf), F(glMaterialfv, e_glMaterialfv), D(glMaterialx), F(glMatrixMode, g_glMatrixMode), F(glMultMatrixf, g_glMultMatrixf),
    F(glMultMatrixx, g_glMultMatrixx), D(glNormal3f), D(glOrthof), D(glOrthox), F(glPixelStorei, e_glPixelStorei), D(glPointSize), D(glPolygonOffset),
    F(glPopMatrix, g_glPopMatrix), F(glPushMatrix, g_glPushMatrix), D(glReadPixels), F(glRotatef, g_glRotatef), F(glRotatex, g_glRotatex), F(glScalef, g_glScalef),
    F(glScalex, g_glScalex), D(glScissor), D(glShadeModel), D(glStencilFunc), D(glStencilMask), D(glStencilOp), F(glTexImage2D, g_glTexImage2D),
    F(glTexParameteri, e_glTexParameteri), D(glTexParameteriv), D(glTexSubImage2D), F(glTranslatef, g_glTranslatef),
    F(glTranslatex, g_glTranslatex), F(glViewport, t_glViewport), D(glBlendEquation), F(glTexEnvfv, e_glTexEnvfv), 
    D(glDeleteRenderbuffers), 
    F(glColor4x, g_glColor4x), F(glColor4f, g_glColor4f), F(glColor4ub, g_glColor4ub), F(glTexEnvi, g_glTexEnvi),
    F(glTexEnvf, g_glTexEnvf), F(glTexEnvx, g_glTexEnvx), F(glTexEnvxv, g_glTexEnvxv),
    F(glTexParameterx, g_glTexParameterx), F(glTexParameterf, g_glTexParameterf), F(glLightModelx, g_glLightModelx),
    F(glLightModelf, g_glLightModelf), F(glLightx, g_glLightx), F(glMaterialxv, g_glMaterialxv),
    F(glGetFixedv, g_glGetFixedv), F(glGetLightxv, g_glGetLightxv), F(glGetPointerv, g_glGetPointerv),
    F(glGetString, g_glGetString), F(glGetIntegerv, g_glGetIntegerv), F(glVertexPointer, g_glVertexPointer),
    F(glNormalPointer, g_glNormalPointer), F(glColorPointer, g_glColorPointer),
    F(glTexCoordPointer, g_glTexCoordPointer), F(glClientActiveTexture, g_glClientActiveTexture),
    F(glDrawArrays, g_glDrawArrays), F(glDrawElements, g_glDrawElements), F(glBindBuffer, g_glBindBuffer),
    F(glBufferData, g_glBufferData), F(glBufferSubData, g_glBufferSubData), F(glDeleteBuffers, g_glDeleteBuffers),
    F(glEnable, g_glEnable), F(glDisable, g_glDisable), F(glEnableClientState, g_glEnableClientState),
    F(glDisableClientState, g_glDisableClientState), F(glClear, t_glClear),
    F(glCurrentPaletteMatrixOES, g_glCurrentPaletteMatrixOES), F(glMatrixIndexPointerOES, g_glMatrixIndexPointerOES),
    F(glWeightPointerOES, g_glWeightPointerOES), F(glLoadPaletteFromModelViewMatrixOES, g_glLoadPaletteFromModelViewMatrixOES),
    F(glGenFramebuffersOES, g_glGenFramebuffersOES), F(glGenRenderbuffersOES, g_glGenRenderbuffersOES),
    F(glBindFramebufferOES, g_glBindFramebufferOES), F(glBindRenderbufferOES, g_glBindRenderbufferOES),
    F(glDeleteFramebuffersOES, g_glDeleteFramebuffersOES), F(glDeleteRenderbuffersOES, g_glNoop),
    F(glRenderbufferStorageOES, g_glRenderbufferStorageOES),
    F(glFramebufferRenderbufferOES, g_glFramebufferRenderbufferOES),
    F(glFramebufferTexture2DOES, g_glFramebufferTexture2DOES),
    F(glCheckFramebufferStatusOES, g_glCheckFramebufferStatusOES),
    F(glGetRenderbufferParameterivOES, g_glGetRenderbufferParameterivOES),
    F(glGetFramebufferAttachmentParameterivOES, g_glGetFramebufferAttachmentParameterivOES),
    F(glIsFramebufferOES, g_glIsFramebufferOES), F(glPointParameterf, g_glNoop), F(glPointParameterfv, g_glNoop),
    D(glClipPlanef), D(glClipPlanex), D(glVertexAttribPointer), D(glEnableVertexAttribArray),
    D(glDisableVertexAttribArray), D(glVertexAttrib4f), D(glDeleteProgram), D(glGenerateMipmap),
    F(glBlendEquationOES, glBlendEquation), F(glBlendFuncSeparateOES, glBlendFuncSeparate), F(glGenerateMipmapOES, glGenerateMipmap),
};
TABLE_END(gles)
