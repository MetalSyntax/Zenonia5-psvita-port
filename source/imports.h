#pragma once
#include <stdint.h>

typedef struct {
    const char *name; // nombre Mach-O (con '_' inicial)
    uintptr_t addr;
    int data; // 1 = variable (se enlaza directo); 0 = función (entra por ml_align_call)
} import_entry;

// Implementación de un import del juego, o NULL si no hay.
const import_entry *imports_lookup(const char *name);

// Tablas por módulo (cada .c exporta la suya; imports.c las recorre en este orden).
#define IMPORT_TABLE(t)                                                                                      \
    extern const import_entry t##_imports[];                                                                 \
    extern const int t##_imports_count;
IMPORT_TABLE(patch)
IMPORT_TABLE(darwin)
IMPORT_TABLE(gles)
IMPORT_TABLE(openal)
IMPORT_TABLE(audio)
IMPORT_TABLE(objc)
IMPORT_TABLE(foundation)
IMPORT_TABLE(uikit)
IMPORT_TABLE(cxx)
IMPORT_TABLE(ios5)

// La Vita es softfp como iOS: floats y doubles viajan en registros enteros en los dos lados, así
// que las funciones con float de newlib/vitaGL se enlazan directo. Lo que NO coincide: Darwin no
// alinea los int64/double a par de registros (r1:r2 es válido); esas firmas necesitan wrapper.
#define SOFTFP __attribute__((pcs("aapcs")))

// name sin el "_" que agrega Mach-O: F(__divsi3, fn) registra "___divsi3".
#define F(name, fn) {"_" #name, (uintptr_t)(fn), 0}
#define D(name) {"_" #name, (uintptr_t)(name), 0} // misma firma en newlib/vitaGL
#define V(name, ptr) {"_" #name, (uintptr_t)(ptr), 1} // variable, no función
#define TABLE_END(t) const int t##_imports_count = sizeof(t##_imports) / sizeof(t##_imports[0]);
