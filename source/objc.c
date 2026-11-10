// Runtime Objective-C 2 (ABI iOS ARM32): clases del juego + clases nativas, selectores internados,
// caché de métodos, refcount con cabecera oculta y pools de autorelease. Ver objc.h.
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imports.h"
#include "log.h"
#include "macho.h"
#include "objc.h"

extern uint32_t objc_call_imp(void *imp, id self, SEL sel, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
extern id objc_msgSend_stret(void *ret, id self, SEL sel, ...);
extern id objc_msgSendSuper2(void *sup, SEL sel, ...);
extern id objc_msgSendSuper2_stret(void *ret, void *sup, SEL sel, ...);

static pthread_mutex_t rt_lock = PTHREAD_MUTEX_INITIALIZER;

// ---------------------------------------------------------------- selectores

#define SEL_BUCKETS 8192
typedef struct sel_node {
    struct sel_node *next;
    const char *s;
} sel_node;
static sel_node *sel_tab[SEL_BUCKETS];

static uint32_t hash_str(const char *s) {
    uint32_t h = 2166136261u;
    while (*s)
        h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

// El primer puntero registrado para una cadena es el canónico (para el juego, su propia cadena de
// __objc_methname; así sus selrefs ya quedan bien).
static SEL sel_intern(const char *s, int copy) {
    if (!s)
        return NULL;
    uint32_t h = hash_str(s) & (SEL_BUCKETS - 1);
    for (sel_node *n = sel_tab[h]; n; n = n->next)
        if (!strcmp(n->s, s))
            return n->s;
    pthread_mutex_lock(&rt_lock);
    for (sel_node *n = sel_tab[h]; n; n = n->next) // otro hilo pudo agregarlo
        if (!strcmp(n->s, s)) {
            pthread_mutex_unlock(&rt_lock);
            return n->s;
        }
    sel_node *n = malloc(sizeof(*n));
    n->s = copy ? strdup(s) : s;
    n->next = sel_tab[h];
    __sync_synchronize();
    sel_tab[h] = n;
    pthread_mutex_unlock(&rt_lock);
    return n->s;
}

SEL sel_registerName(const char *s) { return sel_intern(s, 1); }

// ---------------------------------------------------------------- clases

typedef struct {
    objc_class cls, meta;
    class_ro ro, mro;
    nmethod *im, *cm; // copias con selectores internados
} native_class;

#define MAX_CLASSES 1024
static objc_class *classes[MAX_CLASSES];
static int nclasses;

// Listas de métodos extra (categorías) por clase.
typedef struct ext_list {
    struct ext_list *next;
    objc_class *cls;
    method_list *ml;
} ext_list;
static ext_list *ext_lists;

static class_ro *ro_of(objc_class *c) { return (class_ro *)((uintptr_t)c->ro & ~3u); }
static int is_native(objc_class *c) { return ro_of(c)->flags & RO_NATIVE; }
static int is_meta(objc_class *c) { return ro_of(c)->flags & RO_META; }

const char *class_getName(objc_class *c) { return c ? ro_of(c)->name : "nil"; }

objc_class *objc_getClass(const char *name) {
    for (int i = 0; i < nclasses; i++)
        if (!strcmp(class_getName(classes[i]), name))
            return classes[i];
    return NULL;
}

static void add_class(objc_class *c) {
    if (nclasses < MAX_CLASSES)
        classes[nclasses++] = c;
    else
        l_error("objc: demasiadas clases");
}

static nmethod *intern_table(const nmethod *t) {
    int n = 0;
    while (t && t[n].sel)
        n++;
    nmethod *c = calloc(n + 1, sizeof(nmethod));
    for (int i = 0; i < n; i++)
        c[i] = (nmethod){sel_registerName(t[i].sel), t[i].imp};
    return c;
}

objc_class *objc_define(const char *name, const char *super, size_t instance_size, const nmethod *im,
                        const nmethod *cm) {
    objc_class *sup = super ? objc_getClass(super) : NULL;
    if (super && !sup)
        fatal_error("objc_define(%s): falta la superclase %s", name, super);
    native_class *n = calloc(1, sizeof(*n));
    n->ro = (class_ro){RO_NATIVE, 4, instance_size, 0, name, NULL, NULL, NULL, 0, NULL};
    n->mro = (class_ro){RO_NATIVE | RO_META, 20, 20, 0, name, NULL, NULL, NULL, 0, NULL};
    n->im = intern_table(im);
    n->cm = intern_table(cm);
    n->cls = (objc_class){&n->meta, sup, NULL, n->im, &n->ro};
    // Metaclase: su superclase es la metaclase de la superclase; la de la raíz es la raíz misma
    // (así [Clase respondsToSelector:] y demás métodos de instancia de NSObject andan en clases).
    n->meta = (objc_class){NULL, sup ? sup->isa : &n->cls, NULL, n->cm, &n->mro};
    objc_class *root = &n->cls;
    while (root->superclass)
        root = root->superclass;
    n->meta.isa = root->isa ? root->isa : &n->meta;
    if (sup && instance_size < ro_of(sup)->instanceSize)
        n->ro.instanceSize = ro_of(sup)->instanceSize;
    add_class(&n->cls);
    return &n->cls;
}

objc_class *object_getClass(id o) { return o ? *(objc_class **)o : NULL; }

int objc_is_kind(id o, objc_class *c) {
    for (objc_class *k = object_getClass(o); k; k = k->superclass)
        if (k == c)
            return 1;
    return 0;
}

// Clase nativa genérica para un _OBJC_CLASS_$_X que nadie implementó (UITableView, GKSession...):
// mensajes desconocidos devuelven nil (o self si empiezan por "init") y se registran en el log.
static objc_class *auto_class(const char *name) {
    objc_class *c = objc_getClass(name);
    if (c)
        return c;
    const char *super = "NSObject";
    size_t l = strlen(name);
    if (!strncmp(name, "UI", 2)) {
        if (l > 10 && !strcmp(name + l - 10, "Controller"))
            super = objc_getClass("UIViewController") ? "UIViewController" : "NSObject";
        else if (strstr(name, "View") || strstr(name, "Button") || strstr(name, "Label") || strstr(name, "Field") ||
                 strstr(name, "Control") || strstr(name, "Bar"))
            super = objc_getClass("UIView") ? "UIView" : "NSObject";
    }
    l_info("objc: clase %s sin implementar -> subclase vacía de %s", name, super);
    return objc_define(strdup(name), super, 0, NULL, NULL);
}

// imports.c: _OBJC_CLASS_$_X / _OBJC_METACLASS_$_X
const import_entry *objc_class_import(const char *name) {
    static import_entry e;
    // isa de los CFString constantes (__cfstring): la dirección del símbolo es la clase
    if (!strcmp(name, "___CFConstantStringClassReference")) {
        e = (import_entry){name, (uintptr_t)objc_getClass("NSConstantString"), 1};
        return &e;
    }
    if (!strncmp(name, "_OBJC_CLASS_$_", 14)) {
        e = (import_entry){name, (uintptr_t)auto_class(name + 14), 1};
        return &e;
    }
    if (!strncmp(name, "_OBJC_METACLASS_$_", 18)) {
        e = (import_entry){name, (uintptr_t)auto_class(name + 18)->isa, 1};
        return &e;
    }
    return NULL;
}

// ---------------------------------------------------------------- imagen del juego

static void intern_mlist(method_list *ml) {
    if (!ml)
        return;
    uint32_t ent = ml->entsize & ~3u;
    for (uint32_t i = 0; i < ml->count; i++) {
        SEL *name = (SEL *)((uint8_t *)ml->m + i * ent);
        SEL c = sel_intern(*name, 0);
        if (c != *name)
            macho_write_word((uint32_t *)name, (uint32_t)c); // la lista puede estar en __TEXT,__const
    }
}

static void register_game_class(objc_class *c) {
    class_ro *ro = ro_of(c), *mro = ro_of(c->isa);
    intern_mlist(ro->baseMethods);
    intern_mlist(mro->baseMethods);
    // La superclase puede ser nativa con otro tamaño que el SDK de Apple: si nuestro objeto nativo
    // es más grande que el instanceStart compilado, los ivars se pisarían. Nuestro estado nativo
    // vive en la cabecera (objc_ext), así que alcanza con avisar.
    if (ro->ivars && c->superclass && is_native(c->superclass) && ro_of(c->superclass)->instanceSize > ro->instanceStart)
        l_error("objc: %s empieza sus ivars en %u y %s mide %u", ro->name, (unsigned)ro->instanceStart,
                class_getName(c->superclass), (unsigned)ro_of(c->superclass)->instanceSize);
    add_class(c);
}

void objc_init_image(void) {
    uint32_t a, s;
    int ncls = 0, ncat = 0, nsel = 0;
    // Clases del juego primero (sus cadenas de selector quedan como canónicas).
    if (macho_section(NULL, "__objc_classlist", &a, &s))
        for (uint32_t i = 0; i < s / 4; i++, ncls++)
            register_game_class(((objc_class **)a)[i]);
    if (macho_section(NULL, "__objc_selrefs", &a, &s))
        for (uint32_t i = 0; i < s / 4; i++, nsel++)
            macho_write_word((uint32_t *)a + i, (uint32_t)sel_intern(((SEL *)a)[i], 0));
    // Categorías: {name, cls, instanceMethods, classMethods, protocols, properties}
    if (macho_section(NULL, "__objc_catlist", &a, &s))
        for (uint32_t i = 0; i < s / 4; i++, ncat++) {
            uint32_t *cat = ((uint32_t **)a)[i];
            objc_class *cls = (objc_class *)cat[1];
            for (int k = 0; k < 2; k++) {
                method_list *ml = (method_list *)cat[2 + k];
                if (!ml || !cls)
                    continue;
                intern_mlist(ml);
                ext_list *e = malloc(sizeof(*e));
                *e = (ext_list){ext_lists, k ? cls->isa : cls, ml};
                ext_lists = e;
            }
            l_debug("objc: categoría %s en %s", (const char *)cat[0], class_getName(cls));
        }
    l_info("objc: %d clases del juego, %d categorías, %d selrefs, %d clases en total", ncls, ncat, nsel,
           nclasses);
}

// ---------------------------------------------------------------- búsqueda de métodos

#define CACHE_SIZE 16384
static struct {
    objc_class *cls;
    SEL sel;
    void *imp;
} cache[CACHE_SIZE];

static void *find_in(objc_class *c, SEL sel) {
    for (ext_list *e = ext_lists; e; e = e->next)
        if (e->cls == c) {
            uint32_t ent = e->ml->entsize & ~3u;
            for (uint32_t i = 0; i < e->ml->count; i++) {
                SEL *m = (SEL *)((uint8_t *)e->ml->m + i * ent);
                if (*m == sel)
                    return (void *)m[2];
            }
        }
    if (is_native(c)) {
        for (const nmethod *m = c->vtable; m && m->sel; m++)
            if (m->sel == sel)
                return m->imp;
        return NULL;
    }
    method_list *ml = ro_of(c)->baseMethods;
    if (ml) {
        uint32_t ent = ml->entsize & ~3u;
        for (uint32_t i = 0; i < ml->count; i++) {
            SEL *m = (SEL *)((uint8_t *)ml->m + i * ent);
            if (*m == sel)
                return (void *)m[2];
        }
    }
    return NULL;
}

static uint32_t nil_imp(id self, SEL sel) {
    // init* sin implementar: devolver self para que [[X alloc] init] no sea nil.
    return !strncmp(sel, "init", 4) ? (uint32_t)self : 0;
}

static void *lookup(objc_class *cls, SEL sel, id self) {
    if (!cls)
        return (void *)nil_imp;
    uint32_t h = (((uintptr_t)cls >> 3) ^ ((uintptr_t)sel >> 2) * 2654435761u) & (CACHE_SIZE - 1);
    if (cache[h].cls == cls && cache[h].sel == sel) {
        void *imp = cache[h].imp;
        __sync_synchronize();
        if (cache[h].cls == cls && cache[h].sel == sel) // otro hilo no la reescribió en el medio
            return imp;
    }
    void *imp = NULL;
    for (objc_class *c = cls; c && !imp; c = c->superclass)
        imp = find_in(c, sel);
    if (!imp) {
        // Selector no internado (cadena armada por el juego en runtime): reintentar por nombre.
        SEL canon = sel_intern(sel, 1);
        if (canon != sel)
            return lookup(cls, canon, self);
        l_error("objc: %c[%s %s] sin implementar", is_meta(cls) ? '+' : '-', class_getName(cls), sel);
        imp = (void *)nil_imp;
    }
    pthread_mutex_lock(&rt_lock);
    cache[h].cls = NULL;
    __sync_synchronize();
    cache[h].imp = imp;
    cache[h].sel = sel;
    __sync_synchronize();
    cache[h].cls = cls;
    pthread_mutex_unlock(&rt_lock);
    return imp;
}

void *objc_lookup_imp(id self, SEL sel) { return lookup(object_getClass(self), sel, self); }

void *objc_lookup_super2(id *sup, SEL sel) { return lookup(((objc_class *)sup[1])->superclass, sel, sup[0]); }

int objc_responds(id o, SEL s) {
    for (objc_class *c = object_getClass(o); c; c = c->superclass)
        if (find_in(c, s))
            return 1;
    return 0;
}

uint32_t objc_msg(id self, SEL sel, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    if (!self)
        return 0;
    return objc_call_imp(objc_lookup_imp(self, sel), self, sel, a, b, c, d);
}

// ---------------------------------------------------------------- objetos

// Cabecera oculta antes de cada objeto que creamos. Los objetos estáticos (CFStrings constantes
// del binario, clases) no la tienen: retain/release los ignoran.
#define HDR_MAGIC 0x4F424A48 // 'OBJH'
typedef struct {
    uint32_t magic;
    volatile int32_t rc;
    void *ext;
    uint32_t pad;
} objhdr;
#define HDR(o) ((objhdr *)((uint8_t *)(o) - sizeof(objhdr)))

static int has_hdr(id o) {
    uintptr_t a = (uintptr_t)o;
    // Objetos del heap de newlib: fuera de la imagen del juego y alineados a 8.
    return a && !(a & 7) && !is_guest_addr(a) && HDR(o)->magic == HDR_MAGIC;
}

id objc_alloc(objc_class *c) {
    size_t size = ro_of(c)->instanceSize;
    if (size < 4)
        size = 4;
    objhdr *h = calloc(1, sizeof(objhdr) + size);
    if (!h)
        fatal_error("objc_alloc(%s, %u): sin memoria", class_getName(c), (unsigned)size);
    h->magic = HDR_MAGIC;
    h->rc = 1;
    id o = h + 1;
    *(objc_class **)o = c;
    return o;
}

void *objc_ext(id o, size_t size) {
    if (!has_hdr(o))
        return NULL;
    objhdr *h = HDR(o);
    if (!h->ext)
        h->ext = calloc(1, size);
    return h->ext;
}

id objc_retain(id o) {
    if (has_hdr(o))
        __sync_fetch_and_add(&HDR(o)->rc, 1);
    return o;
}

static SEL sel_dealloc;
#define RC_DEALLOCATING (-0x40000000) // retain/release dentro de -dealloc no vuelven a llegar a 0
void objc_release(id o) {
    if (has_hdr(o) && __sync_sub_and_fetch(&HDR(o)->rc, 1) == 0) {
        HDR(o)->rc = RC_DEALLOCATING;
        objc_msg(o, sel_dealloc, 0, 0, 0, 0);
    }
}

// NSObject -dealloc: libera la memoria. Se llega acá por [super dealloc] desde el juego.
void objc_free(id o) {
    if (!has_hdr(o))
        return;
    objhdr *h = HDR(o);
    h->magic = 0xDEADBEEF;
    free(h->ext);
    free(h);
}

int objc_retain_count(id o) { return has_hdr(o) ? HDR(o)->rc : 0x7FFFFFFF; }

// Pools de autorelease: una pila por hilo (TLS), como en Foundation.
typedef struct {
    id *items;
    int n, cap;
    int marks[256], depth;
} ar_pool;
static pthread_key_t pool_key;
static pthread_once_t pool_once = PTHREAD_ONCE_INIT;
static void pool_key_init(void) { pthread_key_create(&pool_key, NULL); }
static ar_pool *cur_pool(void) {
    pthread_once(&pool_once, pool_key_init);
    ar_pool *p = pthread_getspecific(pool_key);
    if (!p) {
        p = calloc(1, sizeof(*p));
        pthread_setspecific(pool_key, p);
    }
    return p;
}

id objc_autorelease(id o) {
    if (!o || !has_hdr(o))
        return o;
    ar_pool *p = cur_pool();
    if (p->n == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 1024;
        p->items = realloc(p->items, p->cap * sizeof(id));
    }
    p->items[p->n++] = o;
    return o;
}

void objc_pool_push(void) {
    ar_pool *p = cur_pool();
    if (p->depth < 256)
        p->marks[p->depth] = p->n;
    p->depth++;
}

void objc_pool_pop(void) {
    ar_pool *p = cur_pool();
    if (p->depth <= 0)
        return;
    p->depth--;
    if (p->depth >= 256) // anidado más allá del tope: se vacía en el pop de nivel 256
        return;
    int mark = p->marks[p->depth];
    while (p->n > mark)
        objc_release(p->items[--p->n]); // un release puede autoliberar más: se relee n
}

int objc_ivar_set(id o, const char *name, id value) {
    for (objc_class *c = object_getClass(o); c && !is_native(c); c = c->superclass) {
        ivar_list *iv = ro_of(c)->ivars;
        for (uint32_t i = 0; iv && i < iv->count; i++) {
            const char *n = *(const char **)((uint8_t *)iv->v + i * iv->entsize + 4);
            int32_t *off = *(int32_t **)((uint8_t *)iv->v + i * iv->entsize);
            if (!strcmp(n, name) || (n[0] == '_' && !strcmp(n + 1, name))) {
                id *slot = (id *)((uint8_t *)o + *off);
                id old = *slot;
                *slot = objc_retain(value);
                objc_release(old);
                return 1;
            }
        }
    }
    return 0;
}

// ---------------------------------------------------------------- funciones C del runtime

static void r_objc_setProperty(id self, SEL _cmd, int32_t offset, id val, int atomic, int copy) {
    id *slot = (id *)((uint8_t *)self + offset);
    id old = *slot;
    *slot = copy ? (id)MSG0(val, "copy") : objc_retain(val);
    objc_release(old);
}
static id r_objc_getProperty(id self, SEL _cmd, int32_t offset, int atomic) {
    return *(id *)((uint8_t *)self + offset);
}
static void r_objc_copyStruct(void *dst, const void *src, int32_t size, int atomic, int strong) {
    memcpy(dst, src, size);
}
static void r_noop(void) {}
static id r_objc_begin_catch(void *exc) { return NULL; }
static objc_class *r_objc_getClass(const char *n) { return objc_getClass(n); }
static SEL r_sel_registerName(const char *n) { return sel_registerName(n); }
static const char *r_sel_getName(SEL s) { return s; }
static const char *r_class_getName(objc_class *c) { return class_getName(c); }
static objc_class *r_object_getClass(id o) { return object_getClass(o); }
static void r_objc_exception_throw(id e) { fatal_error("objc_exception_throw (excepción ObjC del juego)"); }

static uint8_t empty_cache[16], empty_vtable[64], ehtype_id[16];

const import_entry objc_imports[] = {
    // Entran directo (sin ml_align_call): reenvían r0-r3 y la pila tal cual.
    V(objc_msgSend, objc_msgSend), V(objc_msgSend_stret, objc_msgSend_stret),
    V(objc_msgSendSuper2, objc_msgSendSuper2), V(objc_msgSendSuper2_stret, objc_msgSendSuper2_stret),
    V(objc_msgSend_fpret, objc_msgSend),
    F(objc_setProperty, r_objc_setProperty), F(objc_getProperty, r_objc_getProperty),
    F(objc_copyStruct, r_objc_copyStruct), F(objc_enumerationMutation, r_noop),
    F(objc_begin_catch, r_objc_begin_catch), F(objc_end_catch, r_noop), F(objc_exception_throw, r_objc_exception_throw),
    F(objc_exception_rethrow, r_objc_exception_throw), F(objc_getClass, r_objc_getClass),
    F(objc_lookUpClass, r_objc_getClass), F(sel_registerName, r_sel_registerName),
    F(sel_getUid, r_sel_registerName), F(sel_getName, r_sel_getName), F(class_getName, r_class_getName),
    F(object_getClass, r_object_getClass), F(objc_retain, objc_retain), F(objc_release, objc_release),
    F(objc_autorelease, objc_autorelease), F(__objc_personality_v0, r_noop),
    V(_objc_empty_cache, empty_cache), V(_objc_empty_vtable, empty_vtable), V(OBJC_EHTYPE_id, ehtype_id),
};
TABLE_END(objc)

void objc_rt_init(void) { sel_dealloc = sel_registerName("dealloc"); }
