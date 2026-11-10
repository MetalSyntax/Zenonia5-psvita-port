// Loader de Mach-O 32 bits (MH_EXECUTE de iOS, ARMv6/ARMv7, PIE o no) para la Vita.
//
// Base: el loader probado de Inotia2-vita. Diferencias:
//  * Reubicaciones e imports precalculados en el host (tools/ios_prep.py): rebase.bin (palabras
//    con dirección absoluta) y binds.bin (slot -> símbolo externo). Sirve igual para binarios
//    clásicos sin relocs (Gameloft 2009-2010, -mdynamic-no-pic, sin símbolos) y para PIE.
//  * Todos los segmentos se mapean en vmaddr + MACHO_SLIDE con kuKernelAllocMemBlock en
//    dirección fija, conservando las distancias entre segmentos.
//  * Expone las secciones (macho_section) para el runtime ObjC.
#include <kubridge.h>
#include <psp2/kernel/sysmem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imports.h"
#include "log.h"
#include "macho.h"

#define LC_SEGMENT 0x1
#define LC_SYMTAB 0x2
#define LC_UNIXTHREAD 0x5
#define LC_MAIN 0x80000028
#define LC_ENCRYPTION_INFO 0x21
#define N_STAB 0xE0
#define N_TYPE 0x0E
#define N_SECT 0x0E
#define N_ARM_THUMB_DEF 0x0008

#ifndef SCE_KERNEL_MEMBLOCK_TYPE_USER_RX
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RX 0x0C20D050
#endif

#define ALIGN(x, a) (((x) + (a)-1) & ~((a)-1))
#define THUNK_ARENA_SIZE 0x40000
#define MAX_SEGS 8
#define MAX_SECTS 96

typedef struct {
    char segname[16];
    uint32_t vmaddr, vmsize, fileoff, filesize, maxprot, initprot, nsects, flags;
} segment_command;

typedef struct {
    char sectname[16], segname[16];
    uint32_t addr, size, offset, align, reloff, nreloc, flags, reserved1, reserved2;
} section;

typedef struct {
    uint32_t n_strx;
    uint8_t n_type, n_sect;
    uint16_t n_desc;
    uint32_t n_value;
} nlist;

static uint8_t *file; // el Mach-O (slice elegido); se conserva para símbolos y secciones
static nlist *symtab;
static uint32_t nsyms;
static const char *strtab;
static section *sects[MAX_SECTS];
static int nsect;
static uint32_t img_lo, img_hi, entry_pc, entry_is_main; // LC_MAIN: entry_pc es main() (con bit Thumb)
static uint8_t *text_buf; // __TEXT editable antes de pasarlo al bloque RX
static uint32_t text_vm, text_size;

static uint32_t *thunk_head, *thunk_end;
static const char **unimpl_names;
static int unimpl_count;

extern void ml_unimpl_entry(void);
extern void ml_align_call(void);

void ml_unimpl_log(int idx) {
    static uint8_t seen[8192];
    if (idx < (int)sizeof(seen) && seen[idx] < 3) {
        seen[idx]++;
        l_error("unimpl: %s%s", unimpl_names[idx], seen[idx] == 3 ? " (silenciado)" : "");
    }
}

// ldr ip, =ip_value; ldr pc, =jump
uintptr_t macho_thunk(uint32_t ip_value, void *jump) {
    if (thunk_head + 4 > thunk_end)
        fatal_error("arena de thunks llena");
    uint32_t code[4] = {0xE59FC000, 0xE59FF000, ip_value, (uint32_t)jump};
    uintptr_t addr = (uintptr_t)thunk_head;
    kuKernelCpuUnrestrictedMemcpy(thunk_head, code, sizeof(code));
    kuKernelFlushCaches(thunk_head, sizeof(code));
    thunk_head += 4;
    return addr;
}

static uintptr_t make_unimpl_thunk(const char *name) {
    unimpl_names = realloc(unimpl_names, (unimpl_count + 1) * sizeof(char *));
    unimpl_names[unimpl_count] = name;
    return macho_thunk(unimpl_count++, ml_unimpl_entry);
}

// Dato sin implementar: un bloque en cero compartido por todas las referencias al símbolo.
static uintptr_t make_unimpl_data(const char *name) {
    static struct {
        const char *name;
        uintptr_t addr;
    } blocks[512];
    static int nblocks;
    for (int i = 0; i < nblocks; i++)
        if (!strcmp(blocks[i].name, name))
            return blocks[i].addr;
    l_error("unimpl (dato): %s", name);
    uintptr_t addr = (uintptr_t)calloc(1, 256);
    if (nblocks < 512)
        blocks[nblocks++] = (typeof(blocks[0])){name, addr};
    return addr;
}

// Misma heurística que tools/ios_prep.py: ¿el import es una variable y no una función?
static int is_data_name(const char *n) {
    static const char *pre[] = {"_OBJC_CLASS_$_", "_OBJC_METACLASS_$_", "_OBJC_EHTYPE_", "_kCF", "_kEAGL",
                                "_kAudio", "_kCG", "_kSC", "___CFConstantStringClassReference", "___stderrp",
                                "___stdoutp", "___stdinp", "__DefaultRuneLocale", "___stack_chk_guard",
                                "__objc_empty", "_CGPointZero", "_CGRectZero", "_CGSizeZero", "__ZTV", "__ZTI",
                                "__ZTS", "__ZNSs4_Rep20_S_empty", "_NSDefault", "_NSRunLoop", "_MPMedia",
                                "_MPMovie", "_UIApplication", "_UITextField", "_NSLocale", "_NSError"};
    for (unsigned i = 0; i < sizeof(pre) / sizeof(pre[0]); i++)
        if (!strncmp(n, pre[i], strlen(pre[i])))
            return 1;
    size_t l = strlen(n);
    return (l > 3 && !strcmp(n + l - 3, "Key")) || (l > 12 && !strcmp(n + l - 12, "Notification"));
}

uintptr_t macho_resolve(const char *name, int is_lazy) {
    const import_entry *e = imports_lookup(name);
    if (e)
        return e->data ? e->addr : macho_thunk(e->addr, ml_align_call);
    return (!is_lazy && is_data_name(name)) ? make_unimpl_data(name) : make_unimpl_thunk(name);
}

static uint32_t *word_ptr(uint32_t vm) {
    if (vm < img_lo || vm + 4 > img_hi)
        return NULL;
    if (text_buf && vm >= text_vm && vm + 4 <= text_vm + text_size)
        return (uint32_t *)(text_buf + (vm - text_vm));
    return (uint32_t *)(vm + MACHO_SLIDE);
}

static int alloc_block(const char *name, int type, uintptr_t addr, uint32_t size) {
    SceKernelAllocMemBlockKernelOpt opt;
    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    opt.attr = 0x1;
    opt.field_C = addr;
    SceUID id = kuKernelAllocMemBlock(name, type, size, &opt);
    if (id < 0)
        return id;
    void *base;
    sceKernelGetMemBlockBase(id, &base);
    if ((uintptr_t)base != addr)
        fatal_error("%s quedó en %p y no en 0x%08X", name, base, (unsigned)addr);
    return 0;
}

static void *read_file(const char *path, long *size) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *b = malloc(*size);
    if (b && fread(b, 1, *size, f) != (size_t)*size) {
        free(b);
        b = NULL;
    }
    fclose(f);
    return b;
}

static uint32_t be32(const uint8_t *p) { return p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

int macho_load(const char *bin_path, const char *rebase_path, const char *binds_path) {
    long fsize;
    uint8_t *raw = read_file(bin_path, &fsize);
    if (!raw)
        fatal_error("No se encuentra %s (copiá la .app del IPA a game_app/)", bin_path);
    file = raw;
    if (be32(raw) == 0xCAFEBABE) { // universal: el mismo slice que eligió ios_prep.py (armv7, si no armv6)
        uint32_t n = be32(raw + 4), off = 0;
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *a = raw + 8 + i * 20;
            if (be32(a) == 12 && (be32(a + 4) == 9 || (be32(a + 4) == 6 && !off)))
                off = be32(a + 8);
        }
        if (!off)
            fatal_error("binario universal sin slice ARM");
        file = raw + off;
    }
    uint32_t *hdr = (uint32_t *)file;
    if (hdr[0] != 0xFEEDFACE || hdr[1] != 12 || hdr[3] != 2)
        fatal_error("%s no es un ejecutable ARM de 32 bits", bin_path);

    segment_command *segs[MAX_SEGS];
    int nseg = 0;
    uint8_t *lc = file + 28;
    img_lo = 0xFFFFFFFF;
    img_hi = 0;
    for (uint32_t i = 0; i < hdr[4]; i++) {
        uint32_t cmd = ((uint32_t *)lc)[0], size = ((uint32_t *)lc)[1];
        if (cmd == LC_SEGMENT) {
            segment_command *seg = (segment_command *)(lc + 8);
            if (strncmp(seg->segname, "__PAGEZERO", 16) && strncmp(seg->segname, "__LINKEDIT", 16) && seg->vmsize &&
                nseg < MAX_SEGS) {
                segs[nseg++] = seg;
                if (seg->vmaddr < img_lo)
                    img_lo = seg->vmaddr;
                if (seg->vmaddr + seg->vmsize > img_hi)
                    img_hi = seg->vmaddr + seg->vmsize;
            }
            section *s = (section *)(seg + 1);
            for (uint32_t j = 0; j < seg->nsects && nsect < MAX_SECTS; j++)
                sects[nsect++] = &s[j];
            if (!strncmp(seg->segname, "__TEXT", 16)) {
                text_vm = seg->vmaddr;
                text_size = seg->vmsize;
            }
        } else if (cmd == LC_SYMTAB) {
            uint32_t *st = (uint32_t *)(lc + 8);
            symtab = (nlist *)(file + st[0]);
            nsyms = st[1];
            strtab = (const char *)(file + st[2]);
        } else if (cmd == LC_UNIXTHREAD) {
            entry_pc = ((uint32_t *)lc)[4 + 15];
        } else if (cmd == LC_MAIN) { // iOS 6+: entryoff desde el inicio de __TEXT
            entry_pc = ((uint32_t *)lc)[2];
            entry_is_main = 1;
        } else if (cmd == LC_ENCRYPTION_INFO && ((uint32_t *)lc)[4] != 0) {
            fatal_error("El ejecutable sigue cifrado (FairPlay cryptid=1): hace falta uno descifrado");
        }
        lc += size;
    }
    if (!text_size)
        fatal_error("Mach-O sin __TEXT");
    if (entry_is_main)
        entry_pc += text_vm;
    img_hi = ALIGN(img_hi, 0x1000);
    l_info("Mach-O: imagen 0x%08X-0x%08X -> 0x%08X (TEXT 0x%X bytes, %d segmentos, entry 0x%08X)",
           (unsigned)img_lo, (unsigned)img_hi, (unsigned)(img_lo + MACHO_SLIDE), (unsigned)text_size, nseg,
           (unsigned)entry_pc);

    // Bloques: thunks justo antes de la imagen, __TEXT RX, el resto RW (uno por segmento).
    if (alloc_block("ml_thunks", SCE_KERNEL_MEMBLOCK_TYPE_USER_RX, img_lo + MACHO_SLIDE - THUNK_ARENA_SIZE,
                    THUNK_ARENA_SIZE) < 0)
        fatal_error("kuKernelAllocMemBlock falló: ¿está kubridge.skprx en ur0:tai/config.txt?");
    thunk_head = (uint32_t *)(img_lo + MACHO_SLIDE - THUNK_ARENA_SIZE);
    thunk_end = thunk_head + THUNK_ARENA_SIZE / 4;
    for (int i = 0; i < nseg; i++) {
        segment_command *s = segs[i];
        int is_text = s->vmaddr == text_vm;
        uint32_t start = s->vmaddr & ~0xFFF, end = ALIGN(s->vmaddr + s->vmsize, 0x1000);
        if (alloc_block(is_text ? "ml_text" : "ml_data",
                        is_text ? SCE_KERNEL_MEMBLOCK_TYPE_USER_RX : SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                        start + MACHO_SLIDE, end - start) < 0)
            fatal_error("no se pudo mapear %.16s en 0x%08X", s->segname, (unsigned)(start + MACHO_SLIDE));
        if (is_text) {
            text_buf = calloc(1, text_size);
            memcpy(text_buf, file + s->fileoff, s->filesize);
        } else {
            memset((void *)(start + MACHO_SLIDE), 0, end - start);
            memcpy((void *)(s->vmaddr + MACHO_SLIDE), file + s->fileoff, s->filesize);
        }
    }

    // rebase.bin: 'RBS1', count, count * vmaddr
    long sz;
    uint32_t *rb = read_file(rebase_path, &sz);
    if (!rb || rb[0] != 0x31534252)
        fatal_error("No se encuentra o no es válido %s (tools/ios_prep.py)", rebase_path);
    for (uint32_t i = 0; i < rb[1]; i++) {
        uint32_t *w = word_ptr(rb[2 + i]);
        if (!w)
            fatal_error("rebase fuera de la imagen: 0x%08X", (unsigned)rb[2 + i]);
        *w += MACHO_SLIDE;
    }
    l_info("rebase: %u palabras", (unsigned)rb[1]);
    free(rb);

    // binds.bin: 'BND1', count, {u32 vmaddr, u32 addend, u8 lazy, char name[]}. name "" = símbolo
    // propio (weak/coalesced de C++): addend es su dirección absoluta en la imagen.
    uint8_t *bd = read_file(binds_path, &sz);
    if (!bd || ((uint32_t *)bd)[0] != 0x31444E42)
        fatal_error("No se encuentra o no es válido %s (tools/ios_prep.py)", binds_path);
    uint32_t nb = ((uint32_t *)bd)[1];
    const uint8_t *p = bd + 8;
    char last_name[256] = "";
    uintptr_t last_addr = 0;
    int last_lazy = -1;
    for (uint32_t i = 0; i < nb; i++) {
        uint32_t vm, addend;
        memcpy(&vm, p, 4);
        memcpy(&addend, p + 4, 4);
        int lazy = p[8];
        const char *name = (const char *)p + 9;
        p += 9 + strlen(name) + 1;
        uint32_t *w = word_ptr(vm);
        if (!w)
            fatal_error("bind fuera de la imagen: 0x%08X (%s)", (unsigned)vm, name);
        if (!*name) {
            *w = addend + MACHO_SLIDE;
            continue;
        }
        if (strcmp(last_name, name) || lazy != last_lazy) {
            // macho_resolve guarda el puntero al nombre (log de unimpl): se duplica.
            last_addr = macho_resolve(strdup(name), lazy);
            snprintf(last_name, sizeof(last_name), "%s", name);
            last_lazy = lazy;
        }
        *w = last_addr + addend;
    }
    free(bd);
    l_info("binds: %u slots, %d imports sin implementar", (unsigned)nb, unimpl_count);
    return 0;
}

// Después de los parches de texto (patch.c): copiar __TEXT al bloque RX.
// VFPv2 "short vector" (FPSCR.LEN != 0): truco de ARMv6 para matrices 4x4 que el Cortex-A9 no implementa
// (Undefined Instruction 0x30002). Bloque: orr rX, rX, #0x30000 (LEN=3 -> vectores de 4, stride 1);
// vmsr fpscr, rX; ...vmul/vmla...; vmsr fpscr (salida, con LEN ya limpio). Se anula el vmsr de entrada
// y cada operación vectorial pasa a su equivalente NEON (q = 4 registros s alineados, escalar del banco
// s0-s7 = dN[x]). La salida queda igual: deja FZ/DN como en iOS. Si un bloque tiene una operación VFP
// que la tabla no cubre, no se toca (y se avisa): ampliar la tabla con su traducción.
static void patch_vfp_vectors(void) {
    static const uint32_t vfp_neon[][2] = {
        // Gameloft CMatrix (Shrek Kart): s8/s12 = s16..s28 (matriz) x s0..s7 (escalares)
        {0xee284a00, 0xf3a84940}, // vmul s8,s16,s0  -> vmul.f32 q2,q4,d0[0]
        {0xee0a4a20, 0xf3aa4160}, // vmla s8,s20,s1  -> vmla.f32 q2,q5,d0[1]
        {0xee0c4a01, 0xf3ac4141}, // vmla s8,s24,s2  -> vmla.f32 q2,q6,d1[0]
        {0xee0e4a21, 0xf3ae4161}, // vmla s8,s28,s3  -> vmla.f32 q2,q7,d1[1]
        {0xee286a02, 0xf3a86942}, // vmul s12,s16,s4 -> vmul.f32 q3,q4,d2[0]
        {0xee0a6a22, 0xf3aa6162}, // vmla s12,s20,s5 -> vmla.f32 q3,q5,d2[1]
        {0xee0c6a03, 0xf3ac6143}, // vmla s12,s24,s6 -> vmla.f32 q3,q6,d3[0]
        {0xee0e6a23, 0xf3ae6163}, // vmla s12,s28,s7 -> vmla.f32 q3,q7,d3[1]
    };
    uint32_t *w = (uint32_t *)text_buf, n = text_size / 4, blocks = 0;
    for (uint32_t i = 1; i + 1 < n; i++) {
        if ((w[i] & 0x0fff0fff) != 0x0ee10a10 || (w[i - 1] & 0x0fe00fff) != 0x03800803) // orr rX, rY, #0x30000; vmsr
            continue;
        uint32_t end = i + 1;
        while (end < n && end < i + 64 && (w[end] & 0x0fff0fff) != 0x0ee10a10)
            end++;
        int ok = end < i + 64, todo = 0;
        for (uint32_t p = i + 1; ok && p < end; p++) {
            if ((w[p] & 0x0f000e10) != 0x0e000a00) // no es una operación VFP de datos (cdp cp10)
                continue;
            unsigned k = 0;
            while (k < sizeof(vfp_neon) / sizeof(vfp_neon[0]) && vfp_neon[k][0] != w[p])
                k++;
            if (k == sizeof(vfp_neon) / sizeof(vfp_neon[0])) {
                l_info("VFP vector en 0x%08X: 0x%08X sin traducción NEON (bloque sin parchear)",
                       (unsigned)(text_vm + p * 4), (unsigned)w[p]);
                ok = 0;
            }
            todo++;
        }
        if (!ok || !todo)
            continue;
        w[i] = 0xe1a00000; // nop: FPSCR.LEN queda en 0
        for (uint32_t p = i + 1; p < end; p++)
            for (unsigned k = 0; k < sizeof(vfp_neon) / sizeof(vfp_neon[0]); k++)
                if (vfp_neon[k][0] == w[p]) {
                    w[p] = vfp_neon[k][1];
                    break;
                }
        blocks++;
        i = end;
    }
    if (blocks)
        l_info("VFP vector -> NEON: %u bloques", (unsigned)blocks);
}

void macho_finalize(void) {
    patch_vfp_vectors();
    kuKernelCpuUnrestrictedMemcpy((void *)(text_vm + MACHO_SLIDE), text_buf, text_size);
    kuKernelFlushCaches((void *)(text_vm + MACHO_SLIDE), text_size);
    free(text_buf);
    text_buf = NULL;
}

// Escribe una palabra en la imagen ya finalizada (__TEXT es RX: se escribe por kubridge).
void macho_write_word(uint32_t *addr, uint32_t v) {
    uintptr_t a = (uintptr_t)addr;
    if (a >= text_vm + MACHO_SLIDE && a < text_vm + MACHO_SLIDE + text_size)
        kuKernelCpuUnrestrictedMemcpy(addr, &v, 4);
    else
        *addr = v;
}

int macho_patch_text(uint32_t vm, const void *data, uint32_t len) {
    if (!text_buf || vm < text_vm || vm + len > text_vm + text_size)
        return -1;
    memcpy(text_buf + (vm - text_vm), data, len);
    return 0;
}

uint32_t macho_read_text(uint32_t vm) {
    if (text_buf && vm >= text_vm && vm + 4 <= text_vm + text_size)
        return *(uint32_t *)(text_buf + (vm - text_vm));
    return *(uint32_t *)(vm + MACHO_SLIDE);
}

int macho_section(const char *seg, const char *name, uint32_t *addr, uint32_t *size) {
    for (int i = 0; i < nsect; i++)
        if (!strncmp(sects[i]->sectname, name, 16) && (!seg || !strncmp(sects[i]->segname, seg, 16))) {
            *addr = sects[i]->addr + MACHO_SLIDE;
            *size = sects[i]->size;
            return 1;
        }
    return 0;
}

void macho_run_inits(void) {
    uint32_t a, s;
    if (!macho_section(NULL, "__mod_init_func", &a, &s))
        return;
    for (uint32_t i = 0; i < s / 4; i++) {
        uintptr_t fn = ((uint32_t *)a)[i];
        l_debug("mod_init[%u] = 0x%08X", (unsigned)i, (unsigned)(fn - MACHO_SLIDE));
        call_guest((void *)fn, 0, 0, 0, 0);
    }
    l_info("inicializadores: %u", (unsigned)(s / 4));
}

static nlist *find_sym(const char *name) {
    for (uint32_t i = 0; symtab && i < nsyms; i++) {
        nlist *s = &symtab[i];
        if (!(s->n_type & N_STAB) && (s->n_type & N_TYPE) == N_SECT && !strcmp(strtab + s->n_strx, name))
            return s;
    }
    return NULL;
}

uintptr_t macho_sym(const char *name) {
    nlist *s = find_sym(name);
    if (!s)
        return 0;
    return (s->n_value + MACHO_SLIDE) | ((s->n_desc & N_ARM_THUMB_DEF) ? 1 : 0);
}

uintptr_t macho_entry(void) { return entry_pc ? entry_pc + MACHO_SLIDE : 0; }
int macho_entry_is_main(void) { return entry_is_main; }

// Redirige una función del juego (vm del Mach-O, bit 0 = Thumb) a dst, que entra con la pila
// alineada. Se escribe en text_buf: llamar antes de macho_finalize().
void macho_hook_addr(uint32_t vm, void *dst) {
    uint32_t patch[2];
    uint32_t addr = vm & ~1;
    if (vm & 1) {
        if (addr & 2) {
            uint16_t nop = 0xBF00;
            macho_patch_text(addr, &nop, 2);
            addr += 2;
        }
        patch[0] = 0xF000F8DF; // ldr.w pc, [pc]
    } else {
        patch[0] = 0xE51FF004; // ldr pc, [pc, #-4]
    }
    patch[1] = macho_thunk((uint32_t)dst, ml_align_call);
    if (macho_patch_text(addr, patch, sizeof(patch)) < 0)
        fatal_error("hook fuera de __TEXT o después de finalizar: 0x%08X", (unsigned)vm);
}

int macho_hook(const char *name, void *dst) {
    nlist *s = find_sym(name);
    if (!s) {
        l_error("hook: no existe %s", name);
        return -1;
    }
    macho_hook_addr(s->n_value | ((s->n_desc & N_ARM_THUMB_DEF) ? 1 : 0), dst);
    return 0;
}
