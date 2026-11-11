// Foundation + CoreFoundation mínimos sobre el runtime de objc.c.
//
// Layouts nativos (el juego no los lee por dentro, salvo NSString = CFString constante):
//   NSString      {isa, flags, char *utf8, int len_bytes}   (= __cfstring del binario)
//   NSNumber      {isa, type, int64/double}
//   NSArray/NSSet {isa, count, cap, id *items}
//   NSDictionary  {isa, count, cap, id *keys, id *vals}
//   NSData        {isa, len, cap, uint8_t *bytes, int owned}
// Retornos: los constructores de clase devuelven autoreleased (+0); alloc/init/copy, +1.
#include <dirent.h>
#include <math.h>
#include <psp2/apputil.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/system_param.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "darwin_printf.h"
#include "foundation.h"
#include "hle.h"
#include "imports.h"
#include "log.h"
#include "macho.h"
#include "objc.h"

objc_class *c_NSObject, *c_NSString, *c_NSMutableString, *c_NSConstantString, *c_NSNumber, *c_NSArray,
    *c_NSMutableArray, *c_NSSet, *c_NSMutableSet, *c_NSDictionary, *c_NSMutableDictionary, *c_NSData,
    *c_NSMutableData, *c_NSTimer, *c_NSDate, *c_NSNotification, *c_NSURL, *c_NSError, *c_NSThread;

typedef struct {
    objc_class *isa;
    uint32_t flags;
    char *s;
    int len;
} ns_string;
typedef struct {
    objc_class *isa;
    int type; // 0 int64, 1 double, 2 bool
    union {
        int64_t i;
        double d;
    };
} ns_number;
typedef struct {
    objc_class *isa;
    int count, cap;
    id *items;
} ns_array;
typedef struct {
    objc_class *isa;
    int count, cap;
    id *keys, *vals;
} ns_dict;
typedef struct {
    objc_class *isa;
    int len, cap;
    uint8_t *bytes;
    int owned;
} ns_data;

#define SELF_STR ((ns_string *)self)
#define AR(x) objc_autorelease((id)(x))

// ================================================================ NSString

static void str_set(ns_string *s, const char *utf8, int len) {
    s->s = malloc(len + 1);
    memcpy(s->s, utf8, len);
    s->s[len] = 0;
    s->len = len;
    s->flags = 0x7C8;
}

id ns_string_new(const char *utf8) {
    ns_string *s = objc_alloc(c_NSString);
    if (!utf8)
        utf8 = "";
    str_set(s, utf8, strlen(utf8));
    return s;
}
id ns_str(const char *utf8) { return AR(ns_string_new(utf8)); }

const void *ns_data_bytes(id o, int *len) {
    if (!o || !objc_is_kind(o, c_NSData))
        return NULL;
    *len = ((ns_data *)o)->len;
    return ((ns_data *)o)->bytes;
}

const char *ns_cstr(id o) {
    if (!o)
        return "";
    if (objc_is_kind(o, c_NSString))
        return ((ns_string *)o)->s ? ((ns_string *)o)->s : "";
    return "";
}

static int utf8_next(const unsigned char **p) {
    unsigned c = *(*p)++;
    if (c < 0x80)
        return c;
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
    c &= 0x3F >> n;
    while (n-- && (**p & 0xC0) == 0x80)
        c = c << 6 | (*(*p)++ & 0x3F);
    return c;
}

static int utf16_len(const char *s) {
    int n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int c = utf8_next(&p);
        n += c >= 0x10000 ? 2 : 1;
    }
    return n;
}

static int utf8_put(char *o, unsigned c) {
    if (c < 0x80)
        return o[0] = c, 1;
    if (c < 0x800)
        return o[0] = 0xC0 | c >> 6, o[1] = 0x80 | (c & 0x3F), 2;
    if (c < 0x10000)
        return o[0] = 0xE0 | c >> 12, o[1] = 0x80 | ((c >> 6) & 0x3F), o[2] = 0x80 | (c & 0x3F), 3;
    o[0] = 0xF0 | c >> 18, o[1] = 0x80 | ((c >> 12) & 0x3F), o[2] = 0x80 | ((c >> 6) & 0x3F);
    o[3] = 0x80 | (c & 0x3F);
    return 4;
}

// UTF-16 (CFString __ustring, characters:length:) -> UTF-8 malloc
static char *utf16_to_utf8(const uint16_t *u, int n) {
    char *o = malloc(n * 3 + 1), *p = o;
    for (int i = 0; i < n; i++) {
        unsigned c = u[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n)
            c = 0x10000 + ((c - 0xD800) << 10) + (u[++i] - 0xDC00);
        p += utf8_put(p, c);
    }
    *p = 0;
    return o;
}

// Byte UTF-8 de la unidad UTF-16 número idx (para substring/characterAtIndex).
static const char *utf8_at(const char *s, int idx) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p && idx > 0) {
        int c = utf8_next(&p);
        idx -= c >= 0x10000 ? 2 : 1;
    }
    return (const char *)p;
}

static char *latin1_to_utf8(const char *b, int n) {
    char *o = malloc(n * 2 + 1), *p = o;
    for (int i = 0; i < n; i++)
        p += utf8_put(p, (unsigned char)b[i]);
    *p = 0;
    return o;
}

#define ENC_ASCII 1
#define ENC_UTF8 4
#define ENC_LATIN1 5
#define ENC_CP1252 12
#define ENC_UTF16 10

static id str_init_bytes(ns_string *s, const char *b, int n, int enc) {
    if (!b)
        b = "", n = 0;
    if (enc == ENC_LATIN1 || enc == ENC_CP1252) {
        char *u = latin1_to_utf8(b, n);
        str_set(s, u, strlen(u));
        free(u);
    } else if (enc == ENC_UTF16 || enc == 0x94000100 || enc == 0x90000100) {
        char *u = utf16_to_utf8((const uint16_t *)b, n / 2);
        str_set(s, u, strlen(u));
        free(u);
    } else {
        str_set(s, b, n);
    }
    return s;
}

static id str_vformat(const char *fmt, words *w) {
    char small[512];
    int n = darwin_vformat(small, sizeof(small), fmt, w);
    ns_string *s = objc_alloc(c_NSString);
    if (n < (int)sizeof(small)) {
        str_set(s, small, n);
    } else {
        // no se puede volver a recorrer un va_list de Darwin dos veces sin copia: alcanza con truncar
        str_set(s, small, sizeof(small) - 1);
    }
    return s;
}

static id s_string(id cls, SEL _c) { return AR(ns_string_new("")); }
static id s_stringWithUTF8String(id cls, SEL _c, const char *u) { return u ? AR(ns_string_new(u)) : NULL; }
static id s_stringWithCString_enc(id cls, SEL _c, const char *b, int enc) {
    return b ? AR(str_init_bytes(objc_alloc(c_NSString), b, strlen(b), enc)) : NULL;
}
static id s_stringWithString(id cls, SEL _c, id o) { return AR(ns_string_new(ns_cstr(o))); }
static id s_stringWithFormat(id cls, SEL _c, id fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    words w = {&ap, NULL};
    id r = str_vformat(ns_cstr(fmt), &w);
    va_end(ap);
    return AR(r);
}
static id s_stringWithCharacters(id cls, SEL _c, const uint16_t *u, int n) {
    ns_string *s = objc_alloc(c_NSString);
    char *b = utf16_to_utf8(u, n);
    str_set(s, b, strlen(b));
    free(b);
    return AR(s);
}
static id s_stringWithContentsOfFile(id cls, SEL _c, id path, int enc, id *err) {
    FILE *f = fopen(ns_cstr(path), "rb");
    if (!f) {
        if (err && (uintptr_t)err > 0x1000)
            *err = NULL;
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1);
    fread(b, 1, n, f);
    fclose(f);
    id r = str_init_bytes(objc_alloc(c_NSString), b, n, enc);
    free(b);
    return AR(r);
}

static id s_initWithUTF8String(ns_string *self, SEL _c, const char *u) {
    return u ? str_init_bytes(self, u, strlen(u), ENC_UTF8) : NULL;
}
static id s_initWithCString_enc(ns_string *self, SEL _c, const char *b, int enc) {
    return b ? str_init_bytes(self, b, strlen(b), enc) : NULL;
}
static id s_initWithString(ns_string *self, SEL _c, id o) { return str_init_bytes(self, ns_cstr(o), strlen(ns_cstr(o)), ENC_UTF8); }
static id s_initWithFormat(ns_string *self, SEL _c, id fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    words w = {&ap, NULL};
    objc_class *isa = self->isa;
    ns_string *t = str_vformat(ns_cstr(fmt), &w);
    va_end(ap);
    *self = *t; // se queda con el buffer de t
    self->isa = isa;
    t->s = NULL;
    objc_free(t);
    return self;
}
static id s_initWithFormat_args(ns_string *self, SEL _c, id fmt, const uint32_t *args) {
    words w = {NULL, args};
    objc_class *isa = self->isa;
    ns_string *t = str_vformat(ns_cstr(fmt), &w);
    *self = *t;
    self->isa = isa;
    t->s = NULL;
    objc_free(t);
    return self;
}
static id s_initWithBytes(ns_string *self, SEL _c, const char *b, int n, int enc) { return str_init_bytes(self, b, n, enc); }
static id s_initWithData(ns_string *self, SEL _c, ns_data *d, int enc) {
    return str_init_bytes(self, d ? (const char *)d->bytes : "", d ? d->len : 0, enc);
}
static id s_initWithCharacters(ns_string *self, SEL _c, const uint16_t *u, int n) {
    char *b = utf16_to_utf8(u, n);
    str_set(self, b, strlen(b));
    free(b);
    return self;
}
// Juegos iOS hacen `c = [s UTF8String]; [pool release]; fopen(c)` (HAWX: CSettings::Load): en iOS la
// memoria liberada queda intacta un rato, aquí free() pisa los primeros 8 bytes. El buffer espera 256
// deallocs de NSString antes de liberarse.
static char *s_quarantine[256];
static unsigned s_quarantine_i;
static void s_dealloc(ns_string *self, SEL _c) {
    if (!is_guest_addr((uintptr_t)self->s)) {
        unsigned i = __atomic_fetch_add(&s_quarantine_i, 1, __ATOMIC_RELAXED) % 256;
        free(__atomic_exchange_n(&s_quarantine[i], self->s, __ATOMIC_ACQ_REL));
    }
    objc_free(self);
}

static const char *s_UTF8String(id self, SEL _c) { return ns_cstr(self); }
static const char *s_cStringUsingEncoding(id self, SEL _c, int enc) {
    if (enc != ENC_LATIN1 && enc != ENC_CP1252 && enc != ENC_ASCII)
        return ns_cstr(self);
    // A Latin-1: buffer autoliberado como un NSData.
    const unsigned char *p = (const unsigned char *)ns_cstr(self);
    ns_data *d = objc_alloc(c_NSData);
    d->bytes = malloc(strlen((const char *)p) + 1);
    d->owned = 1;
    int n = 0;
    while (*p) {
        int c = utf8_next(&p);
        d->bytes[n++] = c < 256 ? c : '?';
    }
    d->bytes[n] = 0;
    d->len = n;
    AR(d);
    return (const char *)d->bytes;
}
static int s_getCString(id self, SEL _c, char *buf, int max, int enc) {
    const char *s = ns_cstr(self);
    if ((int)strlen(s) + 1 > max)
        return 0;
    strcpy(buf, s);
    return 1;
}
static int s_length(id self, SEL _c) { return utf16_len(ns_cstr(self)); }
static int s_lengthOfBytes(id self, SEL _c, int enc) { return strlen(ns_cstr(self)); }
static int s_characterAtIndex(id self, SEL _c, int i) {
    const unsigned char *p = (const unsigned char *)utf8_at(ns_cstr(self), i);
    return *p ? utf8_next(&p) : 0;
}
static void s_getCharacters(id self, SEL _c, uint16_t *out, int loc, int len) {
    const unsigned char *p = (const unsigned char *)utf8_at(ns_cstr(self), loc);
    for (int i = 0; i < len && *p; i++)
        out[i] = utf8_next(&p);
}
static int s_isEqualToString(id self, SEL _c, id o) { return o && !strcmp(ns_cstr(self), ns_cstr(o)); }
static int s_isEqual(id self, SEL _c, id o) {
    return o == self || (o && objc_is_kind(o, c_NSString) && !strcmp(ns_cstr(self), ns_cstr(o)));
}
static uint32_t s_hash(id self, SEL _c) {
    uint32_t h = 5381;
    for (const char *p = ns_cstr(self); *p; p++)
        h = h * 33 + (uint8_t)*p;
    return h;
}
static int s_compare(id self, SEL _c, id o) {
    int r = strcmp(ns_cstr(self), ns_cstr(o));
    return r < 0 ? -1 : r > 0;
}
static int s_caseInsensitiveCompare(id self, SEL _c, id o) {
    int r = strcasecmp(ns_cstr(self), ns_cstr(o));
    return r < 0 ? -1 : r > 0;
}
static int s_intValue(id self, SEL _c) { return atoi(ns_cstr(self)); }
static long long s_longLongValue(id self, SEL _c) { return atoll(ns_cstr(self)); }
static float s_floatValue(id self, SEL _c) { return strtof(ns_cstr(self), NULL); }
static double s_doubleValue(id self, SEL _c) { return strtod(ns_cstr(self), NULL); }
static int s_boolValue(id self, SEL _c) {
    const char *s = ns_cstr(self);
    while (*s == ' ')
        s++;
    return *s == 'Y' || *s == 'y' || *s == 'T' || *s == 't' || (*s >= '1' && *s <= '9');
}

static id str_cat(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    ns_string *s = objc_alloc(c_NSString);
    s->s = malloc(la + lb + 1);
    memcpy(s->s, a, la);
    memcpy(s->s + la, b, lb + 1);
    s->len = la + lb;
    s->flags = 0x7C8;
    return AR(s);
}
static id s_stringByAppendingString(id self, SEL _c, id o) { return str_cat(ns_cstr(self), ns_cstr(o)); }
static id s_stringByAppendingFormat(id self, SEL _c, id fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    words w = {&ap, NULL};
    id t = str_vformat(ns_cstr(fmt), &w);
    va_end(ap);
    id r = str_cat(ns_cstr(self), ns_cstr(t));
    objc_release(t);
    return r;
}
static id s_stringByAppendingPathComponent(id self, SEL _c, id o) {
    const char *a = ns_cstr(self), *b = ns_cstr(o);
    while (*b == '/')
        b++;
    char tmp[1024];
    size_t la = strlen(a);
    snprintf(tmp, sizeof(tmp), "%s%s%s", a, la && a[la - 1] != '/' ? "/" : "", b);
    return AR(ns_string_new(tmp));
}
static id s_stringByAppendingPathExtension(id self, SEL _c, id o) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s.%s", ns_cstr(self), ns_cstr(o));
    return AR(ns_string_new(tmp));
}
static id sub_bytes(const char *s, size_t n) {
    ns_string *r = objc_alloc(c_NSString);
    str_set(r, s, n);
    return AR(r);
}
static id s_lastPathComponent(id self, SEL _c) {
    const char *s = ns_cstr(self), *p = strrchr(s, '/');
    return AR(ns_string_new(p ? p + 1 : s));
}
static id s_stringByDeletingLastPathComponent(id self, SEL _c) {
    const char *s = ns_cstr(self), *p = strrchr(s, '/');
    return p ? sub_bytes(s, p - s) : AR(ns_string_new(""));
}
static id s_pathExtension(id self, SEL _c) {
    const char *s = ns_cstr(self), *p = strrchr(s, '.'), *sl = strrchr(s, '/');
    return AR(ns_string_new(p && (!sl || p > sl) ? p + 1 : ""));
}
static id s_stringByDeletingPathExtension(id self, SEL _c) {
    const char *s = ns_cstr(self), *p = strrchr(s, '.'), *sl = strrchr(s, '/');
    return p && (!sl || p > sl) ? sub_bytes(s, p - s) : AR(ns_string_new(s));
}
static id s_self_copy(id self, SEL _c) { return AR(ns_string_new(ns_cstr(self))); }
// "/a//b/" -> ("/", "a", "b"). ponytail: Foundation agrega un "/" final si la ruta termina en barra; acá se omite.
static id s_pathComponents(id self, SEL _c) {
    const char *p = ns_cstr(self);
    id a = ns_array_new();
    if (*p == '/')
        ns_array_add(a, ns_str("/"));
    while (*p) {
        p += strspn(p, "/");
        size_t n = strcspn(p, "/");
        if (n)
            ns_array_add(a, sub_bytes(p, n));
        p += n;
    }
    return AR(a);
}
static id s_pathWithComponents(id cls, SEL _c, id comps) {
    char tmp[1024] = "";
    for (int i = 0, n = ns_array_count(comps); i < n; i++) {
        const char *c = ns_cstr(ns_array_get(comps, i));
        size_t l = strlen(tmp);
        if (!strcmp(c, "/")) {
            if (!l)
                strcpy(tmp, "/");
            continue;
        }
        snprintf(tmp + l, sizeof(tmp) - l, "%s%s", l && tmp[l - 1] != '/' ? "/" : "", c);
    }
    return AR(ns_string_new(tmp));
}
static id s_substringFromIndex(id self, SEL _c, int i) { return AR(ns_string_new(utf8_at(ns_cstr(self), i))); }
static id s_substringToIndex(id self, SEL _c, int i) {
    const char *s = ns_cstr(self);
    return sub_bytes(s, utf8_at(s, i) - s);
}
static id s_substringWithRange(id self, SEL _c, int loc, int len) {
    const char *s = ns_cstr(self), *a = utf8_at(s, loc), *b = utf8_at(a, len);
    return sub_bytes(a, b - a);
}
typedef struct {
    int loc, len;
} ns_range;
static void s_rangeOfString(ns_range *ret, id self, SEL _c, id o) {
    const char *s = ns_cstr(self), *f = strstr(s, ns_cstr(o));
    if (!f || !*ns_cstr(o)) {
        *ret = (ns_range){0x7FFFFFFF, 0};
        return;
    }
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%.*s", (int)(f - s), s);
    *ret = (ns_range){utf16_len(tmp), utf16_len(ns_cstr(o))};
}
static id s_lowercaseString(id self, SEL _c) {
    ns_string *r = (ns_string *)s_self_copy(self, _c);
    for (char *p = r->s; *p; p++)
        if (*p >= 'A' && *p <= 'Z')
            *p += 32;
    return r;
}
static id s_uppercaseString(id self, SEL _c) {
    ns_string *r = (ns_string *)s_self_copy(self, _c);
    for (char *p = r->s; *p; p++)
        if (*p >= 'a' && *p <= 'z')
            *p -= 32;
    return r;
}
static int s_hasPrefix(id self, SEL _c, id o) { return !strncmp(ns_cstr(self), ns_cstr(o), strlen(ns_cstr(o))); }
static int s_hasSuffix(id self, SEL _c, id o) {
    size_t a = strlen(ns_cstr(self)), b = strlen(ns_cstr(o));
    return b <= a && !strcmp(ns_cstr(self) + a - b, ns_cstr(o));
}
id ns_array_new(void);
void ns_array_add(id a, id o);
static id s_componentsSeparatedByString(id self, SEL _c, id sep) {
    id arr = ns_array_new();
    const char *s = ns_cstr(self), *d = ns_cstr(sep);
    size_t dl = strlen(d);
    const char *f;
    while (dl && (f = strstr(s, d))) {
        ns_array_add(arr, sub_bytes(s, f - s));
        s = f + dl;
    }
    ns_array_add(arr, AR(ns_string_new(s)));
    return AR(arr);
}
static id s_stringByReplacing(id self, SEL _c, id a, id b) {
    const char *s = ns_cstr(self), *x = ns_cstr(a), *y = ns_cstr(b);
    size_t xl = strlen(x), yl = strlen(y);
    if (!xl)
        return s_self_copy(self, _c);
    size_t cap = strlen(s) * (yl > xl ? yl / xl + 1 : 1) + 1, n = 0;
    char *o = malloc(cap + 16);
    const char *f;
    while ((f = strstr(s, x))) {
        if (n + (f - s) + yl + 1 > cap)
            o = realloc(o, cap = (n + (f - s) + yl + 1) * 2);
        memcpy(o + n, s, f - s);
        n += f - s;
        memcpy(o + n, y, yl);
        n += yl;
        s = f + xl;
    }
    size_t rest = strlen(s);
    o = realloc(o, n + rest + 1);
    memcpy(o + n, s, rest + 1);
    ns_string *r = objc_alloc(c_NSString);
    r->s = o;
    r->len = n + rest;
    r->flags = 0x7C8;
    return AR(r);
}
static id s_stringByTrimming(id self, SEL _c, id set) {
    const char *s = ns_cstr(self), *e = s + strlen(s);
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        s++;
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
        e--;
    return sub_bytes(s, e - s);
}
static int s_writeToFile(id self, SEL _c, id path, int atomic, int enc, id *err) {
    FILE *f = fopen(ns_cstr(path), "wb");
    if (!f)
        return 0;
    fputs(ns_cstr(self), f);
    fclose(f);
    return 1;
}
static id s_dataUsingEncoding(id self, SEL _c, int enc) {
    ns_data *d = objc_alloc(c_NSData);
    d->len = strlen(ns_cstr(self));
    d->bytes = malloc(d->len + 1);
    memcpy(d->bytes, ns_cstr(self), d->len + 1);
    d->owned = 1;
    return AR(d);
}
static id s_description(id self, SEL _c) { return self; }
static id s_mutableCopy(id self, SEL _c) {
    ns_string *r = objc_alloc(c_NSMutableString);
    str_set(r, ns_cstr(self), strlen(ns_cstr(self)));
    return r;
}
static id s_copy(id self, SEL _c) { return ns_string_new(ns_cstr(self)); }

// NSMutableString
static void ms_append(ns_string *self, const char *b) {
    size_t lb = strlen(b), la = self->s ? strlen(self->s) : 0;
    char *n = malloc(la + lb + 1);
    if (la)
        memcpy(n, self->s, la);
    memcpy(n + la, b, lb + 1);
    if (self->s && !is_guest_addr((uintptr_t)self->s))
        free(self->s);
    self->s = n;
    self->len = la + lb;
}
static void ms_appendString(ns_string *self, SEL _c, id o) { ms_append(self, ns_cstr(o)); }
static void ms_appendFormat(ns_string *self, SEL _c, id fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    words w = {&ap, NULL};
    id t = str_vformat(ns_cstr(fmt), &w);
    va_end(ap);
    ms_append(self, ns_cstr(t));
    objc_release(t);
}
static void ms_setString(ns_string *self, SEL _c, id o) {
    if (self->s && !is_guest_addr((uintptr_t)self->s))
        free(self->s);
    self->s = NULL;
    ms_append(self, ns_cstr(o));
}
static id ms_stringWithCapacity(id cls, SEL _c, int n) {
    ns_string *s = objc_alloc(c_NSMutableString);
    str_set(s, "", 0);
    return AR(s);
}
static id ms_initWithCapacity(ns_string *self, SEL _c, int n) {
    str_set(self, "", 0);
    return self;
}
static id ms_init(ns_string *self, SEL _c) {
    if (!self->s)
        str_set(self, "", 0);
    return self;
}
static void ms_deleteCharactersInRange(ns_string *self, SEL _c, int loc, int len) {
    char *a = (char *)utf8_at(self->s, loc), *b = (char *)utf8_at(a, len);
    memmove(a, b, strlen(b) + 1);
    self->len = strlen(self->s);
}
static void ms_insertString(ns_string *self, SEL _c, id o, int at) {
    const char *ins = ns_cstr(o);
    size_t off = utf8_at(self->s, at) - self->s, li = strlen(ins), l = strlen(self->s);
    char *n = malloc(l + li + 1);
    memcpy(n, self->s, off);
    memcpy(n + off, ins, li);
    memcpy(n + off + li, self->s + off, l - off + 1);
    free(self->s);
    self->s = n;
    self->len = l + li;
}

static const nmethod nsstring_c[] = {
    NM("string", s_string), NM("stringWithUTF8String:", s_stringWithUTF8String),
    NM("stringWithCString:encoding:", s_stringWithCString_enc), NM("stringWithCString:", s_stringWithUTF8String),
    NM("stringWithString:", s_stringWithString), NM("stringWithFormat:", s_stringWithFormat),
    NM("localizedStringWithFormat:", s_stringWithFormat), NM("stringWithCharacters:length:", s_stringWithCharacters),
    NM("stringWithContentsOfFile:encoding:error:", s_stringWithContentsOfFile),
    NM("stringWithContentsOfFile:", s_stringWithContentsOfFile), NM("pathWithComponents:", s_pathWithComponents), {0}};
static const nmethod nsstring_i[] = {
    NM("initWithUTF8String:", s_initWithUTF8String), NM("initWithCString:encoding:", s_initWithCString_enc),
    NM("initWithCString:", s_initWithUTF8String), NM("initWithString:", s_initWithString),
    NM("initWithFormat:", s_initWithFormat), NM("initWithFormat:arguments:", s_initWithFormat_args),
    NM("initWithBytes:length:encoding:", s_initWithBytes), NM("initWithData:encoding:", s_initWithData),
    NM("initWithCharacters:length:", s_initWithCharacters), NM("dealloc", s_dealloc),
    NM("UTF8String", s_UTF8String), NM("cString", s_UTF8String), NM("fileSystemRepresentation", s_UTF8String),
    NM("lossyCString", s_UTF8String), NM("cStringUsingEncoding:", s_cStringUsingEncoding),
    NM("getCString:maxLength:encoding:", s_getCString), NM("length", s_length),
    NM("lengthOfBytesUsingEncoding:", s_lengthOfBytes), NM("cStringLength", s_lengthOfBytes),
    NM("characterAtIndex:", s_characterAtIndex), NM("getCharacters:range:", s_getCharacters),
    NM("isEqualToString:", s_isEqualToString), NM("isEqual:", s_isEqual), NM("hash", s_hash),
    NM("compare:", s_compare), NM("caseInsensitiveCompare:", s_caseInsensitiveCompare),
    NM("intValue", s_intValue), NM("integerValue", s_intValue), NM("longLongValue", s_longLongValue),
    NM("floatValue", s_floatValue), NM("doubleValue", s_doubleValue), NM("boolValue", s_boolValue),
    NM("stringByAppendingString:", s_stringByAppendingString), NM("stringByAppendingFormat:", s_stringByAppendingFormat),
    NM("stringByAppendingPathComponent:", s_stringByAppendingPathComponent),
    NM("stringByAppendingPathExtension:", s_stringByAppendingPathExtension),
    NM("lastPathComponent", s_lastPathComponent), NM("pathComponents", s_pathComponents), NM("stringByDeletingLastPathComponent", s_stringByDeletingLastPathComponent),
    NM("pathExtension", s_pathExtension), NM("stringByDeletingPathExtension", s_stringByDeletingPathExtension),
    NM("stringByStandardizingPath", s_self_copy), NM("stringByExpandingTildeInPath", s_self_copy),
    NM("stringByResolvingSymlinksInPath", s_self_copy),
    NM("substringFromIndex:", s_substringFromIndex), NM("substringToIndex:", s_substringToIndex),
    NM("substringWithRange:", s_substringWithRange), NM("rangeOfString:", s_rangeOfString),
    NM("lowercaseString", s_lowercaseString), NM("uppercaseString", s_uppercaseString),
    NM("hasPrefix:", s_hasPrefix), NM("hasSuffix:", s_hasSuffix),
    NM("componentsSeparatedByString:", s_componentsSeparatedByString),
    NM("stringByReplacingOccurrencesOfString:withString:", s_stringByReplacing),
    NM("stringByTrimmingCharactersInSet:", s_stringByTrimming),
    NM("writeToFile:atomically:encoding:error:", s_writeToFile), NM("writeToFile:atomically:", s_writeToFile),
    NM("dataUsingEncoding:", s_dataUsingEncoding), NM("dataUsingEncoding:allowLossyConversion:", s_dataUsingEncoding),
    NM("description", s_description), NM("copy", s_copy), NM("mutableCopy", s_mutableCopy),
    NM("stringByAddingPercentEscapesUsingEncoding:", s_self_copy), NM("stringByReplacingPercentEscapesUsingEncoding:", s_self_copy),
    {0}};
static const nmethod nsmstring_c[] = {NM("stringWithCapacity:", ms_stringWithCapacity), NM("string", ms_stringWithCapacity), {0}};
static const nmethod nsmstring_i[] = {
    NM("init", ms_init), NM("initWithCapacity:", ms_initWithCapacity), NM("appendString:", ms_appendString),
    NM("appendFormat:", ms_appendFormat), NM("setString:", ms_setString),
    NM("deleteCharactersInRange:", ms_deleteCharactersInRange), NM("insertString:atIndex:", ms_insertString),
    NM("copy", s_copy), {0}};

// ================================================================ NSObject

static id o_alloc(objc_class *cls, SEL _c) { return objc_alloc(cls); }
static id o_new(objc_class *cls, SEL _c) { return MSG0(objc_alloc(cls), "init"); }
static id o_self(id self, SEL _c) { return self; }
static void o_dealloc(id self, SEL _c) { objc_free(self); }
static id o_retain(id self, SEL _c) { return objc_retain(self); }
static void o_release(id self, SEL _c) { objc_release(self); }
static id o_autorelease(id self, SEL _c) { return objc_autorelease(self); }
int objc_retain_count(id o);
void objc_free(id o);
static int o_retainCount(id self, SEL _c) { return objc_retain_count(self); }
static objc_class *o_class(id self, SEL _c) { return object_getClass(self); }
static objc_class *o_superclass(id self, SEL _c) { return object_getClass(self)->superclass; }
static objc_class *o_class_cls(objc_class *self, SEL _c) { return self; }
static objc_class *o_superclass_cls(objc_class *self, SEL _c) { return self->superclass; }
static int o_isKindOfClass(id self, SEL _c, objc_class *k) { return objc_is_kind(self, k); }
static int o_isMemberOfClass(id self, SEL _c, objc_class *k) { return object_getClass(self) == k; }
static int o_respondsToSelector(id self, SEL _c, SEL s) { return s && objc_responds(self, sel_registerName(s)); }
static int o_instancesRespond(objc_class *self, SEL _c, SEL s) {
    id fake = &self; // objeto "falso" cuyo isa es la clase
    return objc_responds(fake, sel_registerName(s));
}
static int o_yes(id self, SEL _c) { return 1; }
static uint32_t o_perform0(id self, SEL _c, SEL s) { return objc_msg(self, sel_registerName(s), 0, 0, 0, 0); }
static uint32_t o_perform1(id self, SEL _c, SEL s, id a) { return objc_msg(self, sel_registerName(s), (uint32_t)a, 0, 0, 0); }
static uint32_t o_perform2(id self, SEL _c, SEL s, id a, id b) {
    return objc_msg(self, sel_registerName(s), (uint32_t)a, (uint32_t)b, 0, 0);
}
static void o_performMain(id self, SEL _c, SEL s, id a, int wait) { objc_msg(self, sel_registerName(s), (uint32_t)a, 0, 0, 0); }
void ns_perform_later(id target, SEL s, id arg, double delay);
static void o_performAfterDelay(id self, SEL _c, SEL s, id a, double delay) { ns_perform_later(self, s, a, delay); }
void ns_cancel_performs(id target);
static void o_cancelPrevious(objc_class *cls, SEL _c, id target) { ns_cancel_performs(target); }
static int o_isEqual(id self, SEL _c, id o) { return self == o; }
static uint32_t o_hash(id self, SEL _c) { return (uint32_t)self >> 3; }
static id o_description(id self, SEL _c) {
    char b[96];
    snprintf(b, sizeof(b), "<%s: %p>", class_getName(object_getClass(self)), self);
    return ns_str(b);
}
static id o_description_cls(objc_class *self, SEL _c) { return ns_str(class_getName(self)); }
static void *o_methodForSelector(id self, SEL _c, SEL s) {
    extern void *objc_lookup_imp(id self, SEL sel);
    extern void ml_align_call(void);
    void *imp = objc_lookup_imp(self, sel_registerName(s));
    // IMP nativa: el juego la llamará directo con la pila alineada a 4 -> por ml_align_call
    return is_guest_addr((uintptr_t)imp) ? imp : (void *)macho_thunk((uint32_t)imp, ml_align_call);
}
static void o_noop(id self, SEL _c) {}
static id o_zone(id self, SEL _c) { return NULL; }

static const nmethod nsobject_c[] = {
    NM("alloc", o_alloc), NM("allocWithZone:", o_alloc), NM("new", o_new), NM("class", o_class_cls),
    NM("superclass", o_superclass_cls), NM("initialize", o_noop), NM("load", o_noop),
    NM("instancesRespondToSelector:", o_instancesRespond), NM("description", o_description_cls),
    NM("cancelPreviousPerformRequestsWithTarget:", o_cancelPrevious),
    NM("cancelPreviousPerformRequestsWithTarget:selector:object:", o_cancelPrevious), {0}};
static const nmethod nsobject_i[] = {
    NM("init", o_self), NM("self", o_self), NM("dealloc", o_dealloc), NM("finalize", o_noop),
    NM("retain", o_retain), NM("release", o_release), NM("autorelease", o_autorelease),
    NM("retainCount", o_retainCount), NM("class", o_class), NM("superclass", o_superclass),
    NM("isKindOfClass:", o_isKindOfClass), NM("isMemberOfClass:", o_isMemberOfClass),
    NM("respondsToSelector:", o_respondsToSelector), NM("conformsToProtocol:", o_yes),
    NM("performSelector:", o_perform0), NM("performSelector:withObject:", o_perform1),
    NM("performSelector:withObject:withObject:", o_perform2),
    NM("performSelectorOnMainThread:withObject:waitUntilDone:", o_performMain),
    NM("performSelector:withObject:afterDelay:", o_performAfterDelay), NM("isEqual:", o_isEqual),
    NM("hash", o_hash), NM("description", o_description), NM("copy", o_retain), NM("mutableCopy", o_retain),
    NM("methodForSelector:", o_methodForSelector), NM("zone", o_zone), {0}};

// ================================================================ NSNumber

static id num_new(int type, int64_t i, double d) {
    ns_number *n = objc_alloc(c_NSNumber);
    n->type = type;
    if (type == 1)
        n->d = d;
    else
        n->i = i;
    return n;
}
id ns_num_int(int64_t v) { return AR(num_new(0, v, 0)); }
static double num_d(ns_number *n) { return !n ? 0 : n->type == 1 ? n->d : (double)n->i; }
static int64_t num_i(ns_number *n) { return !n ? 0 : n->type == 1 ? (int64_t)n->d : n->i; }

static id n_numberWithInt(id cls, SEL _c, int v) { return AR(num_new(0, v, 0)); }
static id n_numberWithUInt(id cls, SEL _c, unsigned v) { return AR(num_new(0, v, 0)); }
static id n_numberWithBool(id cls, SEL _c, int v) { return AR(num_new(2, v != 0, 0)); }
static id n_numberWithLongLong(id cls, SEL _c, long long v) { return AR(num_new(0, v, 0)); }
static id n_numberWithFloat(id cls, SEL _c, float v) { return AR(num_new(1, 0, v)); }
static id n_numberWithDouble(id cls, SEL _c, double v) { return AR(num_new(1, 0, v)); }
static id n_initWithInt(ns_number *self, SEL _c, int v) { self->type = 0, self->i = v; return self; }
static id n_initWithBool(ns_number *self, SEL _c, int v) { self->type = 2, self->i = v != 0; return self; }
static id n_initWithFloat(ns_number *self, SEL _c, float v) { self->type = 1, self->d = v; return self; }
static id n_initWithDouble(ns_number *self, SEL _c, double v) { self->type = 1, self->d = v; return self; }
static int n_intValue(ns_number *self, SEL _c) { return (int)num_i(self); }
static long long n_longLongValue(ns_number *self, SEL _c) { return num_i(self); }
static int n_boolValue(ns_number *self, SEL _c) { return num_d(self) != 0; }
static float n_floatValue(ns_number *self, SEL _c) { return (float)num_d(self); }
static double n_doubleValue(ns_number *self, SEL _c) { return num_d(self); }
static int n_isEqual(ns_number *self, SEL _c, ns_number *o) {
    return o && objc_is_kind(o, c_NSNumber) && num_d(self) == num_d(o);
}
static int n_compare(ns_number *self, SEL _c, ns_number *o) {
    double a = num_d(self), b = num_d(o);
    return a < b ? -1 : a > b;
}
static uint32_t n_hash(ns_number *self, SEL _c) { return (uint32_t)num_i(self); }
static id n_stringValue(ns_number *self, SEL _c) {
    char b[64];
    if (self->type == 1)
        snprintf(b, sizeof(b), "%g", self->d);
    else
        snprintf(b, sizeof(b), "%lld", (long long)self->i);
    return ns_str(b);
}
static id n_copy(id self, SEL _c) { return objc_retain(self); }
static const nmethod nsnumber_c[] = {
    NM("numberWithInt:", n_numberWithInt), NM("numberWithInteger:", n_numberWithInt),
    NM("numberWithUnsignedInt:", n_numberWithUInt), NM("numberWithUnsignedInteger:", n_numberWithUInt),
    NM("numberWithBool:", n_numberWithBool), NM("numberWithChar:", n_numberWithInt), NM("numberWithShort:", n_numberWithInt),
    NM("numberWithLong:", n_numberWithInt), NM("numberWithLongLong:", n_numberWithLongLong),
    NM("numberWithUnsignedLongLong:", n_numberWithLongLong), NM("numberWithFloat:", n_numberWithFloat),
    NM("numberWithDouble:", n_numberWithDouble), {0}};
static const nmethod nsnumber_i[] = {
    NM("initWithInt:", n_initWithInt), NM("initWithInteger:", n_initWithInt), NM("initWithBool:", n_initWithBool),
    NM("initWithFloat:", n_initWithFloat), NM("initWithDouble:", n_initWithDouble), NM("intValue", n_intValue),
    NM("integerValue", n_intValue), NM("unsignedIntValue", n_intValue), NM("unsignedIntegerValue", n_intValue),
    NM("longValue", n_intValue), NM("shortValue", n_intValue), NM("charValue", n_intValue),
    NM("unsignedCharValue", n_intValue), NM("longLongValue", n_longLongValue), NM("unsignedLongLongValue", n_longLongValue),
    NM("boolValue", n_boolValue), NM("floatValue", n_floatValue), NM("doubleValue", n_doubleValue),
    NM("isEqual:", n_isEqual), NM("isEqualToNumber:", n_isEqual), NM("compare:", n_compare), NM("hash", n_hash),
    NM("stringValue", n_stringValue), NM("description", n_stringValue), NM("copy", n_copy), {0}};

// ================================================================ NSArray / NSSet

id ns_array_new(void) { return objc_alloc(c_NSMutableArray); }
void ns_array_add(id a, id o) {
    ns_array *s = a;
    if (!o)
        return;
    if (s->count == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->items = realloc(s->items, s->cap * sizeof(id));
    }
    s->items[s->count++] = objc_retain(o);
}
int ns_array_count(id a) { return a ? ((ns_array *)a)->count : 0; }
id ns_array_get(id a, int i) { return a && i >= 0 && i < ((ns_array *)a)->count ? ((ns_array *)a)->items[i] : NULL; }

static int obj_equal(id a, id b) {
    if (a == b)
        return 1;
    if (!a || !b)
        return 0;
    return (uintptr_t)MSG1(a, "isEqual:", b) & 0xFF;
}

static id a_array(objc_class *cls, SEL _c) { return AR(objc_alloc(cls)); }
static id a_arrayWithObject(objc_class *cls, SEL _c, id o) {
    id a = objc_alloc(cls);
    ns_array_add(a, o);
    return AR(a);
}
static id a_fill_va(id a, id first, va_list ap) {
    for (id o = first; o; o = va_arg(ap, id))
        ns_array_add(a, o);
    return a;
}
static id a_arrayWithObjects(objc_class *cls, SEL _c, id first, ...) {
    va_list ap;
    va_start(ap, first);
    id a = a_fill_va(objc_alloc(cls), first, ap);
    va_end(ap);
    return AR(a);
}
static id a_initWithObjects(id self, SEL _c, id first, ...) {
    va_list ap;
    va_start(ap, first);
    a_fill_va(self, first, ap);
    va_end(ap);
    return self;
}
static id a_arrayWithObjects_count(objc_class *cls, SEL _c, id *objs, int n) {
    id a = objc_alloc(cls);
    for (int i = 0; i < n; i++)
        ns_array_add(a, objs[i]);
    return AR(a);
}
static id a_initWithArray(id self, SEL _c, ns_array *o) {
    for (int i = 0; o && i < o->count; i++)
        ns_array_add(self, o->items[i]);
    return self;
}
static id a_arrayWithArray(objc_class *cls, SEL _c, ns_array *o) { return AR(a_initWithArray(objc_alloc(cls), _c, o)); }
static id a_initWithCapacity(id self, SEL _c, int n) { return self; }
static id a_arrayWithCapacity(objc_class *cls, SEL _c, int n) { return AR(objc_alloc(cls)); }
static void a_dealloc(ns_array *self, SEL _c) {
    for (int i = 0; i < self->count; i++)
        objc_release(self->items[i]);
    free(self->items);
    objc_free(self);
}
static int a_count(ns_array *self, SEL _c) { return self->count; }
static id a_objectAtIndex(ns_array *self, SEL _c, int i) {
    if (i < 0 || i >= self->count) {
        l_error("NSArray objectAtIndex:%d fuera de rango (%d)", i, self->count);
        return NULL;
    }
    return self->items[i];
}
static id a_lastObject(ns_array *self, SEL _c) { return self->count ? self->items[self->count - 1] : NULL; }
static id a_firstObject(ns_array *self, SEL _c) { return self->count ? self->items[0] : NULL; }
static void a_addObject(ns_array *self, SEL _c, id o) { ns_array_add(self, o); }
static void a_addObjectsFromArray(ns_array *self, SEL _c, ns_array *o) { a_initWithArray(self, _c, o); }
static void a_insertObject(ns_array *self, SEL _c, id o, int at) {
    ns_array_add(self, o);
    if (at < 0 || at >= self->count)
        return;
    memmove(self->items + at + 1, self->items + at, (self->count - 1 - at) * sizeof(id));
    self->items[at] = o;
}
static void a_removeObjectAtIndex(ns_array *self, SEL _c, int i) {
    if (i < 0 || i >= self->count)
        return;
    id o = self->items[i];
    memmove(self->items + i, self->items + i + 1, (self->count - i - 1) * sizeof(id));
    self->count--;
    objc_release(o);
}
static void a_removeLastObject(ns_array *self, SEL _c) { a_removeObjectAtIndex(self, _c, self->count - 1); }
static int a_indexOfObject(ns_array *self, SEL _c, id o) {
    for (int i = 0; i < self->count; i++)
        if (obj_equal(self->items[i], o))
            return i;
    return 0x7FFFFFFF;
}
static void a_removeObject(ns_array *self, SEL _c, id o) {
    for (int i = self->count - 1; i >= 0; i--)
        if (obj_equal(self->items[i], o))
            a_removeObjectAtIndex(self, _c, i);
}
static int a_containsObject(ns_array *self, SEL _c, id o) { return a_indexOfObject(self, _c, o) != 0x7FFFFFFF; }
static void a_removeAllObjects(ns_array *self, SEL _c) {
    while (self->count)
        a_removeObjectAtIndex(self, _c, self->count - 1);
}
static void a_replaceObject(ns_array *self, SEL _c, int i, id o) {
    if (i < 0 || i >= self->count)
        return;
    objc_retain(o);
    objc_release(self->items[i]);
    self->items[i] = o;
}
static void a_exchange(ns_array *self, SEL _c, int i, int j) {
    if (i >= 0 && j >= 0 && i < self->count && j < self->count) {
        id t = self->items[i];
        self->items[i] = self->items[j];
        self->items[j] = t;
    }
}
typedef struct {
    unsigned long state;
    id *itemsPtr;
    unsigned long *mutationsPtr;
    unsigned long extra[5];
} fast_enum;
static int a_countByEnumerating(ns_array *self, SEL _c, fast_enum *st, id *buf, int len) {
    if (st->state)
        return 0;
    st->state = 1;
    st->itemsPtr = self->items;
    st->mutationsPtr = &st->extra[0];
    return self->count;
}
static void a_makeObjectsPerform(ns_array *self, SEL _c, SEL s) {
    for (int i = 0; i < self->count; i++)
        objc_msg(self->items[i], sel_registerName(s), 0, 0, 0, 0);
}
static id a_componentsJoined(ns_array *self, SEL _c, id sep) {
    ns_string *r = objc_alloc(c_NSMutableString);
    str_set(r, "", 0);
    for (int i = 0; i < self->count; i++) {
        if (i)
            ms_append(r, ns_cstr(sep));
        ms_append(r, objc_is_kind(self->items[i], c_NSString) ? ns_cstr(self->items[i])
                                                                : ns_cstr((id)MSG0(self->items[i], "description")));
    }
    return AR(r);
}
static id a_copy(ns_array *self, SEL _c) { return a_initWithArray(objc_alloc(c_NSArray), _c, self); }
static id a_mutableCopy(ns_array *self, SEL _c) { return a_initWithArray(objc_alloc(c_NSMutableArray), _c, self); }
static id a_anyObject(ns_array *self, SEL _c) { return self->count ? self->items[0] : NULL; }
static id a_allObjects(ns_array *self, SEL _c) { return AR(a_copy(self, _c)); }
static id a_setWithObject(objc_class *cls, SEL _c, id o) { return a_arrayWithObject(cls, _c, o); }
static id a_objectEnumerator(ns_array *self, SEL _c);
static int a_isEqual(ns_array *self, SEL _c, ns_array *o) {
    if (!o || o->count != self->count)
        return self == o;
    for (int i = 0; i < self->count; i++)
        if (!obj_equal(self->items[i], o->items[i]))
            return 0;
    return 1;
}
static SEL sort_sel;
static int sort_cmp(const void *a, const void *b) {
    return (int)(int8_t)objc_msg(*(id *)a, sort_sel, (uint32_t) * (id *)b, 0, 0, 0);
}
static void a_sortUsingSelector(ns_array *self, SEL _c, SEL s) {
    sort_sel = sel_registerName(s);
    qsort(self->items, self->count, sizeof(id), sort_cmp);
}
static id a_sortedArrayUsingSelector(ns_array *self, SEL _c, SEL s) {
    ns_array *r = a_mutableCopy(self, _c);
    a_sortUsingSelector(r, _c, s);
    return AR(r);
}

// NSEnumerator sobre un array (copia de los elementos)
static objc_class *c_NSEnumerator;
typedef struct {
    objc_class *isa;
    ns_array *arr;
    int pos;
} ns_enum;
static id a_objectEnumerator(ns_array *self, SEL _c) {
    ns_enum *e = objc_alloc(c_NSEnumerator);
    e->arr = a_copy(self, _c);
    return AR(e);
}
static id e_nextObject(ns_enum *self, SEL _c) { return self->pos < self->arr->count ? self->arr->items[self->pos++] : NULL; }
static id e_allObjects(ns_enum *self, SEL _c) { return self->arr; }
static void e_dealloc(ns_enum *self, SEL _c) {
    objc_release(self->arr);
    objc_free(self);
}

static const nmethod nsarray_c[] = {
    NM("array", a_array), NM("arrayWithObject:", a_arrayWithObject), NM("arrayWithObjects:", a_arrayWithObjects),
    NM("arrayWithObjects:count:", a_arrayWithObjects_count), NM("arrayWithArray:", a_arrayWithArray),
    NM("arrayWithCapacity:", a_arrayWithCapacity), NM("set", a_array), NM("setWithObject:", a_setWithObject),
    NM("setWithObjects:", a_arrayWithObjects), NM("setWithArray:", a_arrayWithArray), {0}};
static const nmethod nsarray_i[] = {
    NM("initWithObjects:", a_initWithObjects), NM("initWithArray:", a_initWithArray),
    NM("initWithCapacity:", a_initWithCapacity), NM("dealloc", a_dealloc), NM("count", a_count),
    NM("objectAtIndex:", a_objectAtIndex), NM("lastObject", a_lastObject), NM("firstObject", a_firstObject),
    NM("indexOfObject:", a_indexOfObject), NM("containsObject:", a_containsObject), NM("member:", a_containsObject),
    NM("countByEnumeratingWithState:objects:count:", a_countByEnumerating),
    NM("makeObjectsPerformSelector:", a_makeObjectsPerform), NM("componentsJoinedByString:", a_componentsJoined),
    NM("copy", a_copy), NM("mutableCopy", a_mutableCopy), NM("anyObject", a_anyObject), NM("allObjects", a_allObjects),
    NM("objectEnumerator", a_objectEnumerator), NM("isEqual:", a_isEqual), NM("isEqualToArray:", a_isEqual),
    NM("sortedArrayUsingSelector:", a_sortedArrayUsingSelector),
    // mutables (NSMutableArray/NSMutableSet comparten layout; se dejan en la base por simplicidad)
    NM("addObject:", a_addObject), NM("addObjectsFromArray:", a_addObjectsFromArray),
    NM("insertObject:atIndex:", a_insertObject), NM("removeObjectAtIndex:", a_removeObjectAtIndex),
    NM("removeLastObject", a_removeLastObject), NM("removeObject:", a_removeObject),
    NM("removeAllObjects", a_removeAllObjects), NM("replaceObjectAtIndex:withObject:", a_replaceObject),
    NM("exchangeObjectAtIndex:withObjectAtIndex:", a_exchange), NM("sortUsingSelector:", a_sortUsingSelector),
    {0}};
static const nmethod nsenum_i[] = {NM("nextObject", e_nextObject), NM("allObjects", e_allObjects), NM("dealloc", e_dealloc), {0}};

// ================================================================ NSDictionary

id ns_dict_new(void) { return objc_alloc(c_NSMutableDictionary); }
static int dict_find(ns_dict *d, id k) {
    const char *ks = objc_is_kind(k, c_NSString) ? ns_cstr(k) : NULL;
    for (int i = 0; i < d->count; i++) {
        if (d->keys[i] == k)
            return i;
        if (ks && objc_is_kind(d->keys[i], c_NSString)) {
            if (!strcmp(ks, ns_cstr(d->keys[i])))
                return i;
        } else if (!ks && obj_equal(d->keys[i], k)) {
            return i;
        }
    }
    return -1;
}
void ns_dict_set(id dict, id k, id v) {
    ns_dict *d = dict;
    if (!k)
        return;
    int i = dict_find(d, k);
    if (i >= 0) {
        objc_retain(v);
        objc_release(d->vals[i]);
        d->vals[i] = v;
        return;
    }
    if (d->count == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 8;
        d->keys = realloc(d->keys, d->cap * sizeof(id));
        d->vals = realloc(d->vals, d->cap * sizeof(id));
    }
    d->keys[d->count] = objc_is_kind(k, c_NSString) ? ns_string_new(ns_cstr(k)) : objc_retain(k);
    d->vals[d->count++] = objc_retain(v);
}
id ns_dict_get(id dict, id k) {
    if (!dict || !k)
        return NULL;
    int i = dict_find(dict, k);
    return i >= 0 ? ((ns_dict *)dict)->vals[i] : NULL;
}
id ns_dict_get_c(id dict, const char *k) {
    id key = ns_string_new(k);
    id r = ns_dict_get(dict, key);
    objc_release(key);
    return r;
}

static id d_dictionary(objc_class *cls, SEL _c) { return AR(objc_alloc(cls)); }
static id d_withObjectForKey(objc_class *cls, SEL _c, id v, id k) {
    id d = objc_alloc(cls);
    ns_dict_set(d, k, v);
    return AR(d);
}
static id d_fill_va(id d, id first, va_list ap) {
    for (id v = first; v; v = va_arg(ap, id)) {
        id k = va_arg(ap, id);
        ns_dict_set(d, k, v);
    }
    return d;
}
static id d_withObjectsAndKeys(objc_class *cls, SEL _c, id first, ...) {
    va_list ap;
    va_start(ap, first);
    id d = d_fill_va(objc_alloc(cls), first, ap);
    va_end(ap);
    return AR(d);
}
static id d_initWithObjectsAndKeys(id self, SEL _c, id first, ...) {
    va_list ap;
    va_start(ap, first);
    d_fill_va(self, first, ap);
    va_end(ap);
    return self;
}
static id d_initWithDictionary(id self, SEL _c, ns_dict *o) {
    for (int i = 0; o && i < o->count; i++)
        ns_dict_set(self, o->keys[i], o->vals[i]);
    return self;
}
static id d_withDictionary(objc_class *cls, SEL _c, ns_dict *o) { return AR(d_initWithDictionary(objc_alloc(cls), _c, o)); }
static id d_withObjectsForKeys(objc_class *cls, SEL _c, ns_array *vals, ns_array *keys) {
    id d = objc_alloc(cls);
    for (int i = 0; vals && keys && i < vals->count && i < keys->count; i++)
        ns_dict_set(d, keys->items[i], vals->items[i]);
    return AR(d);
}
id plist_load(const char *path);
static id d_withContentsOfFile(objc_class *cls, SEL _c, id path) {
    id r = plist_load(ns_cstr(path));
    return r ? AR(r) : NULL;
}
static id d_initWithContentsOfFile(id self, SEL _c, id path) {
    id r = plist_load(ns_cstr(path));
    objc_free(self);
    return r;
}
static void d_dealloc(ns_dict *self, SEL _c) {
    for (int i = 0; i < self->count; i++) {
        objc_release(self->keys[i]);
        objc_release(self->vals[i]);
    }
    free(self->keys);
    free(self->vals);
    objc_free(self);
}
static int d_count(ns_dict *self, SEL _c) { return self->count; }
static id d_objectForKey(ns_dict *self, SEL _c, id k) { return ns_dict_get(self, k); }
static void d_setObjectForKey(ns_dict *self, SEL _c, id v, id k) { ns_dict_set(self, k, v); }
static void d_removeObjectForKey(ns_dict *self, SEL _c, id k) {
    int i = dict_find(self, k);
    if (i < 0)
        return;
    objc_release(self->keys[i]);
    objc_release(self->vals[i]);
    self->count--;
    memmove(self->keys + i, self->keys + i + 1, (self->count - i) * sizeof(id));
    memmove(self->vals + i, self->vals + i + 1, (self->count - i) * sizeof(id));
}
static void d_removeAll(ns_dict *self, SEL _c) {
    while (self->count)
        d_removeObjectForKey(self, _c, self->keys[self->count - 1]);
}
static id d_allKeys(ns_dict *self, SEL _c) {
    id a = ns_array_new();
    for (int i = 0; i < self->count; i++)
        ns_array_add(a, self->keys[i]);
    return AR(a);
}
static id d_allValues(ns_dict *self, SEL _c) {
    id a = ns_array_new();
    for (int i = 0; i < self->count; i++)
        ns_array_add(a, self->vals[i]);
    return AR(a);
}
static id d_keyEnumerator(ns_dict *self, SEL _c) { return a_objectEnumerator(d_allKeys(self, _c), _c); }
static int d_countByEnumerating(ns_dict *self, SEL _c, fast_enum *st, id *buf, int len) {
    if (st->state)
        return 0;
    st->state = 1;
    st->itemsPtr = self->keys;
    st->mutationsPtr = &st->extra[0];
    return self->count;
}
static id d_copy(ns_dict *self, SEL _c) { return d_initWithDictionary(objc_alloc(c_NSDictionary), _c, self); }
static id d_mutableCopy(ns_dict *self, SEL _c) { return d_initWithDictionary(objc_alloc(c_NSMutableDictionary), _c, self); }
static void d_addEntries(ns_dict *self, SEL _c, ns_dict *o) { d_initWithDictionary(self, _c, o); }
static int plist_write(const char *path, id obj);
static int d_writeToFile(ns_dict *self, SEL _c, id path, int atomic) { return plist_write(ns_cstr(path), self); }

static const nmethod nsdict_c[] = {
    NM("dictionary", d_dictionary), NM("dictionaryWithObject:forKey:", d_withObjectForKey),
    NM("dictionaryWithObjectsAndKeys:", d_withObjectsAndKeys), NM("dictionaryWithDictionary:", d_withDictionary),
    NM("dictionaryWithObjects:forKeys:", d_withObjectsForKeys), NM("dictionaryWithContentsOfFile:", d_withContentsOfFile),
    NM("dictionaryWithCapacity:", a_arrayWithCapacity), {0}};
static const nmethod nsdict_i[] = {
    NM("initWithObjectsAndKeys:", d_initWithObjectsAndKeys), NM("initWithDictionary:", d_initWithDictionary),
    NM("initWithContentsOfFile:", d_initWithContentsOfFile), NM("initWithCapacity:", a_initWithCapacity),
    NM("dealloc", d_dealloc), NM("count", d_count), NM("objectForKey:", d_objectForKey),
    NM("valueForKey:", d_objectForKey), NM("setObject:forKey:", d_setObjectForKey),
    NM("setValue:forKey:", d_setObjectForKey), NM("removeObjectForKey:", d_removeObjectForKey),
    NM("removeAllObjects", d_removeAll), NM("allKeys", d_allKeys), NM("allValues", d_allValues),
    NM("keyEnumerator", d_keyEnumerator), NM("countByEnumeratingWithState:objects:count:", d_countByEnumerating),
    NM("copy", d_copy), NM("mutableCopy", d_mutableCopy), NM("addEntriesFromDictionary:", d_addEntries),
    NM("writeToFile:atomically:", d_writeToFile), {0}};

// ================================================================ NSData

static id data_new(objc_class *cls, const void *b, int n, int copy) {
    ns_data *d = objc_alloc(cls);
    d->len = d->cap = n;
    if (copy || !b) {
        d->bytes = calloc(1, n + 1);
        if (b)
            memcpy(d->bytes, b, n);
        d->owned = 1;
    } else {
        d->bytes = (uint8_t *)b;
    }
    return d;
}
static id data_from_file(objc_class *cls, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    ns_data *d = data_new(cls, NULL, n, 1);
    fread(d->bytes, 1, n, f);
    fclose(f);
    return d;
}
static id dt_data(objc_class *cls, SEL _c) { return AR(data_new(cls, NULL, 0, 1)); }
static id dt_withBytes(objc_class *cls, SEL _c, const void *b, int n) { return AR(data_new(cls, b, n, 1)); }
static id dt_withBytesNoCopy(objc_class *cls, SEL _c, void *b, int n) {
    ns_data *d = data_new(cls, b, n, 0);
    d->owned = 1; // NoCopy sin freeWhenDone:NO = se adueña
    return AR(d);
}
static id dt_withBytesNoCopyFree(objc_class *cls, SEL _c, void *b, int n, int fr) {
    ns_data *d = data_new(cls, b, n, 0);
    d->owned = fr;
    return AR(d);
}
static id dt_withContentsOfFile(objc_class *cls, SEL _c, id path) {
    id d = data_from_file(cls, ns_cstr(path));
    return d ? AR(d) : NULL;
}
static id dt_initWithContentsOfFile(id self, SEL _c, id path) {
    id d = data_from_file(object_getClass(self), ns_cstr(path));
    objc_free(self);
    return d;
}
static id dt_initWithBytes(ns_data *self, SEL _c, const void *b, int n) {
    self->bytes = calloc(1, n + 1);
    memcpy(self->bytes, b, n);
    self->len = self->cap = n;
    self->owned = 1;
    return self;
}
static id dt_initWithLength(ns_data *self, SEL _c, int n) {
    self->bytes = calloc(1, n + 1);
    self->len = self->cap = n;
    self->owned = 1;
    return self;
}
static id dt_withLength(objc_class *cls, SEL _c, int n) { return AR(data_new(cls, NULL, n, 1)); }
static void dt_dealloc(ns_data *self, SEL _c) {
    if (self->owned)
        free(self->bytes);
    objc_free(self);
}
static int dt_length(ns_data *self, SEL _c) { return self->len; }
static void *dt_bytes(ns_data *self, SEL _c) { return self->bytes; }
static void dt_getBytes(ns_data *self, SEL _c, void *out, int n) { memcpy(out, self->bytes, n < self->len ? n : self->len); }
static int dt_writeToFile(ns_data *self, SEL _c, id path, int atomic) {
    FILE *f = fopen(ns_cstr(path), "wb");
    if (!f)
        return 0;
    fwrite(self->bytes, 1, self->len, f);
    fclose(f);
    return 1;
}
static void dt_grow(ns_data *self, int n) {
    if (n > self->cap || !self->owned) {
        uint8_t *b = calloc(1, n * 2 + 16);
        memcpy(b, self->bytes, self->len);
        if (self->owned)
            free(self->bytes);
        self->bytes = b;
        self->cap = n * 2 + 15;
        self->owned = 1;
    }
}
static void dt_appendBytes(ns_data *self, SEL _c, const void *b, int n) {
    dt_grow(self, self->len + n);
    memcpy(self->bytes + self->len, b, n);
    self->len += n;
}
static void dt_appendData(ns_data *self, SEL _c, ns_data *o) {
    if (o)
        dt_appendBytes(self, _c, o->bytes, o->len);
}
static void dt_setLength(ns_data *self, SEL _c, int n) {
    dt_grow(self, n);
    if (n > self->len)
        memset(self->bytes + self->len, 0, n - self->len);
    self->len = n;
}
static void dt_increaseLength(ns_data *self, SEL _c, int n) { dt_setLength(self, _c, self->len + n); }
static id dt_copy(ns_data *self, SEL _c) { return data_new(c_NSData, self->bytes, self->len, 1); }
static const nmethod nsdata_c[] = {
    NM("data", dt_data), NM("dataWithBytes:length:", dt_withBytes), NM("dataWithBytesNoCopy:length:", dt_withBytesNoCopy),
    NM("dataWithBytesNoCopy:length:freeWhenDone:", dt_withBytesNoCopyFree),
    NM("dataWithContentsOfFile:", dt_withContentsOfFile), NM("dataWithLength:", dt_withLength),
    NM("dataWithCapacity:", dt_withLength), {0}};
static const nmethod nsdata_i[] = {
    NM("initWithContentsOfFile:", dt_initWithContentsOfFile), NM("initWithBytes:length:", dt_initWithBytes),
    NM("initWithLength:", dt_initWithLength), NM("initWithCapacity:", dt_initWithLength), NM("dealloc", dt_dealloc),
    NM("length", dt_length), NM("bytes", dt_bytes), NM("mutableBytes", dt_bytes), NM("getBytes:length:", dt_getBytes),
    NM("writeToFile:atomically:", dt_writeToFile), NM("appendBytes:length:", dt_appendBytes),
    NM("appendData:", dt_appendData), NM("setLength:", dt_setLength), NM("increaseLengthBy:", dt_increaseLength),
    NM("copy", dt_copy), {0}};

// ================================================================ plist binario (Info.plist, dictionaryWithContentsOfFile:)

typedef struct {
    const uint8_t *d;
    int n, osz, rsz;
    const uint8_t *offs;
} bplist;
static uint64_t be(const uint8_t *p, int n) {
    uint64_t v = 0;
    while (n--)
        v = v << 8 | *p++;
    return v;
}
static id bp_obj(bplist *b, uint64_t ref, int depth);
static uint64_t bp_len(bplist *b, const uint8_t **p, int lo) {
    if (lo != 0xF)
        return lo;
    int m = **p & 0xF;
    uint64_t v = be(*p + 1, 1 << m);
    *p += 1 + (1 << m);
    return v;
}
static id bp_obj(bplist *b, uint64_t ref, int depth) {
    if (depth > 32)
        return NULL;
    uint64_t off = be(b->offs + ref * b->osz, b->osz);
    if (off >= (uint64_t)b->n)
        return NULL;
    const uint8_t *p = b->d + off;
    int hi = *p >> 4, lo = *p & 0xF;
    p++;
    switch (hi) {
    case 0x0:
        return lo == 8 || lo == 9 ? num_new(2, lo == 9, 0) : NULL;
    case 0x1:
        return num_new(0, (int64_t)be(p, 1 << lo), 0);
    case 0x2: {
        uint64_t v = be(p, 1 << lo);
        double d;
        if (lo == 2) {
            uint32_t f32 = v;
            float f;
            memcpy(&f, &f32, 4);
            d = f;
        } else {
            memcpy(&d, &v, 8);
        }
        return num_new(1, 0, d);
    }
    case 0x3: {
        uint64_t v = be(p, 8);
        double d;
        memcpy(&d, &v, 8);
        return num_new(1, 0, d);
    }
    case 0x4: {
        uint64_t n = bp_len(b, &p, lo);
        return data_new(c_NSData, p, n, 1);
    }
    case 0x5: {
        uint64_t n = bp_len(b, &p, lo);
        ns_string *s = objc_alloc(c_NSString);
        return str_init_bytes(s, (const char *)p, n, ENC_LATIN1);
    }
    case 0x6: {
        uint64_t n = bp_len(b, &p, lo);
        uint16_t *u = malloc(n * 2 + 2);
        for (uint64_t i = 0; i < n; i++)
            u[i] = p[i * 2] << 8 | p[i * 2 + 1];
        char *s8 = utf16_to_utf8(u, n);
        free(u);
        id r = ns_string_new(s8);
        free(s8);
        return r;
    }
    case 0xA: {
        uint64_t n = bp_len(b, &p, lo);
        id a = objc_alloc(c_NSMutableArray);
        for (uint64_t i = 0; i < n; i++) {
            id o = bp_obj(b, be(p + i * b->rsz, b->rsz), depth + 1);
            ns_array_add(a, o);
            objc_release(o);
        }
        return a;
    }
    case 0xD: {
        uint64_t n = bp_len(b, &p, lo);
        id d = objc_alloc(c_NSMutableDictionary);
        for (uint64_t i = 0; i < n; i++) {
            id k = bp_obj(b, be(p + i * b->rsz, b->rsz), depth + 1);
            id v = bp_obj(b, be(p + (n + i) * b->rsz, b->rsz), depth + 1);
            ns_dict_set(d, k, v);
            objc_release(k);
            objc_release(v);
        }
        return d;
    }
    }
    return NULL;
}

// plist XML (partículas de cocos2d: ptc/NNN.plist): dict, array, key, string, integer, real, true, false.
// ponytail: <data> y <date> se devuelven como nil; agregarlos si algún plist del juego los usa.
static const char *xp_skip(const char *p) {
    for (;;) {
        while (*p && *p != '<')
            p++;
        if (!strncmp(p, "<?", 2) || !strncmp(p, "<!", 2) || !strncmp(p, "<plist", 6) || !strncmp(p, "</plist", 7)) {
            p = strchr(p, '>');
            if (!p)
                return "";
            p++;
            continue;
        }
        return p;
    }
}
static char *xp_text(const char *p, const char *end) { // texto hasta '<' con &amp; &lt; &gt; &quot; &apos;
    char *o = malloc(end - p + 1), *q = o;
    while (p < end) {
        static const struct {
            const char *e;
            char c;
        } ent[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
        unsigned k = 0;
        if (*p == '&')
            for (; k < 5 && strncmp(p, ent[k].e, strlen(ent[k].e)); k++)
                ;
        if (*p == '&' && k < 5)
            *q++ = ent[k].c, p += strlen(ent[k].e);
        else
            *q++ = *p++;
    }
    *q = 0;
    return o;
}
static id xp_obj(const char **pp, int depth) {
    const char *p = xp_skip(*pp);
    id r = NULL;
    char tag[16] = "";
    int n = 0;
    if (*p != '<' || depth > 64)
        return *pp = p, NULL;
    for (p++; *p && *p != '>' && *p != '/' && *p != ' ' && n < 15; p++)
        tag[n++] = *p;
    tag[n] = 0;
    int empty = 0;
    while (*p && *p != '>')
        empty = *p++ == '/';
    if (*p)
        p++;
    if (!strcmp(tag, "true") || !strcmp(tag, "false")) {
        r = num_new(2, tag[0] == 't', 0);
    } else if (!strcmp(tag, "dict") || !strcmp(tag, "array")) {
        int dict = tag[0] == 'd';
        r = objc_alloc(dict ? c_NSMutableDictionary : c_NSMutableArray);
        while (!empty) {
            p = xp_skip(p);
            if (!*p || p[1] == '/') { // </dict> o </array>
                p = *p ? strchr(p, '>') + 1 : p;
                break;
            }
            id k = dict ? xp_obj(&p, depth + 1) : NULL, v = xp_obj(&p, depth + 1);
            if (dict && k && v)
                ns_dict_set(r, k, v);
            else if (!dict && v)
                ns_array_add(r, v);
            objc_release(k);
            objc_release(v);
            if (!*p)
                break;
        }
    } else {
        const char *end = empty ? p : strchr(p, '<');
        if (!end)
            end = p + strlen(p);
        char *t = xp_text(p, end);
        if (!strcmp(tag, "key") || !strcmp(tag, "string"))
            r = ns_string_new(t);
        else if (!strcmp(tag, "integer"))
            r = num_new(0, strtoll(t, NULL, 10), 0);
        else if (!strcmp(tag, "real"))
            r = num_new(1, 0, strtod(t, NULL));
        free(t);
        p = end;
        if (!empty && *p) {
            const char *gt = strchr(p, '>'); // cierre </tag>
            p = gt ? gt + 1 : p + strlen(p);
        }
    }
    *pp = p;
    return r;
}

id plist_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(n);
    fread(d, 1, n, f);
    fclose(f);
    id r = NULL;
    if (n > 40 && !memcmp(d, "bplist00", 8)) {
        const uint8_t *t = d + n - 32;
        bplist b = {d, n, t[6], t[7], NULL};
        uint64_t nobj = be(t + 8, 8), top = be(t + 16, 8), toff = be(t + 24, 8);
        b.offs = d + toff;
        if (toff < (uint64_t)n && top < nobj)
            r = bp_obj(&b, top, 0);
    } else {
        d = realloc(d, n + 1);
        d[n] = 0;
        const char *p = (const char *)d;
        r = xp_obj(&p, 0);
        if (!r)
            l_error("plist: no se pudo leer %s", path);
    }
    free(d);
    return r;
}

// Escritura: formato propio de texto para NSUserDefaults/writeToFile (lo lee plist_load_text).
static void pl_write_obj(FILE *f, id o, int ind) {
    if (!o) {
        fprintf(f, "N\n");
    } else if (objc_is_kind(o, c_NSString)) {
        fprintf(f, "S %u\n%s\n", (unsigned)strlen(ns_cstr(o)), ns_cstr(o));
    } else if (objc_is_kind(o, c_NSNumber)) {
        ns_number *n = o;
        if (n->type == 1)
            fprintf(f, "D %.17g\n", n->d);
        else
            fprintf(f, "%c %lld\n", n->type == 2 ? 'B' : 'I', (long long)n->i);
    } else if (objc_is_kind(o, c_NSData)) {
        ns_data *d = o;
        fprintf(f, "X %d\n", d->len);
        fwrite(d->bytes, 1, d->len, f);
        fputc('\n', f);
    } else if (objc_is_kind(o, c_NSDictionary)) {
        ns_dict *d = o;
        fprintf(f, "{ %d\n", d->count);
        for (int i = 0; i < d->count; i++) {
            pl_write_obj(f, d->keys[i], ind + 1);
            pl_write_obj(f, d->vals[i], ind + 1);
        }
    } else if (objc_is_kind(o, c_NSArray)) {
        ns_array *a = o;
        fprintf(f, "[ %d\n", a->count);
        for (int i = 0; i < a->count; i++)
            pl_write_obj(f, a->items[i], ind + 1);
    } else {
        fprintf(f, "N\n");
    }
}
static int plist_write(const char *path, id obj) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return 0;
    fprintf(f, "MLPL1\n");
    pl_write_obj(f, obj, 0);
    fclose(f);
    return 1;
}
static id pl_read_obj(FILE *f) {
    char line[64];
    if (!fgets(line, sizeof(line), f))
        return NULL;
    long long v;
    double d;
    int n;
    switch (line[0]) {
    case 'S': {
        n = atoi(line + 2);
        char *b = malloc(n + 2);
        fread(b, 1, n + 1, f);
        b[n] = 0;
        id s = ns_string_new(b);
        free(b);
        return s;
    }
    case 'I':
    case 'B':
        sscanf(line + 2, "%lld", &v);
        return num_new(line[0] == 'B' ? 2 : 0, v, 0);
    case 'D':
        sscanf(line + 2, "%lf", &d);
        return num_new(1, 0, d);
    case 'X': {
        n = atoi(line + 2);
        ns_data *x = data_new(c_NSMutableData, NULL, n, 1);
        fread(x->bytes, 1, n, f);
        fgetc(f);
        return x;
    }
    case '{': {
        n = atoi(line + 2);
        id dct = objc_alloc(c_NSMutableDictionary);
        for (int i = 0; i < n; i++) {
            id k = pl_read_obj(f), val = pl_read_obj(f);
            ns_dict_set(dct, k, val);
            objc_release(k);
            objc_release(val);
        }
        return dct;
    }
    case '[': {
        n = atoi(line + 2);
        id a = objc_alloc(c_NSMutableArray);
        for (int i = 0; i < n; i++) {
            id o = pl_read_obj(f);
            ns_array_add(a, o);
            objc_release(o);
        }
        return a;
    }
    }
    return NULL;
}
static id plist_load_text(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char hdr[16];
    id r = NULL;
    if (fgets(hdr, sizeof(hdr), f) && !strncmp(hdr, "MLPL1", 5))
        r = pl_read_obj(f);
    fclose(f);
    return r;
}

// ================================================================ NSBundle / rutas / idioma

static objc_class *c_NSBundle, *c_NSUserDefaults, *c_NSFileManager, *c_NSLocale;
static id main_bundle, info_dict, user_defaults, defaults_dict, file_manager, cur_locale;
static char lang_code[8] = "en";

static void detect_language(void) {
    int lang = 1;
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &lang);
    static const char *codes[] = {"ja", "en", "fr", "es", "de", "it", "nl", "pt", "ru", "ko", "zh-Hant", "zh-Hans",
                                  "fi", "sv", "da", "no", "pl", "pt", "en", "tr"};
    if (lang >= 0 && lang < (int)(sizeof(codes) / sizeof(codes[0])))
        snprintf(lang_code, sizeof(lang_code), "%s", codes[lang]);
    l_info("idioma del sistema: %s", lang_code);
}
const char *ns_language(void) { return lang_code; }

static id path_no_slash(const char *p) {
    char b[256];
    snprintf(b, sizeof(b), "%s", p);
    size_t l = strlen(b);
    if (l && b[l - 1] == '/')
        b[l - 1] = 0;
    return ns_str(b);
}
static id b_mainBundle(id cls, SEL _c) { return main_bundle; }
static id b_resourcePath(id self, SEL _c) { return path_no_slash(BUNDLE_PATH); }
static id b_pathForResource(id self, SEL _c, id name, id type, id dir) {
    char p[512];
    const char *n = ns_cstr(name), *t = ns_cstr(type), *d = dir ? ns_cstr(dir) : "";
    struct stat st;
    // 1) carpeta pedida 2) <idioma>.lproj 3) en.lproj
    const char *lp[] = {d, NULL, "en.lproj"};
    char lproj[32];
    snprintf(lproj, sizeof(lproj), "%s.lproj", lang_code);
    lp[1] = lproj;
    for (int i = 0; i < 3; i++) {
        if (i && *d)
            break;
        snprintf(p, sizeof(p), BUNDLE_PATH "%s%s%s%s%s", lp[i], *lp[i] ? "/" : "", n, *t ? "." : "", t);
        if (!stat(p, &st))
            return ns_str(p);
        if (!i) {
            snprintf(p, sizeof(p), BUNDLE_PATH "%s%s%s", n, *t ? "." : "", t);
            if (!stat(p, &st))
                return ns_str(p);
        }
    }
    l_debug("NSBundle pathForResource:%s ofType:%s -> nil", n, t);
    return NULL;
}
static id b_pathForResource2(id self, SEL _c, id name, id type) { return b_pathForResource(self, _c, name, type, NULL); }
static id b_bundleIdentifier(id self, SEL _c) {
    id v = ns_dict_get_c(info_dict, "CFBundleIdentifier");
    return v ? v : ns_str("com.gameloft.game");
}
static id b_infoDictionary(id self, SEL _c) { return info_dict; }
static id b_objectForInfoKey(id self, SEL _c, id k) { return ns_dict_get(info_dict, k); }
static id b_localizedString(id self, SEL _c, id key, id value, id table) {
    return value && *ns_cstr(value) ? value : key;
}
static id b_preferredLocalizations(id self, SEL _c) {
    id a = ns_array_new();
    ns_array_add(a, ns_str(lang_code));
    return AR(a);
}
static const nmethod nsbundle_c[] = {NM("mainBundle", b_mainBundle), {0}};
static const nmethod nsbundle_i[] = {
    NM("resourcePath", b_resourcePath), NM("bundlePath", b_resourcePath),
    NM("pathForResource:ofType:", b_pathForResource2), NM("pathForResource:ofType:inDirectory:", b_pathForResource),
    NM("bundleIdentifier", b_bundleIdentifier), NM("infoDictionary", b_infoDictionary),
    NM("localizedInfoDictionary", b_infoDictionary), NM("objectForInfoDictionaryKey:", b_objectForInfoKey),
    NM("localizedStringForKey:value:table:", b_localizedString), NM("preferredLocalizations", b_preferredLocalizations),
    {0}};

// NSUserDefaults: diccionario persistido en DATA_PATH/Library/Preferences/defaults.txt
#define DEFAULTS_PATH DATA_PATH "Library/Preferences/defaults.txt"
static id ud_standard(id cls, SEL _c) { return user_defaults; }
static id ud_objectForKey(id self, SEL _c, id k) { return ns_dict_get(defaults_dict, k); }
static void ud_setObject(id self, SEL _c, id v, id k) {
    if (v)
        ns_dict_set(defaults_dict, k, v);
    else
        d_removeObjectForKey(defaults_dict, _c, k);
}
static void ud_remove(id self, SEL _c, id k) { d_removeObjectForKey(defaults_dict, _c, k); }
static int ud_integerForKey(id self, SEL _c, id k) {
    id v = ns_dict_get(defaults_dict, k);
    return !v ? 0 : objc_is_kind(v, c_NSNumber) ? (int)num_i(v) : atoi(ns_cstr(v));
}
static int ud_boolForKey(id self, SEL _c, id k) { return ud_integerForKey(self, _c, k) != 0; }
static float ud_floatForKey(id self, SEL _c, id k) {
    id v = ns_dict_get(defaults_dict, k);
    return !v ? 0 : objc_is_kind(v, c_NSNumber) ? (float)num_d(v) : strtof(ns_cstr(v), NULL);
}
static double ud_doubleForKey(id self, SEL _c, id k) { return ud_floatForKey(self, _c, k); }
static void ud_setInteger(id self, SEL _c, int v, id k) { ns_dict_set(defaults_dict, k, AR(num_new(0, v, 0))); }
static void ud_setBool(id self, SEL _c, int v, id k) { ns_dict_set(defaults_dict, k, AR(num_new(2, v != 0, 0))); }
static void ud_setFloat(id self, SEL _c, float v, id k) { ns_dict_set(defaults_dict, k, AR(num_new(1, 0, v))); }
static void ud_setDouble(id self, SEL _c, double v, id k) { ns_dict_set(defaults_dict, k, AR(num_new(1, 0, v))); }
static id ud_stringForKey(id self, SEL _c, id k) {
    id v = ns_dict_get(defaults_dict, k);
    return v && objc_is_kind(v, c_NSString) ? v : NULL;
}
static int ud_synchronize(id self, SEL _c) {
    mkdir(DATA_PATH "Library", 0777);
    mkdir(DATA_PATH "Library/Preferences", 0777);
    return plist_write(DEFAULTS_PATH, defaults_dict);
}
static void ud_registerDefaults(id self, SEL _c, ns_dict *d) {
    for (int i = 0; d && i < d->count; i++)
        if (!ns_dict_get(defaults_dict, d->keys[i]))
            ns_dict_set(defaults_dict, d->keys[i], d->vals[i]);
}
static id ud_dict(id self, SEL _c) { return defaults_dict; }
static const nmethod nsud_c[] = {NM("standardUserDefaults", ud_standard), {0}};
static const nmethod nsud_i[] = {
    NM("objectForKey:", ud_objectForKey), NM("setObject:forKey:", ud_setObject), NM("setValue:forKey:", ud_setObject),
    NM("removeObjectForKey:", ud_remove), NM("integerForKey:", ud_integerForKey), NM("boolForKey:", ud_boolForKey),
    NM("floatForKey:", ud_floatForKey), NM("doubleForKey:", ud_doubleForKey), NM("setInteger:forKey:", ud_setInteger),
    NM("setBool:forKey:", ud_setBool), NM("setFloat:forKey:", ud_setFloat), NM("setDouble:forKey:", ud_setDouble),
    NM("stringForKey:", ud_stringForKey), NM("arrayForKey:", ud_objectForKey), NM("dictionaryForKey:", ud_objectForKey),
    NM("dataForKey:", ud_objectForKey), NM("synchronize", ud_synchronize), NM("registerDefaults:", ud_registerDefaults),
    NM("dictionaryRepresentation", ud_dict), {0}};

// NSFileManager
static id fm_default(id cls, SEL _c) { return file_manager; }
static int fm_exists(id self, SEL _c, id path) {
    struct stat st;
    return !stat(ns_cstr(path), &st);
}
static int fm_existsDir(id self, SEL _c, id path, int8_t *isdir) {
    struct stat st;
    int r = !stat(ns_cstr(path), &st);
    if (isdir && (uintptr_t)isdir > 0x1000)
        *isdir = r && S_ISDIR(st.st_mode);
    return r;
}
static int mkdir_p(const char *path) {
    char b[512];
    snprintf(b, sizeof(b), "%s", path);
    for (char *p = b + 5; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(b, 0777);
            *p = '/';
        }
    return !mkdir(b, 0777) || fm_exists(NULL, NULL, ns_str(b));
}
static int fm_createDir(id self, SEL _c, id path, int inter, id attrs, id *err) { return mkdir_p(ns_cstr(path)); }
static int fm_createDir2(id self, SEL _c, id path, id attrs) { return mkdir_p(ns_cstr(path)); }
static int fm_remove(id self, SEL _c, id path, id *err) {
    const char *p = ns_cstr(path);
    return !remove(p) || !rmdir(p);
}
static int fm_copy(id self, SEL _c, id from, id to, id *err) {
    FILE *a = fopen(ns_cstr(from), "rb"), *b = a ? fopen(ns_cstr(to), "wb") : NULL;
    char buf[8192];
    size_t n;
    while (a && b && (n = fread(buf, 1, sizeof(buf), a)) > 0)
        fwrite(buf, 1, n, b);
    if (a)
        fclose(a);
    if (b)
        fclose(b);
    return a && b;
}
static int fm_move(id self, SEL _c, id from, id to, id *err) { return !rename(ns_cstr(from), ns_cstr(to)); }
static id fm_contents(id self, SEL _c, id path, id *err) {
    DIR *d = opendir(ns_cstr(path));
    if (!d)
        return NULL;
    id a = ns_array_new();
    struct dirent *e;
    while ((e = readdir(d)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
            ns_array_add(a, ns_str(e->d_name));
    closedir(d);
    return AR(a);
}
static id fm_attributes(id self, SEL _c, id path, id *err) {
    struct stat st;
    if (stat(ns_cstr(path), &st))
        return NULL;
    id d = ns_dict_new();
    ns_dict_set(d, ns_str("NSFileSize"), ns_num_int(st.st_size));
    ns_dict_set(d, ns_str("NSFileType"), ns_str(S_ISDIR(st.st_mode) ? "NSFileTypeDirectory" : "NSFileTypeRegular"));
    return AR(d);
}
static int fm_createFile(id self, SEL _c, id path, ns_data *data, id attrs) {
    FILE *f = fopen(ns_cstr(path), "wb");
    if (!f)
        return 0;
    if (data)
        fwrite(data->bytes, 1, data->len, f);
    fclose(f);
    return 1;
}
static id fm_contentsAtPath(id self, SEL _c, id path) { return dt_withContentsOfFile(c_NSData, _c, path); }
static id fm_currentDir(id self, SEL _c) { return path_no_slash(BUNDLE_PATH); }
static int fm_yes(id self, SEL _c, id path) { return 1; }
static const nmethod nsfm_c[] = {NM("defaultManager", fm_default), {0}};
static const nmethod nsfm_i[] = {
    NM("fileExistsAtPath:", fm_exists), NM("fileExistsAtPath:isDirectory:", fm_existsDir),
    NM("createDirectoryAtPath:withIntermediateDirectories:attributes:error:", fm_createDir),
    NM("createDirectoryAtPath:attributes:", fm_createDir2), NM("removeItemAtPath:error:", fm_remove),
    NM("removeFileAtPath:handler:", fm_remove), NM("copyItemAtPath:toPath:error:", fm_copy),
    NM("moveItemAtPath:toPath:error:", fm_move), NM("contentsOfDirectoryAtPath:error:", fm_contents),
    NM("directoryContentsAtPath:", fm_contents), NM("attributesOfItemAtPath:error:", fm_attributes),
    NM("fileAttributesAtPath:traverseLink:", fm_attributes), NM("createFileAtPath:contents:attributes:", fm_createFile),
    NM("contentsAtPath:", fm_contentsAtPath), NM("currentDirectoryPath", fm_currentDir),
    NM("isWritableFileAtPath:", fm_yes), NM("isReadableFileAtPath:", fm_exists), {0}};

// NSLocale
static id lc_current(id cls, SEL _c) { return cur_locale; }
static id lc_preferredLanguages(id cls, SEL _c) { return b_preferredLocalizations(NULL, _c); }
static id lc_objectForKey(id self, SEL _c, id k) {
    const char *key = ns_cstr(k);
    if (strstr(key, "Country"))
        return ns_str("US");
    if (strstr(key, "Language"))
        return ns_str(lang_code);
    if (strstr(key, "Identifier"))
        return MSG0(self, "localeIdentifier");
    return NULL;
}
static id lc_identifier(id self, SEL _c) {
    char b[16];
    snprintf(b, sizeof(b), "%s_US", lang_code);
    return ns_str(b);
}
static const nmethod nslocale_c[] = {
    NM("currentLocale", lc_current), NM("systemLocale", lc_current), NM("autoupdatingCurrentLocale", lc_current),
    NM("preferredLanguages", lc_preferredLanguages), {0}};
static const nmethod nslocale_i[] = {NM("objectForKey:", lc_objectForKey), NM("localeIdentifier", lc_identifier), {0}};

// NSSearchPathForDirectoriesInDomains / NSHomeDirectory
static id f_NSSearchPath(int dir, int mask, int expand) {
    const char *p = dir == 9 ? DOCS_PATH : dir == 13 ? DATA_PATH "Library/Caches/" : DATA_PATH "Library/";
    mkdir_p(p);
    id a = ns_array_new();
    ns_array_add(a, path_no_slash(p));
    return AR(a);
}
static id f_NSHomeDirectory(void) { return path_no_slash(DATA_PATH); }
static id f_NSTemporaryDirectory(void) {
    mkdir_p(DATA_PATH "tmp");
    return ns_str(DATA_PATH "tmp/");
}
static objc_class *f_NSClassFromString(id s) { return objc_getClass(ns_cstr(s)); }
static id f_NSStringFromClass(objc_class *c) { return ns_str(class_getName(c)); }
static SEL f_NSSelectorFromString(id s) { return sel_registerName(ns_cstr(s)); }
static id f_NSStringFromSelector(SEL s) { return ns_str(s); }

static void f_NSLog(id fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    words w = {&ap, NULL};
    darwin_vformat(buf, sizeof(buf), ns_cstr(fmt), &w);
    va_end(ap);
    l_info("NSLog: %s", buf);
}

const char *objc_describe(void *o) {
    if (!o)
        return "(null)";
    if (objc_is_kind(o, c_NSString))
        return ns_cstr(o);
    return ns_cstr((id)MSG0(o, "description"));
}

// ================================================================ tiempo: NSDate, NSTimer, run loop

double ns_now(void) { // segundos desde 2001-01-01 (CFAbsoluteTime)
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec - 978307200.0 + tv.tv_usec / 1e6;
}
static double mono(void) { return sceKernelGetProcessTimeWide() / 1e6; }

typedef struct {
    objc_class *isa;
    double t; // CFAbsoluteTime
} ns_date;
static id date_new(double t) {
    ns_date *d = objc_alloc(c_NSDate);
    d->t = t;
    return AR(d);
}
static id dd_date(id cls, SEL _c) { return date_new(ns_now()); }
static id dd_sinceNow(id cls, SEL _c, double s) { return date_new(ns_now() + s); }
static id dd_since1970(id cls, SEL _c, double s) { return date_new(s - 978307200.0); }
static id dd_distantFuture(id cls, SEL _c) { return date_new(1e12); }
static id dd_distantPast(id cls, SEL _c) { return date_new(-1e12); }
static double dd_ti1970(ns_date *self, SEL _c) { return self->t + 978307200.0; }
static double dd_tiRef(ns_date *self, SEL _c) { return self->t; }
static double dd_tiNow(ns_date *self, SEL _c) { return self->t - ns_now(); }
static double dd_tiSince(ns_date *self, SEL _c, ns_date *o) { return self->t - (o ? o->t : 0); }
static double dd_tiRefCls(id cls, SEL _c) { return ns_now(); }
static id dd_init(ns_date *self, SEL _c) { self->t = ns_now(); return self; }
static id dd_description(ns_date *self, SEL _c) {
    char b[64];
    time_t t = (time_t)(self->t + 978307200.0);
    strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S +0000", gmtime(&t));
    return ns_str(b);
}
static const nmethod nsdate_c[] = {
    NM("date", dd_date), NM("dateWithTimeIntervalSinceNow:", dd_sinceNow), NM("dateWithTimeIntervalSince1970:", dd_since1970),
    NM("distantFuture", dd_distantFuture), NM("distantPast", dd_distantPast),
    NM("timeIntervalSinceReferenceDate", dd_tiRefCls), {0}};
static const nmethod nsdate_i[] = {
    NM("init", dd_init), NM("timeIntervalSince1970", dd_ti1970), NM("timeIntervalSinceReferenceDate", dd_tiRef),
    NM("timeIntervalSinceNow", dd_tiNow), NM("timeIntervalSinceDate:", dd_tiSince), NM("description", dd_description), {0}};

typedef struct {
    objc_class *isa;
    double interval, next; // reloj monotónico
    id target, userInfo;
    SEL sel;
    int repeats, valid;
    SceUID owner; // hilo cuyo run loop lo dispara (el que lo creó, como en iOS)
} ns_timer;
#define MAX_TIMERS 128
static ns_timer *timers[MAX_TIMERS];
static int ntimers;
static pthread_mutex_t timer_lock = PTHREAD_MUTEX_INITIALIZER;
static SceUID main_thread_id;

static id timer_make(double ti, id target, SEL sel, id ui, int rep) {
    ns_timer *t = objc_alloc(c_NSTimer);
    t->interval = ti < 0.001 ? 0.001 : ti;
    t->next = mono() + t->interval;
    t->target = objc_retain(target);
    t->userInfo = objc_retain(ui);
    t->sel = sel_registerName(sel);
    t->repeats = rep & 0xFF;
    t->valid = 1;
    t->owner = sceKernelGetThreadId();
    return t;
}
static void timer_schedule(ns_timer *t) {
    pthread_mutex_lock(&timer_lock);
    if (ntimers < MAX_TIMERS)
        timers[ntimers++] = objc_retain(t);
    pthread_mutex_unlock(&timer_lock);
    static int nlog; // hay juegos que programan un timer de un disparo por frame: el log a ux0 costaba ms por frame
    if (nlog++ < 30)
        l_info("NSTimer %.4fs -> -[%s %s]%s", t->interval, class_getName(object_getClass(t->target)), t->sel,
               t->repeats ? " (repite)" : "");
}
static id tm_scheduled(id cls, SEL _c, double ti, id target, SEL sel, id ui, int rep) {
    ns_timer *t = timer_make(ti, target, sel, ui, rep);
    timer_schedule(t);
    return AR(t);
}
static id tm_timerWith(id cls, SEL _c, double ti, id target, SEL sel, id ui, int rep) {
    return AR(timer_make(ti, target, sel, ui, rep));
}
static void tm_invalidate(ns_timer *self, SEL _c) { self->valid = 0; }
static int tm_isValid(ns_timer *self, SEL _c) { return self->valid; }
static id tm_userInfo(ns_timer *self, SEL _c) { return self->userInfo; }
static void tm_fire(ns_timer *self, SEL _c) { objc_msg(self->target, self->sel, (uint32_t)self, 0, 0, 0); }
static double tm_timeInterval(ns_timer *self, SEL _c) { return self->interval; }
static void tm_setFireDate(ns_timer *self, SEL _c, ns_date *d) { self->next = mono() + (d ? d->t - ns_now() : 0); }
static void tm_dealloc(ns_timer *self, SEL _c) {
    objc_release(self->target);
    objc_release(self->userInfo);
    objc_free(self);
}
static const nmethod nstimer_c[] = {
    NM("scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:", tm_scheduled),
    NM("timerWithTimeInterval:target:selector:userInfo:repeats:", tm_timerWith), {0}};
static const nmethod nstimer_i[] = {
    NM("invalidate", tm_invalidate), NM("isValid", tm_isValid), NM("userInfo", tm_userInfo), NM("fire", tm_fire),
    NM("timeInterval", tm_timeInterval), NM("setFireDate:", tm_setFireDate), NM("dealloc", tm_dealloc), {0}};

// Timer repetitivo creado desde nuestro C (GLKViewController, CADisplayLink): +1
id ns_timer_schedule_c(double interval, id target, SEL sel) {
    ns_timer *t = timer_make(interval, target, sel, NULL, 1);
    t->owner = main_thread_id; // CADisplayLink / GLKViewController: siempre en el run loop principal
    timer_schedule(t);
    return t;
}

// performSelector:withObject:afterDelay: = timer de un disparo con argumento
void ns_perform_later(id target, SEL s, id arg, double delay) {
    ns_timer *t = timer_make(delay, target, s, arg, 0);
    t->repeats = 2; // marca: pasa userInfo como argumento en vez del timer
    timer_schedule(t);
    objc_release(t);
}
void ns_cancel_performs(id target) {
    pthread_mutex_lock(&timer_lock);
    for (int i = 0; i < ntimers; i++)
        if (timers[i]->repeats == 2 && timers[i]->target == target)
            timers[i]->valid = 0;
    pthread_mutex_unlock(&timer_lock);
}

// Dispara los timers vencidos del hilo actual. Devuelve los segundos hasta el próximo (o 0.016).
// Los vencidos se juntan bajo el lock y se disparan sin él (el callback puede crear timers).
double ns_run_timers(void) {
    SceUID me = sceKernelGetThreadId();
    ns_timer *due[MAX_TIMERS];
    int ndue = 0;
    double now = mono(), wait = 0.016;
    pthread_mutex_lock(&timer_lock);
    for (int i = 0; i < ntimers; i++) {
        ns_timer *t = timers[i];
        if (t->owner != me)
            continue;
        if (t->valid && now >= t->next) {
            if (t->repeats == 1) {
                t->next += t->interval;
                if (t->next < now) // atrasado (carga, pausa): no acumular disparos
                    t->next = now + t->interval;
            } else {
                t->valid = 0;
            }
            due[ndue++] = objc_retain(t);
        }
        if (t->valid && t->next - now < wait)
            wait = t->next - now;
    }
    for (int i = 0; i < ntimers;) // purgar inválidos
        if (!timers[i]->valid) {
            objc_release(timers[i]);
            timers[i] = timers[--ntimers];
        } else {
            i++;
        }
    pthread_mutex_unlock(&timer_lock);
    for (int i = 0; i < ndue; i++) {
        ns_timer *t = due[i];
        objc_pool_push();
        objc_msg(t->target, t->sel, (uint32_t)(t->repeats == 2 ? t->userInfo : (id)t), 0, 0, 0);
        objc_pool_pop();
        objc_release(t);
    }
    return wait < 0 ? 0 : wait;
}

// Run loop de un hilo secundario: en iOS es propio del hilo (solo sus timers). El tick de UIKit
// (input, acelerómetro, cola main de GCD) es del principal y no es reentrante.
static int is_main_thread(void);
static void bg_runloop(double secs) {
    double end = mono() + secs;
    do {
        double w = ns_run_timers(), left = end - mono();
        if (w > left)
            w = left;
        if (w > 0.0005)
            sceKernelDelayThread((SceUInt)(w * 1e6));
    } while (mono() < end);
}

// NSRunLoop / CFRunLoop: el bucle real lo lleva ui_runloop_tick (uikit.c: input + timers + present)
void ui_runloop_tick(double max_wait);
void ui_runloop_forever(void) __attribute__((noreturn));
static objc_class *c_NSRunLoop;
static id run_loop;
static id rl_current(id cls, SEL _c) { return run_loop; }
static void rl_addTimer(id self, SEL _c, ns_timer *t, id mode) {
    int found = 0;
    for (int i = 0; i < ntimers; i++)
        found |= timers[i] == t;
    if (!found)
        timer_schedule(t);
}
static void rl_run(id self, SEL _c) {
    if (!is_main_thread())
        for (;;)
            bg_runloop(1);
    ui_runloop_forever();
}
static void rl_runUntilDate(id self, SEL _c, ns_date *d) {
    double end = d ? d->t : ns_now();
    if (!is_main_thread()) {
        bg_runloop(end - ns_now() > 0 ? end - ns_now() : 0);
        return;
    }
    do
        ui_runloop_tick(0.005);
    while (ns_now() < end);
}
static int rl_runMode(id self, SEL _c, id mode, ns_date *d) {
    if (!is_main_thread())
        bg_runloop(0);
    else
        ui_runloop_tick(0);
    return 1;
}
static const nmethod nsrl_c[] = {NM("currentRunLoop", rl_current), NM("mainRunLoop", rl_current), {0}};
static const nmethod nsrl_i[] = {
    NM("addTimer:forMode:", rl_addTimer), NM("run", rl_run), NM("runUntilDate:", rl_runUntilDate),
    NM("runMode:beforeDate:", rl_runMode), {0}};

// ================================================================ NSThread / NSLock / NSAutoreleasePool

typedef struct {
    objc_class *isa;
    id target, arg;
    SEL sel;
    int stack;
} ns_thread;
static void *thread_entry(void *p) {
    ns_thread *t = p;
    objc_pool_push();
    objc_msg(t->target, t->sel, (uint32_t)t->arg, 0, 0, 0);
    objc_pool_pop();
    return NULL;
}
static void thread_start(ns_thread *t) {
    pthread_t th;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, t->stack ? t->stack : 1024 * 1024);
    pthread_create(&th, &a, thread_entry, objc_retain(t));
    pthread_attr_destroy(&a);
    pthread_detach(th);
    l_info("NSThread -> -[%s %s]", class_getName(object_getClass(t->target)), t->sel);
}
static void th_detach(id cls, SEL _c, SEL sel, id target, id arg) {
    ns_thread *t = objc_alloc(c_NSThread);
    t->target = objc_retain(target);
    t->arg = objc_retain(arg);
    t->sel = sel_registerName(sel);
    thread_start(t);
}
static id th_initWithTarget(ns_thread *self, SEL _c, id target, SEL sel, id arg) {
    self->target = objc_retain(target);
    self->arg = objc_retain(arg);
    self->sel = sel_registerName(sel);
    return self;
}
static void th_start(ns_thread *self, SEL _c) { thread_start(self); }
static void th_setStackSize(ns_thread *self, SEL _c, int n) { self->stack = n; }
static int is_main_thread(void) { return sceKernelGetThreadId() == main_thread_id; }
static int th_isMain(id x, SEL _c) { return is_main_thread(); }
static id th_current(id cls, SEL _c) { return cls; }
static void th_sleep(id cls, SEL _c, double s) { sceKernelDelayThread(s > 0 ? (SceUInt)(s * 1e6) : 0); }
static void th_sleepUntil(id cls, SEL _c, ns_date *d) {
    double w = d ? d->t - ns_now() : 0;
    sceKernelDelayThread(w > 0 ? (SceUInt)(w * 1e6) : 0);
}
static int th_setPriority(id x, SEL _c, double p) { return 1; }
static void th_noop(id x, SEL _c) {}
static const nmethod nsthread_c[] = {
    NM("detachNewThreadSelector:toTarget:withObject:", th_detach), NM("isMainThread", th_isMain),
    NM("currentThread", th_current), NM("mainThread", th_current), NM("sleepForTimeInterval:", th_sleep),
    NM("sleepUntilDate:", th_sleepUntil), NM("setThreadPriority:", th_setPriority), NM("exit", th_noop), {0}};
static const nmethod nsthread_i[] = {
    NM("initWithTarget:selector:object:", th_initWithTarget), NM("start", th_start), NM("setStackSize:", th_setStackSize),
    NM("isMainThread", th_isMain), NM("setThreadPriority:", th_setPriority), NM("setName:", th_noop), {0}};

typedef struct {
    objc_class *isa;
    pthread_mutex_t m;
} ns_lock;
static id lk_init(ns_lock *self, SEL _c) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&self->m, &a);
    pthread_mutexattr_destroy(&a);
    return self;
}
static void lk_lock(ns_lock *self, SEL _c) { pthread_mutex_lock(&self->m); }
static void lk_unlock(ns_lock *self, SEL _c) { pthread_mutex_unlock(&self->m); }
static int lk_tryLock(ns_lock *self, SEL _c) { return !pthread_mutex_trylock(&self->m); }
static const nmethod nslock_i[] = {
    NM("init", lk_init), NM("lock", lk_lock), NM("unlock", lk_unlock), NM("tryLock", lk_tryLock), NM("wait", th_noop),
    NM("signal", th_noop), NM("broadcast", th_noop), {0}};

static id pool_init(id self, SEL _c) {
    objc_pool_push();
    return self;
}
static void pool_release(id self, SEL _c) {
    objc_pool_pop();
    objc_free(self);
}
static void pool_addObject(id cls, SEL _c, id o) { objc_autorelease(o); }
static const nmethod nspool_c[] = {NM("addObject:", pool_addObject), {0}};
static const nmethod nspool_i[] = {NM("init", pool_init), NM("release", pool_release), NM("drain", pool_release), {0}};

// ================================================================ NSNotificationCenter

typedef struct {
    id observer, object;
    SEL sel;
    char name[96];
} observer;
static observer observers[128];
static int nobservers;
static objc_class *c_NSNotificationCenter;
static id notif_center;
typedef struct {
    objc_class *isa;
    id name, object, userInfo;
} ns_notif;

static id nc_default(id cls, SEL _c) { return notif_center; }
static void nc_addObserver(id self, SEL _c, id obs, SEL sel, id name, id obj) {
    if (nobservers >= 128)
        return;
    observer *o = &observers[nobservers++];
    o->observer = obs;
    o->sel = sel_registerName(sel);
    o->object = obj;
    snprintf(o->name, sizeof(o->name), "%s", name ? ns_cstr(name) : "");
    l_debug("NSNotificationCenter: %s observa %s", class_getName(object_getClass(obs)), o->name);
}
static void nc_removeObserver(id self, SEL _c, id obs) {
    for (int i = 0; i < nobservers;)
        if (observers[i].observer == obs)
            observers[i] = observers[--nobservers];
        else
            i++;
}
static void nc_removeObserverNamed(id self, SEL _c, id obs, id name, id obj) {
    for (int i = 0; i < nobservers;)
        if (observers[i].observer == obs && (!name || !strcmp(observers[i].name, ns_cstr(name))))
            observers[i] = observers[--nobservers];
        else
            i++;
}
void ns_post_notification(const char *name, id object, id userInfo) {
    ns_notif *n = objc_alloc(c_NSNotification);
    n->name = ns_string_new(name);
    n->object = objc_retain(object);
    n->userInfo = objc_retain(userInfo);
    for (int i = 0; i < nobservers; i++)
        if ((!observers[i].name[0] || !strcmp(observers[i].name, name)) && (!observers[i].object || observers[i].object == object))
            objc_msg(observers[i].observer, observers[i].sel, (uint32_t)n, 0, 0, 0);
    objc_release(n);
}
static void nc_post(id self, SEL _c, id name, id obj) { ns_post_notification(ns_cstr(name), obj, NULL); }
static void nc_postUI(id self, SEL _c, id name, id obj, id ui) { ns_post_notification(ns_cstr(name), obj, ui); }
static void nc_postN(id self, SEL _c, ns_notif *n) { ns_post_notification(ns_cstr(n->name), n->object, n->userInfo); }
static const nmethod nsnc_c[] = {NM("defaultCenter", nc_default), {0}};
static const nmethod nsnc_i[] = {
    NM("addObserver:selector:name:object:", nc_addObserver), NM("removeObserver:", nc_removeObserver),
    NM("removeObserver:name:object:", nc_removeObserverNamed), NM("postNotificationName:object:", nc_post),
    NM("postNotificationName:object:userInfo:", nc_postUI), NM("postNotification:", nc_postN), {0}};
static id nt_name(ns_notif *self, SEL _c) { return self->name; }
static id nt_object(ns_notif *self, SEL _c) { return self->object; }
static id nt_userInfo(ns_notif *self, SEL _c) { return self->userInfo; }
static id nt_with(id cls, SEL _c, id name, id obj) {
    ns_notif *n = objc_alloc(c_NSNotification);
    n->name = objc_retain(name);
    n->object = objc_retain(obj);
    return AR(n);
}
static const nmethod nsnotif_c[] = {NM("notificationWithName:object:", nt_with), {0}};
static const nmethod nsnotif_i[] = {NM("name", nt_name), NM("object", nt_object), NM("userInfo", nt_userInfo), {0}};

// ================================================================ red / misc (offline)

static id ret_nil(id self, SEL _c) { return NULL; }
static int ret_no(id self, SEL _c) { return 0; }
static id url_with(id cls, SEL _c, id s) {
    ns_string *u = objc_alloc(objc_getClass("NSURL"));
    str_set(u, ns_cstr(s), strlen(ns_cstr(s)));
    return AR(u);
}
static id url_path(id self, SEL _c) {
    const char *s = ns_cstr(self);
    return ns_str(!strncmp(s, "file://", 7) ? s + 7 : s);
}
static const nmethod nsurl_c[] = {
    NM("URLWithString:", url_with), NM("fileURLWithPath:", url_with), NM("fileURLWithPath:isDirectory:", url_with), {0}};
static const nmethod nsurl_i[] = {
    NM("initWithString:", s_initWithString), NM("path", url_path), NM("absoluteString", s_self_copy),
    NM("isFileURL", o_yes), {0}};
// NSURLConnection: sin red -> nil. NSXMLParser: parse = NO. NSError: dominio/código genéricos.
static const nmethod nsurlconn_c[] = {
    NM("connectionWithRequest:delegate:", ret_nil), NM("sendSynchronousRequest:returningResponse:error:", ret_nil),
    NM("canHandleRequest:", ret_no), {0}};
static const nmethod nsurlconn_i[] = {NM("initWithRequest:delegate:", ret_nil), NM("initWithRequest:delegate:startImmediately:", ret_nil), {0}};
static const nmethod nsxml_i[] = {NM("parse", ret_no), {0}};
static id err_desc(id self, SEL _c) { return ns_str("offline"); }
static const nmethod nserror_i[] = {NM("localizedDescription", err_desc), NM("code", ret_no), NM("domain", err_desc), {0}};
static void ah_fail(id self, SEL _c, SEL m, id obj, id file, int line, id desc, ...) {
    l_error("NSAssert falló: %s (%s:%d) %s", m, ns_cstr(file), line, ns_cstr(desc));
}
// InFunction no lleva object: y el nombre de función es NSString, no SEL.
static void ah_fail_fn(id self, SEL _c, id fn, id file, int line, id desc, ...) {
    l_error("NSCAssert falló: %s (%s:%d) %s", ns_cstr(fn), ns_cstr(file), line, ns_cstr(desc));
}
static id ah_current(id cls, SEL _c) { return cls; }
static const nmethod nsassert_c[] = {
    NM("currentHandler", ah_current), NM("handleFailureInMethod:object:file:lineNumber:description:", ah_fail),
    NM("handleFailureInFunction:file:lineNumber:description:", ah_fail_fn), {0}};
static id pi_info(id cls, SEL _c) { return cls; }
static id pi_name(id self, SEL _c) { return ns_str("game"); }
static double pi_uptime(id self, SEL _c) { return mono(); }
static long long pi_mem(id self, SEL _c) { return 128LL * 1024 * 1024; }
static const nmethod nsproc_c[] = {
    NM("processInfo", pi_info), NM("processName", pi_name), NM("systemUptime", pi_uptime), NM("physicalMemory", pi_mem),
    NM("globallyUniqueString", pi_name), NM("arguments", ret_nil), NM("environment", ret_nil), {0}};
static id cs_set(id cls, SEL _c) { return cls; }
static const nmethod nscharset_c[] = {
    NM("whitespaceCharacterSet", cs_set), NM("whitespaceAndNewlineCharacterSet", cs_set),
    NM("newlineCharacterSet", cs_set), NM("decimalDigitCharacterSet", cs_set), NM("alphanumericCharacterSet", cs_set),
    NM("characterSetWithCharactersInString:", cs_set), NM("punctuationCharacterSet", cs_set), {0}};

// ================================================================ CoreFoundation

static void cf_release(id o) { objc_release(o); }
static id cf_retain(id o) { return objc_retain(o); }
static id cf_arrayCreateMutable(void *a, int cap, void *cb) { return ns_array_new(); }
static void cf_arrayAppend(id a, id v) {
    ns_array *s = a; // CFArray sin callbacks de retain: guardar tal cual
    if (s->count == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->items = realloc(s->items, s->cap * sizeof(id));
    }
    s->items[s->count++] = v;
}
static int cf_arrayCount(id a) { return ns_array_count(a); }
static id cf_arrayGet(id a, int i) { return ns_array_get(a, i); }
static void cf_arrayRemoveAll(ns_array *a) { a->count = 0; }
static id cf_mainBundle(void) { return main_bundle; }
static id cf_bundleURL(id b) { return objc_retain(url_with(NULL, NULL, path_no_slash(BUNDLE_PATH))); }
static id cf_urlPath(id url, int style) { return ns_string_new(ns_cstr(url_path(url, NULL))); }
static id cf_urlFromFS(void *a, const char *buf, int len, int isdir) {
    char b[512];
    snprintf(b, sizeof(b), "%.*s", len, buf);
    return objc_retain(url_with(NULL, NULL, ns_str(b)));
}
static int cf_urlGetFS(id url, int resolve, char *buf, int max) {
    snprintf(buf, max, "%s", ns_cstr(url_path(url, NULL)));
    return 1;
}
static id cf_bundleCopyResourceURL(id b, id name, id type, id dir) {
    id p = b_pathForResource(main_bundle, NULL, name, type, dir);
    return p ? objc_retain(url_with(NULL, NULL, p)) : NULL;
}
static int cf_stringGetCString(id s, char *buf, int size, int enc) {
    return s_getCString(s, NULL, buf, size, enc);
}
static const char *cf_stringGetCStringPtr(id s, int enc) { return ns_cstr(s); }
static id cf_stringCreateWithCString(void *a, const char *s, int enc) {
    return str_init_bytes(objc_alloc(c_NSString), s, strlen(s), enc == 0x08000100 ? ENC_UTF8 : enc == 0x0201 ? ENC_LATIN1 : ENC_UTF8);
}
static int cf_stringGetLength(id s) { return s_length(s, NULL); }
static double cf_absTime(void) { return ns_now(); }
static id cf_localeCopyCurrent(void) { return objc_retain(cur_locale); }
static id cf_localeGetValue(id loc, id key) { return lc_objectForKey(loc, NULL, key); }
static id cf_runLoopGetCurrent(void) { return run_loop; }
// CFRunLoopRunInMode(mode, double seconds, Boolean): Darwin pasa el double en r1:r2 (sin alinear).
static int cf_runLoopRunInMode(id mode, uint32_t lo, uint32_t hi, int ret_after) {
    union {
        uint64_t u;
        double d;
    } v = {(uint64_t)hi << 32 | lo};
    double secs = v.d > 0 ? v.d : 0;
    if (!is_main_thread()) { // hilo de juego de Gameloft: MoviePlayer playMovie espera su NSTimer acá
        bg_runloop(secs);
        return 3;
    }
    double end = mono() + secs;
    do
        ui_runloop_tick(0);
    while (mono() < end);
    return 3; // kCFRunLoopRunTimedOut
}
static void cf_noop(void) {}
static id cf_null(void) { return NULL; }
static int cf_zero(void) { return 0; }
static id cf_preferencesCopy(id key, id app) { return objc_retain(ns_dict_get(defaults_dict, key)); }
static void cf_preferencesSet(id key, id v, id app) { ud_setObject(NULL, NULL, v, key); }
static int cf_preferencesSync(id app) { return ud_synchronize(NULL, NULL); }
static int cf_equal(id a, id b) { return obj_equal(a, b); }
static id cf_copyDescription(id o) { return objc_retain(ns_str(objc_describe(o))); }
static int cf_numberGetValue(ns_number *n, int type, void *out) {
    if (type == 13 || type == 6) // Float64 / Double
        *(double *)out = num_d(n);
    else if (type == 5 || type == 12) // Float32 / Float
        *(float *)out = num_d(n);
    else if (type == 4 || type == 11) // SInt64 / LongLong
        *(int64_t *)out = num_i(n);
    else
        *(int32_t *)out = (int32_t)num_i(n);
    return 1;
}
static id cf_dictGetValue(id d, id k) { return ns_dict_get(d, k); }
static void cf_dictSetValue(id d, id k, id v) { ns_dict_set(d, k, v); }
static id cf_dictCreateMutable(void *a, int cap, void *kcb, void *vcb) { return ns_dict_new(); }

static void cf_dictAddValue(id d, id k, id v) {
    if (!ns_dict_get(d, k))
        ns_dict_set(d, k, v);
}
static id cf_setCreateMutable(void *a, int cap, void *cb) { return objc_alloc(c_NSMutableSet); }
static void cf_setAdd(ns_array *s, id v) {
    for (int i = 0; i < s->count; i++)
        if (obj_equal(s->items[i], v))
            return;
    ns_array_add(s, v);
}
static id cf_stringCreateMutable(void *a, int max) {
    ns_string *s = objc_alloc(c_NSMutableString);
    str_set(s, "", 0);
    return s;
}
static void cf_stringAppend(ns_string *s, id o) { ms_append(s, ns_cstr(o)); }
static void cf_stringAppendCString(ns_string *s, const char *c, int enc) { ms_append(s, c); }
static id cf_copyString(void *a, id s) { return ns_string_new(ns_cstr(s)); }
static id cf_uuidCreate(void *a) { return ns_string_new("00000000-0000-0000-0000-000000000001"); }
static id cf_uuidCreateString(void *a, id u) { return ns_string_new(ns_cstr(u)); }
static void cf_uuidGetBytes(uint8_t *ret, id u) { memset(ret, 0x11, 16); }
static id cf_runLoopCopyAllModes(id rl) {
    id a = ns_array_new();
    ns_array_add(a, ns_str("kCFRunLoopDefaultMode"));
    return a;
}
// ---- CF de Zenonia 5 (GVJK/JSONKit, FontManager de FontLabel, CocosDenshion, CoreText)
typedef struct {
    int loc, len;
} cf_range;
static int cf_enc(int e) { // CFStringEncoding -> ENC_
    return e == 0x0100 || e == 0x14000100 ? ENC_UTF16 : e == 0 || e == 0x0201 || e == 0x0500 ? ENC_LATIN1 : ENC_UTF8;
}
static id cf_arrayCreate(void *a, id *vals, int n, void *cb) {
    id r = ns_array_new();
    for (int i = 0; i < n; i++)
        ns_array_add(r, vals[i]);
    return r;
}
static void cf_arrayGetValues(ns_array *a, cf_range r, id *out) {
    for (int i = 0; i < r.len && r.loc + i < a->count; i++)
        out[i] = a->items[r.loc + i];
}
id ns_data_new(const void *b, int n) { return data_new(c_NSMutableData, b, n, 1); }
static id cf_dataCreate(void *a, const void *b, int n) { return data_new(c_NSData, b, n, 1); }
static id cf_dataCreateMutable(void *a, int cap) { return data_new(c_NSMutableData, NULL, 0, 1); }
static id cf_dataCreateNoCopy(void *a, const void *b, int n, void *dealloc) { return data_new(c_NSData, b, n, 1); }
static void *cf_dataBytes(ns_data *d) { return d ? d->bytes : NULL; }
static void cf_dataSetLength(ns_data *d, int n) {
    if (n > d->cap) {
        d->bytes = d->owned ? realloc(d->bytes, n + 1) : memcpy(malloc(n + 1), d->bytes, d->len);
        memset(d->bytes + d->len, 0, n + 1 - d->len);
        d->cap = n, d->owned = 1;
    }
    d->len = n;
}
static int cf_dictCount(ns_dict *d) { return d ? d->count : 0; }
static void cf_dictGetKeysAndValues(ns_dict *d, id *k, id *v) {
    for (int i = 0; i < d->count; i++) {
        if (k)
            k[i] = d->keys[i];
        if (v)
            v[i] = d->vals[i];
    }
}
static int cf_dictGetValueIfPresent(ns_dict *d, id k, id *v) {
    int i = dict_find(d, k);
    if (i >= 0 && v)
        *v = d->vals[i];
    return i >= 0;
}
static void cf_dictRemoveValue(ns_dict *d, id k) { d_removeObjectForKey(d, NULL, k); }
static uint32_t cf_hash(id o) { return (uint32_t)objc_msg(o, sel_registerName("hash"), 0, 0, 0, 0); }
static id cf_makeCollectable(id o) { return o; }
static id cf_numberCreate(void *a, int type, const void *v) {
    switch (type) { // CFNumberType
    case 1: case 7: return num_new(0, *(const int8_t *)v, 0);
    case 2: case 8: return num_new(0, *(const int16_t *)v, 0);
    case 4: case 11: return num_new(0, *(const int64_t *)v, 0);
    case 5: case 12: return num_new(1, 0, *(const float *)v);
    case 6: case 13: return num_new(1, 0, *(const double *)v);
    default: return num_new(0, *(const int32_t *)v, 0);
    }
}
static int cf_stringCompare(id a, id b, int flags) { // flags & 1 = kCFCompareCaseInsensitive
    int r = (flags & 1) ? strcasecmp(ns_cstr(a), ns_cstr(b)) : strcmp(ns_cstr(a), ns_cstr(b));
    return r < 0 ? -1 : r > 0;
}
static id cf_stringCreateWithBytes(void *a, const char *b, int n, int enc, int ext) {
    return str_init_bytes(objc_alloc(c_NSString), b, n, cf_enc(enc));
}
static id cf_stringCreateWithBytesNoCopy(void *a, const char *b, int n, int enc, int ext, void *dealloc) {
    return cf_stringCreateWithBytes(a, b, n, enc, ext);
}
static id cf_stringCreateWithCharactersNoCopy(void *a, const uint16_t *u, int n, void *dealloc) {
    return str_init_bytes(objc_alloc(c_NSString), (const char *)u, n * 2, ENC_UTF16);
}
// CFStringGetBytes(str, range, enc, lossByte, isExtRep, buf, maxLen, *usedLen): solo UTF-8/ASCII.
// ponytail: el rango se toma en bytes UTF-8 (igual a UTF-16 para texto ASCII, que es lo que serializa GVJK).
static int cf_stringGetBytes(id s, cf_range r, int enc, int loss, int ext, uint8_t *buf, int max, int *used) {
    const char *c = ns_cstr(s);
    int n = strlen(c) - r.loc;
    if (n > r.len)
        n = r.len;
    if (n < 0)
        n = 0;
    if (buf && n > max)
        n = max;
    if (buf)
        memcpy(buf, c + r.loc, n);
    if (used)
        *used = n;
    return n;
}
static int cf_stringMaxSize(int len, int enc) { return len * 3; }
static id cf_urlCopyPathExtension(id url) {
    const char *p = ns_cstr(url_path(url, NULL)), *dot = strrchr(p, '.');
    return dot && !strchr(dot, '/') ? ns_string_new(dot + 1) : NULL;
}
static void cf_streamPairToHost(void *a, id host, int port, id *rd, id *wr) { // sin red
    l_info("CFStreamCreatePairWithSocketToHost(%s:%d): red no disponible", ns_cstr(host), port);
    if (rd)
        *rd = NULL;
    if (wr)
        *wr = NULL;
}
static void ns_unionRange(cf_range *ret, cf_range a, cf_range b) {
    int lo = a.loc < b.loc ? a.loc : b.loc, hi = a.loc + a.len > b.loc + b.len ? a.loc + a.len : b.loc + b.len;
    *ret = (cf_range){lo, hi - lo};
}
static double k_kCFCoreFoundationVersionNumber = 793.00; // iOS 6.1
static void *k_kCFAllocatorSystemDefault;
static id k_kCFNumberNaN, k_kCFNumberNegativeInfinity, k_kCFNumberPositiveInfinity;

static id k_kCFRunLoopDefaultMode, k_kCFRunLoopCommonModes, k_NSDefaultRunLoopMode, k_NSRunLoopCommonModes,
    k_kCFLocaleCountryCode, k_NSLocaleCountryCode, k_NSLocaleLanguageCode, k_NSErrorFailingURLStringKey, k_kCFBooleanTrue,
    k_kCFBooleanFalse;
static void *k_kCFAllocatorDefault;

const import_entry foundation_imports[] = {
    F(NSLog, f_NSLog), F(NSSearchPathForDirectoriesInDomains, f_NSSearchPath), F(NSHomeDirectory, f_NSHomeDirectory),
    F(NSTemporaryDirectory, f_NSTemporaryDirectory), F(NSClassFromString, f_NSClassFromString),
    F(NSStringFromClass, f_NSStringFromClass), F(NSSelectorFromString, f_NSSelectorFromString),
    F(NSStringFromSelector, f_NSStringFromSelector),
    V(kCFRunLoopDefaultMode, &k_kCFRunLoopDefaultMode), V(kCFRunLoopCommonModes, &k_kCFRunLoopCommonModes),
    V(NSDefaultRunLoopMode, &k_NSDefaultRunLoopMode), V(NSRunLoopCommonModes, &k_NSRunLoopCommonModes),
    V(kCFLocaleCountryCode, &k_kCFLocaleCountryCode), V(NSLocaleCountryCode, &k_NSLocaleCountryCode),
    V(NSLocaleLanguageCode, &k_NSLocaleLanguageCode), V(NSErrorFailingURLStringKey, &k_NSErrorFailingURLStringKey),
    V(kCFBooleanTrue, &k_kCFBooleanTrue), V(kCFBooleanFalse, &k_kCFBooleanFalse),
    V(kCFAllocatorDefault, &k_kCFAllocatorDefault), V(kCFStreamPropertyShouldCloseNativeSocket, &k_NSErrorFailingURLStringKey),
    F(CFRelease, cf_release), F(CFRetain, cf_retain), F(CFArrayCreateMutable, cf_arrayCreateMutable),
    F(CFArrayAppendValue, cf_arrayAppend), F(CFArrayGetCount, cf_arrayCount), F(CFArrayGetValueAtIndex, cf_arrayGet),
    F(CFArrayRemoveAllValues, cf_arrayRemoveAll), F(CFBundleGetMainBundle, cf_mainBundle),
    F(CFBundleCopyBundleURL, cf_bundleURL), F(CFBundleCopyResourcesDirectoryURL, cf_bundleURL),
    F(CFBundleCopyResourceURL, cf_bundleCopyResourceURL), F(CFURLCopyFileSystemPath, cf_urlPath),
    F(CFURLCreateFromFileSystemRepresentation, cf_urlFromFS), F(CFURLGetFileSystemRepresentation, cf_urlGetFS),
    F(CFStringGetCString, cf_stringGetCString), F(CFStringGetCStringPtr, cf_stringGetCStringPtr),
    F(CFStringCreateWithCString, cf_stringCreateWithCString), F(CFStringGetLength, cf_stringGetLength),
    F(CFAbsoluteTimeGetCurrent, cf_absTime), F(CFLocaleCopyCurrent, cf_localeCopyCurrent),
    F(CFLocaleGetSystem, cf_localeCopyCurrent), F(CFLocaleGetValue, cf_localeGetValue),
    F(CFRunLoopGetCurrent, cf_runLoopGetCurrent), F(CFRunLoopGetMain, cf_runLoopGetCurrent),
    F(CFRunLoopRunInMode, cf_runLoopRunInMode), F(CFRunLoopAddSource, cf_noop), F(CFRunLoopRemoveSource, cf_noop),
    F(CFReadStreamSetProperty, cf_zero), F(CFWriteStreamSetProperty, cf_zero), F(CFSocketCreate, cf_null),
    F(CFSocketCopyAddress, cf_null), F(CFSocketCreateRunLoopSource, cf_null), F(CFSocketGetNative, cf_zero),
    F(CFSocketInvalidate, cf_noop), F(CFSocketSetAddress, cf_zero), F(CFStreamCreatePairWithSocket, cf_noop),
    F(CFPreferencesCopyAppValue, cf_preferencesCopy), F(CFPreferencesSetAppValue, cf_preferencesSet),
    F(CFPreferencesAppSynchronize, cf_preferencesSync), F(CFEqual, cf_equal), F(CFCopyDescription, cf_copyDescription),
    F(CFNumberGetValue, cf_numberGetValue), F(CFDictionaryGetValue, cf_dictGetValue),
    F(CFDictionarySetValue, cf_dictSetValue), F(CFDictionaryCreateMutable, cf_dictCreateMutable),
    F(CFDictionaryAddValue, cf_dictAddValue), F(CFSetCreateMutable, cf_setCreateMutable), F(CFSetAddValue, cf_setAdd),
    F(CFStringCreateMutable, cf_stringCreateMutable), F(CFStringAppend, cf_stringAppend),
    F(CFStringAppendCString, cf_stringAppendCString), F(CFURLCreateStringByAddingPercentEscapes, cf_copyString),
    F(CFURLCreateStringByReplacingPercentEscapesUsingEncoding, cf_copyString), F(CFUUIDCreate, cf_uuidCreate),
    F(CFUUIDCreateFromUUIDBytes, cf_uuidCreate), F(CFUUIDCreateString, cf_uuidCreateString),
    F(CFUUIDGetUUIDBytes, cf_uuidGetBytes), F(CFRunLoopCopyAllModes, cf_runLoopCopyAllModes),
    V(kCFAllocatorSystemDefault, &k_kCFAllocatorSystemDefault), V(kCFNumberNaN, &k_kCFNumberNaN),
    V(kCFNumberNegativeInfinity, &k_kCFNumberNegativeInfinity), V(kCFNumberPositiveInfinity, &k_kCFNumberPositiveInfinity),
    // SystemConfiguration: siempre sin red
    F(SCNetworkReachabilityCreateWithAddress, cf_null), F(SCNetworkReachabilityCreateWithName, cf_null),
    F(SCNetworkReachabilityGetFlags, cf_zero), F(SCNetworkReachabilityScheduleWithRunLoop, cf_zero),
    F(SCNetworkReachabilitySetCallback, cf_zero), F(SCNetworkReachabilityUnscheduleFromRunLoop, cf_zero),
    F(CFArrayCreate, cf_arrayCreate), F(CFArrayGetValues, cf_arrayGetValues), F(CFDataCreate, cf_dataCreate),
    F(CFDataCreateMutable, cf_dataCreateMutable), F(CFDataCreateWithBytesNoCopy, cf_dataCreateNoCopy),
    F(CFDataGetBytePtr, cf_dataBytes), F(CFDataGetMutableBytePtr, cf_dataBytes), F(CFDataSetLength, cf_dataSetLength),
    F(CFDictionaryGetCount, cf_dictCount), F(CFDictionaryGetKeysAndValues, cf_dictGetKeysAndValues),
    F(CFDictionaryGetValueIfPresent, cf_dictGetValueIfPresent), F(CFDictionaryRemoveValue, cf_dictRemoveValue),
    F(CFHash, cf_hash), F(CFMakeCollectable, cf_makeCollectable), F(CFNumberCreate, cf_numberCreate),
    F(CFStringCompare, cf_stringCompare), F(CFStringCreateWithBytes, cf_stringCreateWithBytes),
    F(CFStringCreateWithBytesNoCopy, cf_stringCreateWithBytesNoCopy),
    F(CFStringCreateWithCharactersNoCopy, cf_stringCreateWithCharactersNoCopy), F(CFStringGetBytes, cf_stringGetBytes),
    F(CFStringGetMaximumSizeForEncoding, cf_stringMaxSize), F(CFURLCopyPathExtension, cf_urlCopyPathExtension),
    F(CFStreamCreatePairWithSocketToHost, cf_streamPairToHost), F(NSUnionRange, ns_unionRange),
    V(kCFCoreFoundationVersionNumber, &k_kCFCoreFoundationVersionNumber),
};
TABLE_END(foundation)

// Constantes NSString de los frameworks que nadie implementó (NSFooKey, UIBarNotification, kCFBaz...):
// una NSString con el nombre del símbolo, como hacen los frameworks de verdad (valores únicos).
const import_entry *const_string_import(const char *name) {
    static const char *pre[] = {"_NS", "_UI", "_kCF", "_kCA", "_kSec", "_GC", "_GK", "_MP", "_AV", "_kAudio", "_kCG", "_CF"};
    static const char *suf[] = {"Key", "Notification", "Domain", "Name", "Type", "Mode", "Scope", "Exception",
                                "Symbol", "Date", "Size", "Permissions", "Calendar", "Actions", "EaseInEaseOut",
                                "Port", "Proxy", "Identifier", "Version", "Accessible", "Account", "Generic", "Service",
                                "Class", "GenericPassword", "Limit", "LimitOne", "Data", "Unlocked", "Item",
                                "Ambient", "Playback", "PlayAndRecord", "Code", "Fade", "Label", "Attributes"};
    int ok = 0;
    for (unsigned i = 0; i < sizeof(pre) / sizeof(pre[0]) && !ok; i++)
        ok = !strncmp(name, pre[i], strlen(pre[i]));
    // los verbos de CF/UIKit delatan funciones (CFBundleGetIdentifier...), no constantes
    if (!ok || strstr(name, "Get") || strstr(name, "Create") || strstr(name, "Copy") || strstr(name, "Set"))
        return NULL;
    size_t l = strlen(name);
    for (unsigned i = 0; i < sizeof(suf) / sizeof(suf[0]); i++) {
        size_t k = strlen(suf[i]);
        if (l > k && !strcmp(name + l - k, suf[i])) {
            static import_entry e;
            id *slot = malloc(sizeof(id));
            *slot = ns_string_new(name + 1);
            e = (import_entry){name, (uintptr_t)slot, 1};
            return &e;
        }
    }
    return NULL;
}

// ================================================================ init

extern objc_class *c_NSObject;
void foundation_init(void) {
    main_thread_id = sceKernelGetThreadId();
    c_NSObject = objc_define("NSObject", NULL, 4, nsobject_i, nsobject_c);
    c_NSString = objc_define("NSString", "NSObject", sizeof(ns_string), nsstring_i, nsstring_c);
    c_NSMutableString = objc_define("NSMutableString", "NSString", sizeof(ns_string), nsmstring_i, nsmstring_c);
    c_NSConstantString = objc_define("NSConstantString", "NSString", sizeof(ns_string), NULL, NULL);
    objc_define("__NSCFConstantString", "NSString", sizeof(ns_string), NULL, NULL);
    c_NSNumber = objc_define("NSNumber", "NSObject", sizeof(ns_number), nsnumber_i, nsnumber_c);
    objc_define("NSValue", "NSObject", sizeof(ns_number), NULL, NULL);
    c_NSArray = objc_define("NSArray", "NSObject", sizeof(ns_array), nsarray_i, nsarray_c);
    c_NSMutableArray = objc_define("NSMutableArray", "NSArray", sizeof(ns_array), NULL, NULL);
    c_NSSet = objc_define("NSSet", "NSArray", sizeof(ns_array), NULL, NULL);
    c_NSMutableSet = objc_define("NSMutableSet", "NSSet", sizeof(ns_array), NULL, NULL);
    c_NSEnumerator = objc_define("NSEnumerator", "NSObject", sizeof(ns_enum), nsenum_i, NULL);
    c_NSDictionary = objc_define("NSDictionary", "NSObject", sizeof(ns_dict), nsdict_i, nsdict_c);
    c_NSMutableDictionary = objc_define("NSMutableDictionary", "NSDictionary", sizeof(ns_dict), NULL, NULL);
    c_NSData = objc_define("NSData", "NSObject", sizeof(ns_data), nsdata_i, nsdata_c);
    c_NSMutableData = objc_define("NSMutableData", "NSData", sizeof(ns_data), NULL, NULL);
    c_NSBundle = objc_define("NSBundle", "NSObject", 8, nsbundle_i, nsbundle_c);
    c_NSUserDefaults = objc_define("NSUserDefaults", "NSObject", 8, nsud_i, nsud_c);
    c_NSFileManager = objc_define("NSFileManager", "NSObject", 8, nsfm_i, nsfm_c);
    c_NSLocale = objc_define("NSLocale", "NSObject", 8, nslocale_i, nslocale_c);
    c_NSDate = objc_define("NSDate", "NSObject", sizeof(ns_date), nsdate_i, nsdate_c);
    c_NSTimer = objc_define("NSTimer", "NSObject", sizeof(ns_timer), nstimer_i, nstimer_c);
    c_NSRunLoop = objc_define("NSRunLoop", "NSObject", 8, nsrl_i, nsrl_c);
    c_NSThread = objc_define("NSThread", "NSObject", sizeof(ns_thread), nsthread_i, nsthread_c);
    objc_define("NSLock", "NSObject", sizeof(ns_lock), nslock_i, NULL);
    objc_define("NSRecursiveLock", "NSLock", sizeof(ns_lock), NULL, NULL);
    objc_define("NSCondition", "NSLock", sizeof(ns_lock), NULL, NULL);
    objc_define("NSConditionLock", "NSLock", sizeof(ns_lock), NULL, NULL);
    objc_define("NSAutoreleasePool", "NSObject", 8, nspool_i, nspool_c);
    c_NSNotificationCenter = objc_define("NSNotificationCenter", "NSObject", 8, nsnc_i, nsnc_c);
    c_NSNotification = objc_define("NSNotification", "NSObject", sizeof(ns_notif), nsnotif_i, nsnotif_c);
    c_NSURL = objc_define("NSURL", "NSString", sizeof(ns_string), nsurl_i, nsurl_c);
    objc_define("NSURLRequest", "NSObject", 8, NULL, NULL);
    objc_define("NSMutableURLRequest", "NSURLRequest", 8, NULL, NULL);
    objc_define("NSURLConnection", "NSObject", 8, nsurlconn_i, nsurlconn_c);
    objc_define("NSXMLParser", "NSObject", 8, nsxml_i, NULL);
    c_NSError = objc_define("NSError", "NSObject", 8, nserror_i, NULL);
    objc_define("NSAssertionHandler", "NSObject", 8, NULL, nsassert_c);
    objc_define("NSProcessInfo", "NSObject", 8, NULL, nsproc_c);
    objc_define("NSCharacterSet", "NSObject", 8, NULL, nscharset_c);

    detect_language();
    main_bundle = objc_alloc(c_NSBundle);
    user_defaults = objc_alloc(c_NSUserDefaults);
    file_manager = objc_alloc(c_NSFileManager);
    cur_locale = objc_alloc(c_NSLocale);
    run_loop = objc_alloc(c_NSRunLoop);
    notif_center = objc_alloc(c_NSNotificationCenter);
    info_dict = plist_load(BUNDLE_PATH "Info.plist");
    if (!info_dict)
        info_dict = ns_dict_new();
    defaults_dict = plist_load_text(DEFAULTS_PATH);
    if (!defaults_dict || !objc_is_kind(defaults_dict, c_NSDictionary))
        defaults_dict = ns_dict_new();
    id langs = ns_array_new();
    ns_array_add(langs, ns_str(lang_code));
    ns_dict_set(defaults_dict, ns_str("AppleLanguages"), langs); // Gameloft elige idioma con esto
    k_kCFRunLoopDefaultMode = k_NSDefaultRunLoopMode = ns_string_new("kCFRunLoopDefaultMode");
    k_kCFRunLoopCommonModes = k_NSRunLoopCommonModes = ns_string_new("kCFRunLoopCommonModes");
    k_kCFLocaleCountryCode = k_NSLocaleCountryCode = ns_string_new("kCFLocaleCountryCodeKey");
    k_NSLocaleLanguageCode = ns_string_new("kCFLocaleLanguageCodeKey");
    k_NSErrorFailingURLStringKey = ns_string_new("NSErrorFailingURLStringKey");
    k_kCFBooleanTrue = num_new(2, 1, 0);
    k_kCFNumberNaN = num_new(1, 0, NAN);
    k_kCFNumberNegativeInfinity = num_new(1, 0, -INFINITY);
    k_kCFNumberPositiveInfinity = num_new(1, 0, INFINITY);
    k_kCFBooleanFalse = num_new(2, 0, 0);
    mkdir_p(DOCS_PATH);
    l_info("Foundation lista: Info.plist %s, %d claves en NSUserDefaults",
           ns_dict_get_c(info_dict, "CFBundleExecutable") ? "ok" : "NO", ((ns_dict *)defaults_dict)->count);
}

// __cfstring del binario: los UTF-16 (__ustring) se pasan a UTF-8 en su lugar (la sección es RW).
void foundation_fix_cfstrings(void) {
    uint32_t a, s;
    if (!macho_section(NULL, "__cfstring", &a, &s))
        return;
    int n16 = 0;
    for (uint32_t i = 0; i < s / 16; i++) {
        ns_string *cs = (ns_string *)(a + i * 16);
        if (cs->flags & 0x10 || (cs->flags & 0xFF) == 0xD0) { // kCFStringEncodingUnicode
            char *u = utf16_to_utf8((const uint16_t *)cs->s, cs->len);
            cs->s = u;
            cs->len = strlen(u);
            cs->flags = 0x7C8;
            n16++;
        }
    }
    l_info("cfstrings: %u (%d UTF-16 convertidos)", (unsigned)(s / 16), n16);
}
