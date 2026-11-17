// Texto: CGFont/CTFont/UIFont + NSString(UIStringDrawing) sobre FreeType con una sola fuente
// (font.ttf, NanumGothic: latín + coreano). Los IDs de glifo son los de FreeType, así que todo lo que el
// juego mide (CTFontGetGlyphsForCharacters, advances, cmap de CGFontCopyTableForTag) es coherente con lo
// que se dibuja. Clientes en Zenonia 5: CCImage::initWithString de cocos2d-x (UIKit drawInRect:withFont:,
// glifo con borde vía CTFontCreatePathForGlyph), el gestor de fuentes C++ de CCGX (métricas y advances)
// y FontLabel/ZFont (CGContextShowGlyphsAtPoint).
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_STROKER_H
#include FT_TRUETYPE_TABLES_H
#include FT_ADVANCES_H
#include <math.h>
#include <psp2/kernel/processmgr.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "foundation.h"
#include "hle.h"
#include "imports.h"
#include "log.h"
#include "objc.h"

typedef struct {
    float x, y;
} CGPoint;
typedef struct {
    float w, h;
} CGSize;
typedef struct {
    CGPoint o;
    CGSize s;
} CGRect;

int cg_ctx_map(void *ctx, float ux, float uy, float *col, float *row); // uikit.c
void cg_ctx_cover(void *ctx, int col, int row, int cov);
void *ui_current_ctx(void);

static FT_Library ft;
static FT_Face face;
static FT_Stroker stroker;
static pthread_mutex_t ft_lock = PTHREAD_MUTEX_INITIALIZER; // cocos2d arma texturas también en hilos de carga
static float cur_px = -1;

static objc_class *c_CGFont, *c_CTFont, *c_CGPath, *c_UIFont;
static struct {
    objc_class *isa;
} cgfont; // única CGFont: sin cabecera de objc_alloc, retain/release no la liberan
typedef struct {
    objc_class *isa;
    float size;
} ct_font; // CTFont y UIFont
typedef struct {
    objc_class *isa;
    int glyph;
    float size;
} cg_path; // CTFontCreatePathForGlyph: el glifo, se rasteriza en FillPath/DrawPath

static void set_px(float px) {
    if (px != cur_px) {
        FT_Set_Char_Size(face, 0, (FT_F26Dot6)(px * 64), 72, 72);
        cur_px = px;
    }
}
static float units(int v, float px) { return v * px / face->units_per_EM; }

// Dibuja un glifo (relleno o solo el borde de grosor stroke) con su origen en (ux, uy) del contexto.
// ponytail: el CTM solo traslada/voltea; escalas y rotaciones del contexto no se aplican al glifo.
// uikit: texto de UIStringDrawing, que asume un contexto con Y hacia abajo (UIGraphicsBeginImageContext). En un
// CGBitmapContext sin voltear (Inotia 4: SGL_RenderFontToAlphaImage + UIGraphicsPushContext) iOS deja el glifo
// espejado en memoria y el juego lo endereza al subirlo como textura: se espeja igual que iOS.
// Caché de glifos rasterizados (sin borde): el juego vuelve a dibujar los mismos textos cada frame y FreeType
// renderizaba cada letra desde cero (PERF "texto": 3-8 ms/frame en Inotia 4). Se llena una vez y no se vacía.
typedef struct {
    int gid, px64; // gid -1 = libre
    short left, top;
    unsigned short w, h;
    float adv;
    uint8_t *buf;
} glyph_cached;
#define GCACHE 2048
static glyph_cached gcache[GCACHE];
static int gcache_init;
static glyph_cached *gcache_slot(int gid, int px64) {
    if (!gcache_init) {
        for (int i = 0; i < GCACHE; i++)
            gcache[i].gid = -1;
        gcache_init = 1;
    }
    unsigned h = ((unsigned)gid * 2654435761u ^ (unsigned)px64 * 40503u) % GCACHE;
    for (int k = 0; k < 8; k++, h = (h + 1) % GCACHE)
        if (gcache[h].gid == gid && gcache[h].px64 == px64)
            return &gcache[h];
        else if (gcache[h].gid < 0)
            return &gcache[h]; // libre: el llamador lo llena
    return NULL; // ponytail: zona llena -> sin caché para ese glifo
}
static void blit_glyph(void *ctx, const glyph_cached *g, float col, float row, int mirror) {
    int x0 = (int)lroundf(col) + g->left, base = (int)lroundf(row);
    for (unsigned y = 0; y < g->h; y++)
        for (unsigned x = 0; x < g->w; x++)
            if (g->buf[y * g->w + x])
                cg_ctx_cover(ctx, x0 + x, mirror ? base + g->top - 1 - (int)y : base - g->top + (int)y, g->buf[y * g->w + x]);
}

static float draw_glyph(void *ctx, int gid, float px, float ux, float uy, float stroke, int uikit) {
    float col, row, c2, r2;
    glyph_cached *gc = stroke > 0 ? NULL : gcache_slot(gid, (int)lroundf(px * 64));
    if (gc && gc->gid == gid) {
        if (ctx && cg_ctx_map(ctx, ux, uy, &col, &row))
            blit_glyph(ctx, gc, col, row, uikit && cg_ctx_map(ctx, ux, uy + 1, &c2, &r2) && r2 < row);
        return gc->adv;
    }
    set_px(px);
    if (FT_Load_Glyph(face, gid, FT_LOAD_NO_BITMAP))
        return 0;
    float adv = face->glyph->advance.x / 64.0f;
    if (!ctx || !cg_ctx_map(ctx, ux, uy, &col, &row))
        return adv;
    FT_Glyph g;
    if (FT_Get_Glyph(face->glyph, &g))
        return adv;
    if (stroke > 0) {
        FT_Stroker_Set(stroker, (FT_Fixed)(stroke * 32), FT_STROKER_LINECAP_ROUND, FT_STROKER_LINEJOIN_ROUND, 0);
        FT_Glyph_Stroke(&g, stroker, 1);
    }
    if (!FT_Glyph_To_Bitmap(&g, FT_RENDER_MODE_NORMAL, NULL, 1)) {
        FT_BitmapGlyph b = (FT_BitmapGlyph)g;
        if (gc && (gc->buf = malloc(b->bitmap.width * b->bitmap.rows + 1))) {
            for (unsigned y = 0; y < b->bitmap.rows; y++)
                memcpy(gc->buf + y * b->bitmap.width, b->bitmap.buffer + y * b->bitmap.pitch, b->bitmap.width);
            gc->left = b->left, gc->top = b->top, gc->w = b->bitmap.width, gc->h = b->bitmap.rows, gc->adv = adv;
            gc->px64 = (int)lroundf(px * 64), gc->gid = gid;
        }
        int mirror = uikit && cg_ctx_map(ctx, ux, uy + 1, &c2, &r2) && r2 < row; // Y del usuario hacia arriba
        int x0 = (int)lroundf(col) + b->left, base = (int)lroundf(row);
        for (unsigned y = 0; y < b->bitmap.rows; y++)
            for (unsigned x = 0; x < b->bitmap.width; x++)
                cg_ctx_cover(ctx, x0 + x, mirror ? base + b->top - 1 - (int)y : base - b->top + (int)y,
                             b->bitmap.buffer[y * b->bitmap.pitch + x]);
    }
    FT_Done_Glyph(g);
    return adv;
}

static int utf8_next(const unsigned char **p) {
    const unsigned char *s = *p;
    int c = *s++, n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    c &= n == 3 ? 7 : n == 2 ? 15 : n == 1 ? 31 : 127;
    while (n-- && (*s & 0xC0) == 0x80)
        c = c << 6 | (*s++ & 63);
    *p = s;
    return c;
}

// Avance por (carácter, tamaño), caché de mapeo directo: hay juegos que miden un texto largo prefijo por
// prefijo (Inotia 4: TEXTCTRL2_LineProcess -> sizeWithFont: por cada carácter) y sin caché eran millones de
// FT_Load_Glyph. Llamar con ft_lock tomado y set_px(px) ya hecho.
static struct {
    int cp;
    float px, adv;
} adv_cache[4096];
static float glyph_adv(int cp, float px) {
    unsigned h = ((unsigned)cp * 2654435761u ^ (unsigned)(px * 16)) & 4095;
    if (adv_cache[h].cp == cp && adv_cache[h].px == px)
        return adv_cache[h].adv;
    float a = 0;
    if (!FT_Load_Glyph(face, FT_Get_Char_Index(face, cp), FT_LOAD_NO_BITMAP))
        a = face->glyph->advance.x / 64.0f;
    adv_cache[h].cp = cp;
    adv_cache[h].px = px;
    adv_cache[h].adv = a;
    return a;
}

// Layout de UIKit: líneas por '\n' y ajuste a maxw (por palabra; si no hay espacio, por carácter).
// Con ctx dibuja en r (origen arriba-izquierda, alineación 0 izq / 1 centro / 2 der). Devuelve el tamaño.
// Tiempo de texto del sistema por frame (línea PERF de gles.c): medir/dibujar con UIKit cuesta CPU.
uint64_t text_us;
unsigned text_n;
static void layout(CGSize *ret, const char *text, float px, float maxw, float maxh, int align, void *ctx, CGRect r) {
    uint64_t t0 = sceKernelGetProcessTimeWide();
    pthread_mutex_lock(&ft_lock);
    set_px(px);
    float asc = face->size->metrics.ascender / 64.0f, lh = face->size->metrics.height / 64.0f, maxlw = 0;
    int n = 0, cap = strlen(text) + 1, lines = 0;
    int *cp = malloc(cap * sizeof(int));
    float *adv = malloc(cap * sizeof(float));
    for (const unsigned char *p = (const unsigned char *)text; *p;) {
        cp[n] = utf8_next(&p);
        adv[n] = cp[n] != '\n' ? glyph_adv(cp[n], px) : 0;
        n++;
    }
    for (int i = 0; i < n || (i == 0 && n == 0);) {
        int start = i, end = i, brk = -1;
        float w = 0;
        while (end < n && cp[end] != '\n') {
            if (maxw > 0 && w + adv[end] > maxw + 0.5f && end > start)
                break;
            if (cp[end] == ' ')
                brk = end;
            w += adv[end++];
        }
        int next = end;
        if (end < n && cp[end] != '\n' && brk > start)
            end = brk, next = brk + 1; // corta en el último espacio
        else if (end < n && cp[end] == '\n')
            next = end + 1;
        w = 0;
        // Los espacios finales cuentan, como en sizeWithFont: de UIKit: el texto con borde de cocos2d mide
        // carácter por carácter y un " " de ancho 0 dejaba las palabras pegadas. Al cortar una línea
        // ajustada el espacio del corte ya quedó afuera (end = brk).
        for (int k = start; k < end; k++)
            w += adv[k];
        if (maxh > 0 && lines > 0 && (lines + 1) * lh > maxh + 0.5f)
            break; // UIKit no dibuja líneas que no entran
        if (w > maxlw)
            maxlw = w;
        if (ctx) {
            float x = r.o.x + (align == 1 ? (r.s.w - w) / 2 : align == 2 ? r.s.w - w : 0);
            for (int k = start; k < end; k++)
                x += cp[k] == ' ' ? adv[k] : draw_glyph(ctx, FT_Get_Char_Index(face, cp[k]), px, x, r.o.y + asc + lines * lh, 0, 1);
        }
        lines++;
        if (n == 0 || next >= n)
            break;
        i = next;
    }
    free(cp);
    free(adv);
    pthread_mutex_unlock(&ft_lock);
    text_us += sceKernelGetProcessTimeWide() - t0, text_n++;
    *ret = (CGSize){ceilf(maxlw), ceilf(lines * lh)};
}

// ================================================================ CGFont (unidades de la fuente)

static void *cg_FontCreateWithFontName(id name) {
    static int logged;
    if (!logged++)
        l_info("CGFontCreateWithFontName(%s) -> font.ttf (todas las fuentes)", ns_cstr(name));
    return &cgfont;
}
static void *cg_DataProviderCreateWithURL(id url) { return (void *)1; }
static void *cg_FontCreateWithDataProvider(void *p) { return &cgfont; }
static void *cg_FontRetain(void *f) { return f; }
static void cg_FontRelease(void *f) {}
static int cg_FontGetUnitsPerEm(void *f) { return face->units_per_EM; }
static int cg_FontGetAscent(void *f) { return face->ascender; }
static int cg_FontGetDescent(void *f) { return face->descender; } // negativo, como en CG
static int os2(int cap) {
    TT_OS2 *t = FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
    int v = t && t->version >= 2 ? (cap ? t->sCapHeight : t->sxHeight) : 0;
    return v ? v : face->units_per_EM * (cap ? 7 : 5) / 10;
}
static int cg_FontGetCapHeight(void *f) { return os2(1); }
static int cg_FontGetXHeight(void *f) { return os2(0); }
static int cg_FontGetGlyphAdvances(void *f, const uint16_t *g, size_t n, int *adv) {
    pthread_mutex_lock(&ft_lock);
    for (size_t i = 0; i < n; i++) {
        FT_Fixed a = 0;
        FT_Get_Advance(face, g[i], FT_LOAD_NO_SCALE, &a);
        adv[i] = a;
    }
    pthread_mutex_unlock(&ft_lock);
    return 1;
}
static int cg_FontGetGlyphBBoxes(void *f, const uint16_t *g, size_t n, CGRect *bb) {
    pthread_mutex_lock(&ft_lock);
    for (size_t i = 0; i < n; i++) {
        bb[i] = (CGRect){{0, 0}, {0, 0}};
        if (!FT_Load_Glyph(face, g[i], FT_LOAD_NO_SCALE)) {
            FT_Glyph_Metrics *m = &face->glyph->metrics;
            bb[i] = (CGRect){{m->horiBearingX, m->horiBearingY - m->height}, {m->width, m->height}};
        }
    }
    cur_px = -1; // NO_SCALE no toca el tamaño, pero por las dudas
    pthread_mutex_unlock(&ft_lock);
    return 1;
}
static id cg_FontCopyTableForTag(void *f, uint32_t tag) {
    FT_ULong len = 0;
    if (FT_Load_Sfnt_Table(face, tag, 0, NULL, &len) || !len)
        return NULL;
    void *b = malloc(len);
    FT_Load_Sfnt_Table(face, tag, 0, b, &len);
    id d = ns_data_new(b, len);
    free(b);
    return d;
}

// ================================================================ CTFont (píxeles al tamaño de la fuente)

static ct_font *font_new(objc_class *c, float size) {
    ct_font *f = objc_alloc(c);
    f->size = size > 0 ? size : 12;
    return f;
}
static ct_font *ct_FontCreateWithGraphicsFont(void *cg, float size, void *matrix, void *attrs) {
    return font_new(c_CTFont, size);
}
static ct_font *ct_FontCreateForString(ct_font *cur, id s, int loc, int len) { return objc_retain(cur); }
static id ct_FontCopyDisplayName(ct_font *f) { return ns_string_new("NanumGothic"); }
static float ct_FontGetSize(ct_font *f) { return f->size; }
static float ct_FontGetAscent(ct_font *f) { return units(face->ascender, f->size); }
static float ct_FontGetDescent(ct_font *f) { return units(-face->descender, f->size); }
static float ct_FontGetLeading(ct_font *f) {
    float l = units(face->height - face->ascender + face->descender, f->size);
    return l > 0 ? l : 0;
}
static float ct_FontGetCapHeight(ct_font *f) { return units(os2(1), f->size); }
static float ct_FontGetXHeight(ct_font *f) { return units(os2(0), f->size); }
static float ct_FontGetUnderlinePosition(ct_font *f) { return units(face->underline_position, f->size); }
// ponytail: UTF-16 sin pares sustitutos (el juego no usa caracteres fuera del BMP)
static int ct_FontGetGlyphsForCharacters(ct_font *f, const uint16_t *ch, uint16_t *g, int n) {
    int all = 1;
    for (int i = 0; i < n; i++)
        all &= (g[i] = FT_Get_Char_Index(face, ch[i])) != 0;
    return all;
}
static double ct_FontGetAdvancesForGlyphs(ct_font *f, int orient, const uint16_t *g, CGSize *adv, int n) {
    double total = 0;
    pthread_mutex_lock(&ft_lock);
    set_px(f->size);
    for (int i = 0; i < n; i++) {
        float a = FT_Load_Glyph(face, g[i], FT_LOAD_NO_BITMAP) ? 0 : face->glyph->advance.x / 64.0f;
        if (adv)
            adv[i] = (CGSize){a, 0};
        total += a;
    }
    pthread_mutex_unlock(&ft_lock);
    return total;
}
static cg_path *ct_FontCreatePathForGlyph(ct_font *f, uint16_t g, void *matrix) {
    cg_path *p = objc_alloc(c_CGPath);
    p->glyph = g, p->size = f->size;
    return p;
}

// ================================================================ CGContext: texto y trazos de glifo

static cg_path *pending; // CGContextAddPath -> FillPath/DrawPath (ponytail: un camino, sin contexto por hilo)
static float line_w = 1, text_px = 12;
static void cg_BeginPath(void *c) { pending = NULL; }
static void cg_AddPath(void *c, cg_path *p) { pending = objc_is_kind(p, c_CGPath) ? p : NULL; }
static void cg_SetLineWidth(void *c, float w) { line_w = w; }
static void path_draw(void *c, float stroke) {
    if (!pending)
        return;
    pthread_mutex_lock(&ft_lock);
    draw_glyph(c, pending->glyph, pending->size, 0, 0, stroke, 0);
    pthread_mutex_unlock(&ft_lock);
}
static void cg_FillPath(void *c) { path_draw(c, 0); }
static void cg_DrawPath(void *c, int mode) { // 0 fill 1 eoFill 2 stroke 3 fillStroke 4 eoFillStroke
    if (mode != 2)
        path_draw(c, 0);
    if (mode >= 2)
        path_draw(c, line_w);
}
// ponytail: UIColor no guarda color en este runtime: relleno y borde usan el color de relleno del contexto
// (cocos2d pinta el texto en blanco y lo tiñe en GL).
static void cg_noop(void) {}
static void cg_SetFont(void *c, void *f) {}
static void cg_SetFontSize(void *c, float px) { text_px = px; }
static void cg_ShowGlyphsAtPoint(void *c, float x, float y, const uint16_t *g, size_t n) {
    pthread_mutex_lock(&ft_lock);
    for (size_t i = 0; i < n; i++)
        x += draw_glyph(c, g[i], text_px, x, y, 0, 0);
    pthread_mutex_unlock(&ft_lock);
}

static CGRect k_CGRectNull = {{INFINITY, INFINITY}, {0, 0}};

const import_entry coretext_imports[] = {
    F(CGFontCreateWithFontName, cg_FontCreateWithFontName), F(CGDataProviderCreateWithURL, cg_DataProviderCreateWithURL),
    F(CGFontCreateWithDataProvider, cg_FontCreateWithDataProvider), F(CGFontRetain, cg_FontRetain),
    F(CGFontRelease, cg_FontRelease), F(CGFontGetUnitsPerEm, cg_FontGetUnitsPerEm), F(CGFontGetAscent, cg_FontGetAscent),
    F(CGFontGetDescent, cg_FontGetDescent), F(CGFontGetCapHeight, cg_FontGetCapHeight),
    F(CGFontGetXHeight, cg_FontGetXHeight), F(CGFontGetGlyphAdvances, cg_FontGetGlyphAdvances),
    F(CGFontGetGlyphBBoxes, cg_FontGetGlyphBBoxes), F(CGFontCopyTableForTag, cg_FontCopyTableForTag),
    F(CTFontCreateWithGraphicsFont, ct_FontCreateWithGraphicsFont), F(CTFontCreateForString, ct_FontCreateForString),
    F(CTFontCopyDisplayName, ct_FontCopyDisplayName), F(CTFontGetSize, ct_FontGetSize),
    F(CTFontGetAscent, ct_FontGetAscent), F(CTFontGetDescent, ct_FontGetDescent), F(CTFontGetLeading, ct_FontGetLeading),
    F(CTFontGetCapHeight, ct_FontGetCapHeight), F(CTFontGetXHeight, ct_FontGetXHeight),
    F(CTFontGetUnderlinePosition, ct_FontGetUnderlinePosition),
    F(CTFontGetGlyphsForCharacters, ct_FontGetGlyphsForCharacters),
    F(CTFontGetAdvancesForGlyphs, ct_FontGetAdvancesForGlyphs), F(CTFontCreatePathForGlyph, ct_FontCreatePathForGlyph),
    F(CGContextBeginPath, cg_BeginPath), F(CGContextAddPath, cg_AddPath), F(CGContextSetLineWidth, cg_SetLineWidth),
    F(CGContextFillPath, cg_FillPath), F(CGContextDrawPath, cg_DrawPath), F(CGContextSetFont, cg_SetFont),
    F(CGContextSetFontSize, cg_SetFontSize), F(CGContextShowGlyphsAtPoint, cg_ShowGlyphsAtPoint),
    F(CGContextSetTextMatrix, cg_noop), F(CGContextSetAllowsFontSmoothing, cg_noop),
    F(CGContextSetFillColorWithColor, cg_noop), F(CGContextSetStrokeColorWithColor, cg_noop),
    V(CGRectNull, &k_CGRectNull),
};
TABLE_END(coretext)

// ================================================================ UIFont + NSString(UIStringDrawing)

#define AR(x) objc_autorelease((id)(x))
static id uf_fontWithNameSize(objc_class *cls, SEL _c, id name, float size) { return AR(font_new(c_UIFont, size)); }
static id uf_systemFontOfSize(objc_class *cls, SEL _c, float size) { return AR(font_new(c_UIFont, size)); }
static id uf_empty(objc_class *cls, SEL _c) { return AR(ns_array_new()); }
static id uf_fontWithSize(ct_font *f, SEL _c, float size) { return AR(font_new(c_UIFont, size)); }
static id uf_fontName(ct_font *f, SEL _c) { return ns_str("NanumGothic"); }
static float uf_pointSize(ct_font *f, SEL _c) { return f->size; }
static float uf_ascender(ct_font *f, SEL _c) { return ct_FontGetAscent(f); }
static float uf_descender(ct_font *f, SEL _c) { return -ct_FontGetDescent(f); }
static float uf_lineHeight(ct_font *f, SEL _c) { return units(face->height, f->size); }
static float uf_capHeight(ct_font *f, SEL _c) { return ct_FontGetCapHeight(f); }
static float uf_xHeight(ct_font *f, SEL _c) { return ct_FontGetXHeight(f); }
static float uf_leading(ct_font *f, SEL _c) { return ct_FontGetLeading(f); }
static const nmethod uifont_c[] = {
    NM("fontWithName:size:", uf_fontWithNameSize), NM("systemFontOfSize:", uf_systemFontOfSize),
    NM("boldSystemFontOfSize:", uf_systemFontOfSize), NM("italicSystemFontOfSize:", uf_systemFontOfSize),
    NM("familyNames", uf_empty), NM("fontNamesForFamilyName:", uf_empty), {0}};
static const nmethod uifont_i[] = {
    NM("fontWithSize:", uf_fontWithSize), NM("fontName", uf_fontName), NM("familyName", uf_fontName),
    NM("pointSize", uf_pointSize), NM("ascender", uf_ascender), NM("descender", uf_descender),
    NM("lineHeight", uf_lineHeight), NM("capHeight", uf_capHeight), NM("xHeight", uf_xHeight),
    NM("leading", uf_leading), {0}};

static float fsize(ct_font *f) { return f && (objc_is_kind(f, c_UIFont) || objc_is_kind(f, c_CTFont)) ? f->size : 12; }
static void s_sizeWithFont(CGSize *ret, id self, SEL _c, ct_font *f) {
    layout(ret, ns_cstr(self), fsize(f), 0, 0, 0, NULL, (CGRect){{0, 0}, {0, 0}});
}
static void s_sizeWithFontConstrained(CGSize *ret, id self, SEL _c, ct_font *f, CGSize max) {
    layout(ret, ns_cstr(self), fsize(f), max.w, max.h, 0, NULL, (CGRect){{0, 0}, {0, 0}});
}
static void s_sizeWithFontConstrainedLBM(CGSize *ret, id self, SEL _c, ct_font *f, CGSize max, int lbm) {
    s_sizeWithFontConstrained(ret, self, _c, f, max);
}
static void s_drawInRectFontLBMAlign(CGSize *ret, id self, SEL _c, CGRect r, ct_font *f, int lbm, int align) {
    layout(ret, ns_cstr(self), fsize(f), r.s.w, r.s.h, align, ui_current_ctx(), r);
}
static void s_drawInRectFontLBM(CGSize *ret, id self, SEL _c, CGRect r, ct_font *f, int lbm) {
    s_drawInRectFontLBMAlign(ret, self, _c, r, f, lbm, 0);
}
static void s_drawInRectFont(CGSize *ret, id self, SEL _c, CGRect r, ct_font *f) {
    s_drawInRectFontLBMAlign(ret, self, _c, r, f, 0, 0);
}
static void s_drawAtPointFont(CGSize *ret, id self, SEL _c, CGPoint p, ct_font *f) {
    layout(ret, ns_cstr(self), fsize(f), 0, 0, 0, ui_current_ctx(), (CGRect){p, {0, 0}});
}
static const nmethod nsstring_draw[] = {
    NM("sizeWithFont:", s_sizeWithFont), NM("sizeWithFont:constrainedToSize:", s_sizeWithFontConstrained),
    NM("sizeWithFont:constrainedToSize:lineBreakMode:", s_sizeWithFontConstrainedLBM),
    NM("drawInRect:withFont:lineBreakMode:alignment:", s_drawInRectFontLBMAlign),
    NM("drawInRect:withFont:lineBreakMode:", s_drawInRectFontLBM), NM("drawInRect:withFont:", s_drawInRectFont),
    NM("drawAtPoint:withFont:", s_drawAtPointFont), {0}};

// Agrega métodos a una clase nativa ya definida (categoría UIStringDrawing de UIKit sobre NSString).
static void add_methods(objc_class *c, const nmethod *add) {
    int n = 0, k = 0;
    for (const nmethod *m = c->vtable; m && m->sel; m++)
        n++;
    while (add[k].sel)
        k++;
    nmethod *t = calloc(n + k + 1, sizeof(nmethod));
    memcpy(t, c->vtable, n * sizeof(nmethod));
    for (int i = 0; i < k; i++)
        t[n + i] = (nmethod){sel_registerName(add[i].sel), add[i].imp};
    c->vtable = t;
}

// font.ttf entero en RAM (~2 MB): con FT_New_Face cada glifo es un fseek+fread a la tarjeta, y medir un
// texto largo (Inotia 4: TEXTCTRL2 / SGL_GetTextSize) tardaba segundos por frame.
static FT_Error face_from_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return 1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n);
    if (!buf || fread(buf, 1, n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return 1;
    }
    fclose(f);
    return FT_New_Memory_Face(ft, buf, n, 0, &face); // buf vive mientras viva la face (todo el proceso)
}

void coretext_init(void) {
    if (FT_Init_FreeType(&ft) || (face_from_file(DATA_PATH "font.ttf") && face_from_file("app0:font.ttf")))
        fatal_error("No se pudo abrir font.ttf (app0: ni " DATA_PATH ")");
    FT_Stroker_New(ft, &stroker);
    l_info("coretext: font.ttf %s, %ld glifos, %d unidades/em", face->family_name, face->num_glyphs, face->units_per_EM);
    c_CGFont = objc_define("CGFont", "NSObject", sizeof(cgfont), NULL, NULL);
    cgfont.isa = c_CGFont;
    c_CTFont = objc_define("CTFont", "NSObject", sizeof(ct_font), NULL, NULL);
    c_CGPath = objc_define("CGPath", "NSObject", sizeof(cg_path), NULL, NULL);
    c_UIFont = objc_define("UIFont", "NSObject", sizeof(ct_font), uifont_i, uifont_c);
    add_methods(objc_getClass("NSString"), nsstring_draw);
}

// Texto UTF-8 en un búfer RGBA (ABGR en memoria) con font.ttf: menú de opciones del port (input.c). y = línea base.
// Devuelve el ancho dibujado.
int ct_text_rgba(uint32_t *px, int w, int h, int x, int y, float size, uint32_t abgr, const char *text) {
    pthread_mutex_lock(&ft_lock);
    set_px(size);
    int x0 = x;
    for (const unsigned char *p = (const unsigned char *)text; *p;) {
        int cp = utf8_next(&p);
        if (FT_Load_Char(face, cp, FT_LOAD_RENDER))
            continue;
        FT_GlyphSlot g = face->glyph;
        for (unsigned r = 0; r < g->bitmap.rows; r++)
            for (unsigned c = 0; c < g->bitmap.width; c++) {
                int X = x + g->bitmap_left + c, Y = y - g->bitmap_top + r;
                unsigned a = g->bitmap.buffer[r * g->bitmap.pitch + c];
                if (!a || X < 0 || Y < 0 || X >= w || Y >= h)
                    continue;
                uint32_t d = px[Y * w + X], o = 0xff000000;
                for (int sh = 0; sh < 24; sh += 8)
                    o |= ((((abgr >> sh) & 0xff) * a + ((d >> sh) & 0xff) * (255 - a)) / 255) << sh;
                px[Y * w + X] = o;
            }
        x += g->advance.x >> 6;
    }
    pthread_mutex_unlock(&ft_lock);
    return x - x0;
}
