// Busca un import en las tablas de cada módulo (patch.c primero: el juego puede pisar cualquiera).
#include <string.h>

#include "imports.h"

static const struct {
    const import_entry *table;
    const int *count;
} tables[] = {
    {patch_imports, &patch_imports_count},   {darwin_imports, &darwin_imports_count},
    {gles_imports, &gles_imports_count},     {openal_imports, &openal_imports_count},
    {audio_imports, &audio_imports_count},   {objc_imports, &objc_imports_count},
    {foundation_imports, &foundation_imports_count}, {uikit_imports, &uikit_imports_count},
    {cxx_imports, &cxx_imports_count},       {ios5_imports, &ios5_imports_count},
};

const import_entry *objc_class_import(const char *name); // objc.c
const import_entry *cxx_import(const char *name);        // cxx_imports.c
const import_entry *ios5_block_import(const char *name); // ios5.c
const import_entry *const_string_import(const char *name); // foundation.c

const import_entry *imports_lookup(const char *name) {
    for (unsigned t = 0; t < sizeof(tables) / sizeof(tables[0]); t++)
        for (int i = 0; i < *tables[t].count; i++)
            if (!strcmp(tables[t].table[i].name, name))
                return &tables[t].table[i];
    const import_entry *e = objc_class_import(name); // _OBJC_CLASS_$_X: nativa o generada vacía
    if (!e)
        e = ios5_block_import(name); // isa de los bloques
    if (!e)
        e = cxx_import(name);
    return e ? e : const_string_import(name); // NSFooKey, UIBarNotification...: NSString con su nombre
}
