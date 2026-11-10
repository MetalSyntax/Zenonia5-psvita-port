#pragma once
// Parches propios del juego: imports que pisan a los genéricos (patch_imports) y hooks/parches de
// __TEXT por dirección del Mach-O, antes de macho_finalize().
void patch_init(void);
