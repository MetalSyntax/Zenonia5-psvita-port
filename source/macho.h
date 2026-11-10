#pragma once
#include <stdint.h>

// El ejecutable de iOS se mapea en su vmaddr + MACHO_SLIDE. Un crash en PC = X se busca en el
// Mach-O en X - MACHO_SLIDE (otool -tV / tools/annotate.py).
#define MACHO_SLIDE 0x98000000u

// Carga el Mach-O (fino o universal) y aplica rebase.bin + binds.bin (tools/ios_prep.py).
int macho_load(const char *bin_path, const char *rebase_path, const char *binds_path);
// Copia __TEXT (ya parcheado) al bloque ejecutable. Llamar después de los hooks/parches.
void macho_finalize(void);
void macho_run_inits(void);
void macho_write_word(uint32_t *addr, uint32_t v); // también en __TEXT (después de finalizar)

uintptr_t macho_sym(const char *name); // 0 si no está (binarios sin símbolos)
uintptr_t macho_entry(void);           // LC_UNIXTHREAD pc (start) o LC_MAIN main(), ya con slide
int macho_entry_is_main(void);         // 1 si macho_entry() es main() (LC_MAIN) y no start
int macho_section(const char *seg, const char *name, uint32_t *addr, uint32_t *size); // addr con slide
int macho_patch_text(uint32_t vm, const void *data, uint32_t len); // vm sin slide
uint32_t macho_read_text(uint32_t vm);
void macho_hook_addr(uint32_t vm, void *dst); // vm sin slide (bit 0 = Thumb)
int macho_hook(const char *name, void *dst);
uintptr_t macho_thunk(uint32_t ip_value, void *jump);
uintptr_t macho_resolve(const char *name, int is_lazy);

// Llamadas al código del juego preservando r9 (temporal en Darwin).
uint32_t call_guest(void *fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
uint32_t call_guest6(void *fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5);

static inline int is_guest_addr(uintptr_t a) { return a >= MACHO_SLIDE && a < MACHO_SLIDE + 0x08000000u; }
