#pragma once
// Runtime Objective-C 2 mínimo (ABI de iOS ARM 32 bits) para ejecutar las clases del juego.
//
// Las clases del juego se usan tal como vienen en el Mach-O (class_t + class_ro_t, ya rebaseados).
// Las de Apple (NSObject, NSString, UIView, ...) son "clases nativas": mismo layout de class_t,
// con una tabla de métodos en C (nmethod) en vez de method_list. Una clase del juego puede heredar
// de una nativa (EAGLView : UIView) y la búsqueda de métodos recorre las dos sin distinguir.
#include <stddef.h>
#include <stdint.h>

typedef void *id;
typedef const char *SEL; // selector internado: comparar punteros
typedef struct objc_class objc_class;

typedef struct {
    uint32_t entsize, count;
    struct {
        SEL name;
        const char *types;
        void *imp;
    } m[];
} method_list;

typedef struct {
    uint32_t entsize, count;
    struct {
        int32_t *offset;
        const char *name, *type;
        uint32_t align, size;
    } v[];
} ivar_list;

typedef struct {
    uint32_t flags, instanceStart, instanceSize, ivarLayout;
    const char *name;
    method_list *baseMethods;
    void *baseProtocols;
    ivar_list *ivars;
    uint32_t weakIvarLayout;
    void *baseProperties;
} class_ro;

struct objc_class {
    objc_class *isa, *superclass;
    void *cache, *vtable; // nativas: vtable = tabla nmethod
    class_ro *ro;         // del juego: los 2 bits bajos son flags
};

#define RO_META 0x1
#define RO_NATIVE 0x10000000

typedef struct {
    const char *sel;
    void *imp;
} nmethod;
#define NM(s, f) {s, (void *)(f)}

// ---- definición de clases nativas (antes de macho_load: los binds las necesitan)
objc_class *objc_define(const char *name, const char *super, size_t instance_size, const nmethod *imethods,
                        const nmethod *cmethods);
objc_class *objc_getClass(const char *name);
const char *class_getName(objc_class *c);
int objc_is_kind(id obj, objc_class *c);
objc_class *object_getClass(id obj);

// ---- imagen del juego (después de macho_load)
void objc_init_image(void);

// ---- selectores
SEL sel_registerName(const char *s);

// ---- objetos
id objc_alloc(objc_class *c);          // cero, isa, rc = 1
id objc_retain(id o);
void objc_release(id o);
id objc_autorelease(id o);
void *objc_ext(id o, size_t size);     // estado nativo por objeto (UIView, ...), se crea al pedirlo
void objc_pool_push(void);
void objc_pool_pop(void);
int objc_ivar_set(id o, const char *name, id value); // KVC mínimo (outlets del nib)
int objc_responds(id o, SEL s);

// ---- mensajes desde nuestro C (preservan r9; hasta 4 argumentos extra de 32 bits)
uint32_t objc_msg(id self, SEL sel, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
static inline id objc_msg_id(id o, const char *s, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return (id)objc_msg(o, sel_registerName(s), a, b, c, d);
}
#define MSG0(o, s) objc_msg_id((id)(o), s, 0, 0, 0, 0)
#define MSG1(o, s, a) objc_msg_id((id)(o), s, (uint32_t)(a), 0, 0, 0)
#define MSG2(o, s, a, b) objc_msg_id((id)(o), s, (uint32_t)(a), (uint32_t)(b), 0, 0)
#define MSG3(o, s, a, b, c) objc_msg_id((id)(o), s, (uint32_t)(a), (uint32_t)(b), (uint32_t)(c), 0)

extern id objc_msgSend(id self, SEL sel, ...);
