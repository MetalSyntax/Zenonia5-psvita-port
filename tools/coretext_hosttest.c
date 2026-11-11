// Prueba en la Mac de source/coretext.c (layout + glifos con FreeType), sin consola:
//   cc -I/opt/homebrew/include/freetype2 -DDATA_PATH='"./"' tools/coretext_hosttest.c \
//      -L/opt/homebrew/lib -lfreetype -o /tmp/cth && (cd . && /tmp/cth)
// Dibuja como CCImage::initWithString de cocos2d-x (contexto volteado, drawInRect:withFont:) y como su
// rama de borde (CTFontCreatePathForGlyph + FillPath/DrawPath), imprime ASCII y verifica con assert.
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../source/objc.h"
typedef struct {
    objc_class *isa;
    const char *s;
} hstr;
static objc_class hcls[8];
static int nh;
objc_class *objc_define(const char *n, const char *s, size_t sz, const nmethod *i, const nmethod *c) {
    return &hcls[nh++];
}
objc_class *objc_getClass(const char *n) { return &hcls[7]; }
SEL sel_registerName(const char *s) { return s; }
id objc_alloc(objc_class *c) {
    id o = calloc(1, 64);
    *(objc_class **)o = c;
    return o;
}
id objc_retain(id o) { return o; }
id objc_autorelease(id o) { return o; }
int objc_is_kind(id o, objc_class *c) { return *(objc_class **)o == c; }
const char *ns_cstr(id s) { return s ? ((hstr *)s)->s : ""; }
id ns_string_new(const char *u) {
    hstr *h = calloc(1, sizeof(*h));
    h->s = u;
    return h;
}
id ns_str(const char *u) { return ns_string_new(u); }
id ns_array_new(void) { return NULL; }
id ns_data_new(const void *b, int n) { return (id)(intptr_t)n; }
void l_info(const char *f, ...) {
    va_list a;
    va_start(a, f);
    vprintf(f, a);
    va_end(a);
    puts("");
}
void fatal_error(const char *f, ...) {
    puts(f);
    exit(1);
}

#include "../source/coretext.c"

// contexto de prueba: 8 bits de alfa, CTM como el de CCImage (T(0,h) S(1,-1))
enum { W = 120, H = 40 };
static uint8_t buf[H][W];
static struct {
    float a, b, c, d, tx, ty;
} ctm;
int cg_ctx_map(void *ctx, float ux, float uy, float *col, float *row) {
    *col = ctm.a * ux + ctm.c * uy + ctm.tx;
    *row = H - (ctm.b * ux + ctm.d * uy + ctm.ty);
    return 1;
}
void cg_ctx_cover(void *ctx, int col, int row, int cov) {
    if (col >= 0 && row >= 0 && col < W && row < H && cov > buf[row][col])
        buf[row][col] = cov;
}
void *ui_current_ctx(void) { return buf; }
static int dump(void) {
    int on = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++)
            putchar(" .:#"[buf[y][x] >> 6]), on += buf[y][x] > 128;
        puts("|");
    }
    return on;
}

int main(void) {
    coretext_init();
    ct_font *f = font_new(c_UIFont, 14);
    CGSize sz;
    s_sizeWithFontConstrained(&sz, ns_str("Hello 안녕 world"), NULL, f, (CGSize){70, 0});
    printf("sizeWithFont:constrainedToSize:(70) = %.0fx%.0f\n", sz.w, sz.h);
    assert(sz.w > 0 && sz.w <= 70 && sz.h >= 2 * 14); // tiene que partir en 2+ líneas
    ctm.a = 1, ctm.d = -1, ctm.ty = H;
    s_drawInRectFontLBMAlign(&sz, ns_str("Hello 안녕\nOK"), NULL, (CGRect){{0, 0}, {W, H}}, f, 0, 1);
    int on = dump();
    assert(on > 50);
    int top = 0; // la primera línea arriba: hay tinta en el tercio superior
    for (int y = 0; y < H / 3; y++)
        for (int x = 0; x < W; x++)
            top += buf[y][x] > 128;
    assert(top > 20);
    // rama con borde: glifo en el origen tras T(s, h-s) S(1,-1) sobre el CTM volteado => (s, s) con y hacia arriba
    memset(buf, 0, sizeof(buf));
    ctm = (typeof(ctm)){1, 0, 0, 1, 2, 10};
    uint16_t ch = 'A', g;
    assert(ct_FontGetGlyphsForCharacters(f, &ch, &g, 1));
    cg_AddPath(NULL, ct_FontCreatePathForGlyph(f, g, NULL));
    cg_SetLineWidth(NULL, 2);
    cg_DrawPath(buf, 2);
    assert(dump() > 10);
    // un " " suelto mide su avance (cocos2d mide el texto con borde carácter por carácter)
    s_sizeWithFontConstrained(&sz, ns_str(" "), NULL, f, (CGSize){0, 0});
    assert(sz.w > 0);
    // tildes y ñ salen de la fuente (tools/merge_latin_font.py), no del glifo vacío .notdef
    uint16_t acc[3] = {0xF3, 0xF1, 0xC1}, ga[3];
    assert(ct_FontGetGlyphsForCharacters(f, acc, ga, 3));
    printf("ascent %.1f descent %.1f leading %.1f\n", ct_FontGetAscent(f), ct_FontGetDescent(f), ct_FontGetLeading(f));
    puts("OK");
    return 0;
}
