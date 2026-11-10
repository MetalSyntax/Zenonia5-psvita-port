// libc, libgcc y pthread de iOS (Darwin, ARM softfp) sobre newlib de la Vita.
// Base: darwin.c de Inotia2-vita (probado). Agregados: structs con otro layout en Darwin (stat,
// dirent, tm, pthread_attr/cond/once), ctype por _DefaultRuneLocale, stdio con printf de Darwin,
// setjmp propio, sockets offline y stubs de Mach.
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

extern void *__dynamic_cast(const void *, const void *, const void *, long); // libsupc++

#include "darwin_printf.h"
#include "hle.h"
#include "imports.h"
#include "log.h"
#include "macho.h"

// ---------------------------------------------------------------- rutas

// Rutas absolutas de iOS ("/var/mobile/...", "/private/...") -> dentro de DATA_PATH; si el
// archivo está en el bundle, se usa ese. Las de la Vita (ux0:, app0:) pasan tal cual.
const char *darwin_path(const char *in, char *out, size_t outlen) {
    if (!in)
        return NULL;
    // Ruta al bundle de iOS: /var/.../X.app/archivo, pero también <cwd>/X.app/archivo o X.app/archivo
    // (el juego hace chdir al padre del bundle y arma la ruta con getcwd: ya llega con "ux0:").
    const char *app = strstr(in, ".app/");
    if (app) {
        snprintf(out, outlen, BUNDLE_PATH "%s", app + 5);
        return out;
    }
    if (strchr(in, ':')) {
        snprintf(out, outlen, "%s", in);
        return out;
    }
    if (in[0] != '/') { // relativa: al cwd (chdir del juego o el bundle)
        snprintf(out, outlen, "%s", in);
        return out;
    }
    const char *docs = strstr(in, "/Documents");
    if (docs) {
        snprintf(out, outlen, DATA_PATH "%s", docs + 1);
        return out;
    }
    while (*in == '/')
        in++;
    char bundle_try[512];
    snprintf(bundle_try, sizeof(bundle_try), BUNDLE_PATH "%s", in);
    struct stat st;
    if (stat(bundle_try, &st) == 0) {
        snprintf(out, outlen, "%s", bundle_try);
        return out;
    }
    snprintf(out, outlen, DATA_PATH "%s", in);
    return out;
}
#define PATH(p) char path_buf[512]; const char *P = darwin_path(p, path_buf, sizeof(path_buf))

static FILE *d_fopen(const char *path, const char *mode) {
    PATH(path);
    FILE *f = fopen(P, mode);
    l_debug("fopen(%s, %s) -> %p", P, mode, f);
    return f;
}
static FILE *d_freopen(const char *path, const char *mode, FILE *f) {
    PATH(path);
    return freopen(P, mode, f);
}
static int d_mkdir(const char *path, int mode) {
    PATH(path);
    return mkdir(P, mode);
}
static int d_remove(const char *path) {
    PATH(path);
    return remove(P);
}
static int d_unlink(const char *path) {
    PATH(path);
    return unlink(P);
}
static int d_rmdir(const char *path) {
    PATH(path);
    return rmdir(P);
}
static int d_rename(const char *a, const char *b) {
    char pa[512], pb[512];
    return rename(darwin_path(a, pa, sizeof(pa)), darwin_path(b, pb, sizeof(pb)));
}
static int d_chdir(const char *path) {
    PATH(path);
    int r = chdir(P);
    l_info("chdir(%s) -> %d", P, r);
    return r;
}
static int d_access(const char *path, int mode) {
    PATH(path);
    struct stat st;
    return stat(P, &st);
}
static char *d_realpath(const char *path, char *out) {
    PATH(path);
    if (!out)
        out = malloc(1024);
    snprintf(out, 1024, "%s", P);
    return out;
}
static int d_open(const char *path, int flags, int mode) {
    PATH(path);
    // O_CREAT/O_TRUNC/O_APPEND de Darwin (0x200/0x400/0x8) -> newlib
    int f = flags & 3;
    if (flags & 0x200)
        f |= O_CREAT;
    if (flags & 0x400)
        f |= O_TRUNC;
    if (flags & 0x8)
        f |= O_APPEND;
    return open(P, f, mode);
}

// struct stat de Darwin (32 bits, ino32): 96 bytes, int64 alineados a 4.
typedef struct __attribute__((packed)) {
    int32_t st_dev;
    uint32_t st_ino;
    uint16_t st_mode, st_nlink;
    uint32_t st_uid, st_gid;
    int32_t st_rdev;
    int32_t st_atime_, st_atime_ns, st_mtime_, st_mtime_ns, st_ctime_, st_ctime_ns;
    int64_t st_size, st_blocks;
    int32_t st_blksize;
    uint32_t st_flags, st_gen;
    int32_t st_lspare;
    int64_t st_qspare[2];
} darwin_stat;
static void to_darwin_stat(const struct stat *s, darwin_stat *d) {
    memset(d, 0, sizeof(*d));
    // S_IFDIR/S_IFREG coinciden (0040000/0100000)
    d->st_mode = s->st_mode;
    d->st_nlink = 1;
    d->st_size = s->st_size;
    d->st_blocks = (s->st_size + 511) / 512;
    d->st_blksize = 4096;
    d->st_atime_ = s->st_atime;
    d->st_mtime_ = s->st_mtime;
    d->st_ctime_ = s->st_ctime;
}
static int d_stat(const char *path, darwin_stat *out) {
    PATH(path);
    struct stat st;
    int r = stat(P, &st);
    if (!r && out)
        to_darwin_stat(&st, out);
    return r;
}
static int d_fstat(int fd, darwin_stat *out) {
    struct stat st;
    int r = fstat(fd, &st);
    if (!r && out)
        to_darwin_stat(&st, out);
    return r;
}

// dirent de Darwin (ino32): {u32 ino; u16 reclen; u8 type; u8 namlen; char name[256]}
typedef struct {
    uint32_t d_ino;
    uint16_t d_reclen;
    uint8_t d_type, d_namlen;
    char d_name[256];
} darwin_dirent;
typedef struct {
    DIR *dir;
    darwin_dirent ent;
    char path[512];
} d_dir;
static d_dir *d_opendir(const char *path) {
    PATH(path);
    DIR *d = opendir(P);
    if (!d)
        return NULL;
    d_dir *r = calloc(1, sizeof(*r));
    r->dir = d;
    snprintf(r->path, sizeof(r->path), "%s", P);
    return r;
}
static darwin_dirent *d_readdir(d_dir *d) {
    struct dirent *e = d ? readdir(d->dir) : NULL;
    if (!e)
        return NULL;
    snprintf(d->ent.d_name, sizeof(d->ent.d_name), "%s", e->d_name);
    d->ent.d_namlen = strlen(d->ent.d_name);
    d->ent.d_reclen = sizeof(darwin_dirent);
    d->ent.d_ino = 1;
    char full[800];
    snprintf(full, sizeof(full), "%s/%s", d->path, e->d_name);
    struct stat st;
    d->ent.d_type = !stat(full, &st) && S_ISDIR(st.st_mode) ? 4 /* DT_DIR */ : 8 /* DT_REG */;
    return &d->ent;
}
static int d_closedir(d_dir *d) {
    if (!d)
        return -1;
    closedir(d->dir);
    free(d);
    return 0;
}

// ---------------------------------------------------------------- stdio / errno / _chk

static FILE *d_stderrp, *d_stdinp, *d_stdoutp;
static int *d___error(void) { return &errno; }
static void *d___memcpy_chk(void *d, const void *s, size_t n, size_t dn) { return memcpy(d, s, n); }
static void *d___memmove_chk(void *d, const void *s, size_t n, size_t dn) { return memmove(d, s, n); }
static void *d___memset_chk(void *d, int c, size_t n, size_t dn) { return memset(d, c, n); }
static char *d___strcpy_chk(char *d, const char *s, size_t dn) { return strcpy(d, s); }
static char *d___strcat_chk(char *d, const char *s, size_t dn) { return strcat(d, s); }
static char *d___strncpy_chk(char *d, const char *s, size_t n, size_t dn) { return strncpy(d, s, n); }

static int d_sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    words w = {&ap, NULL};
    int r = darwin_vformat(buf, INT_MAX, fmt, &w);
    va_end(ap);
    return r;
}
static int d_snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    words w = {&ap, NULL};
    int r = darwin_vformat(buf, n, fmt, &w);
    va_end(ap);
    return r;
}
static int d___sprintf_chk(char *buf, int flag, size_t len, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    words w = {&ap, NULL};
    int r = darwin_vformat(buf, len, fmt, &w);
    va_end(ap);
    return r;
}
static int d___snprintf_chk(char *buf, size_t n, int flag, size_t len, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    words w = {&ap, NULL};
    int r = darwin_vformat(buf, n, fmt, &w);
    va_end(ap);
    return r;
}
// va_list de Darwin = puntero a las palabras
static int d_vsprintf(char *buf, const char *fmt, const uint32_t *ap) {
    words w = {NULL, ap};
    return darwin_vformat(buf, INT_MAX, fmt, &w);
}
static int d_vsnprintf(char *buf, size_t size, const char *fmt, const uint32_t *ap) {
    words w = {NULL, ap};
    return darwin_vformat(buf, size, fmt, &w);
}
static int file_out(FILE *f, const char *s, int n) {
    if (f == d_stdoutp || f == d_stderrp || f == stdout || f == stderr) {
        char line[1100];
        snprintf(line, sizeof(line), "%.*s", n > 1024 ? 1024 : n, s);
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r'))
            line[--l] = 0;
        if (l)
            l_info("juego: %s", line);
        return n;
    }
    return fwrite(s, 1, n, f);
}
static int d_vfprintf(FILE *f, const char *fmt, const uint32_t *ap) {
    char buf[2048];
    words w = {NULL, ap};
    int n = darwin_vformat(buf, sizeof(buf), fmt, &w);
    return file_out(f, buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1);
}
static int d_fprintf(FILE *f, const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    words w = {&ap, NULL};
    int n = darwin_vformat(buf, sizeof(buf), fmt, &w);
    va_end(ap);
    return file_out(f, buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1);
}
static int d_printf(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    words w = {&ap, NULL};
    int n = darwin_vformat(buf, sizeof(buf), fmt, &w);
    va_end(ap);
    return file_out(d_stdoutp, buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1);
}
static int d_vprintf(const char *fmt, const uint32_t *ap) { return d_vfprintf(d_stdoutp, fmt, ap); }
static int d_puts(const char *s) { return file_out(d_stdoutp, s, strlen(s)); }
static int d_fputs(const char *s, FILE *f) { return file_out(f, s, strlen(s)) >= 0 ? 0 : EOF; }
static size_t d_fwrite(const void *p, size_t sz, size_t n, FILE *f) {
    if (f == d_stdoutp || f == d_stderrp)
        return file_out(f, p, sz * n) / (sz ? sz : 1);
    return fwrite(p, sz, n, f);
}
static int d_putchar(int c) { return c; }
static int d_swprintf(wchar_t *buf, size_t n, const wchar_t *fmt, ...) {
    // formato ancho -> angosto, darwin_vformat, y de vuelta (los juegos lo usan con %d/%s simples)
    char f8[512], o8[1024];
    size_t i = 0;
    for (; fmt[i] && i < sizeof(f8) - 1; i++)
        f8[i] = fmt[i] < 128 ? fmt[i] : '?';
    f8[i] = 0;
    va_list ap;
    va_start(ap, fmt);
    words w = {&ap, NULL};
    int r = darwin_vformat(o8, sizeof(o8), f8, &w);
    va_end(ap);
    for (i = 0; o8[i] && i + 1 < n; i++)
        buf[i] = (unsigned char)o8[i];
    if (n)
        buf[i] = 0;
    return r;
}

static void d___assert_rtn(const char *func, const char *file, int line, const char *expr) {
    l_error("assert del juego: %s (%s:%d %s)", expr, file, line, func);
}
static void d_abort(void) { fatal_error("el juego llamó abort()"); }
static uint32_t d_stack_chk_guard = 0xDEADC0DE;
static void d___stack_chk_fail(void) { fatal_error("__stack_chk_fail (pila corrupta)"); }

static int qsort_guest_cmp;
static int qsort_tramp(const void *a, const void *b) {
    return (int)call_guest((void *)qsort_guest_cmp, (uint32_t)a, (uint32_t)b, 0, 0);
}
static void d_qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *)) {
    int prev = qsort_guest_cmp; // qsort anidado: se restaura al salir
    qsort_guest_cmp = (int)cmp;
    qsort(base, n, sz, qsort_tramp);
    qsort_guest_cmp = prev;
}

// ---------------------------------------------------------------- ctype de Darwin

// _RuneLocale: magic[8], encoding[32], 2 punteros, invalid_rune, runetype[256] (offset 52),
// maplower[256], mapupper[256], ... Las macros isalpha() etc. del juego leen runetype directo.
#define CT_A 0x100
#define CT_C 0x200
#define CT_D 0x400
#define CT_G 0x800
#define CT_L 0x1000
#define CT_P 0x2000
#define CT_S 0x4000
#define CT_U 0x8000
#define CT_X 0x10000
#define CT_B 0x20000
#define CT_R 0x40000
static struct {
    char magic[8], encoding[32];
    void *sgetrune, *sputrune;
    int32_t invalid_rune;
    uint32_t runetype[256];
    int32_t maplower[256], mapupper[256];
    uint8_t rest[256];
} rune_locale;
static void rune_init(void) {
    memcpy(rune_locale.magic, "RuneMagA", 8);
    strcpy(rune_locale.encoding, "NONE");
    for (int c = 0; c < 256; c++) {
        uint32_t t = 0;
        if (c < 128) {
            if (isalpha(c))
                t |= CT_A;
            if (iscntrl(c))
                t |= CT_C;
            if (isdigit(c))
                t |= CT_D;
            if (isgraph(c))
                t |= CT_G;
            if (islower(c))
                t |= CT_L;
            if (ispunct(c))
                t |= CT_P;
            if (isspace(c))
                t |= CT_S;
            if (isupper(c))
                t |= CT_U;
            if (isxdigit(c))
                t |= CT_X;
            if (c == ' ' || c == '\t')
                t |= CT_B;
            if (isprint(c))
                t |= CT_R;
            if (isdigit(c))
                t |= c - '0';
            else if (isxdigit(c))
                t |= (tolower(c) - 'a' + 10);
        }
        rune_locale.runetype[c] = t;
        rune_locale.maplower[c] = c < 128 ? tolower(c) : c;
        rune_locale.mapupper[c] = c < 128 ? toupper(c) : c;
    }
}
static uint32_t d___maskrune(int c, uint32_t mask) { return (c >= 0 && c < 256 ? rune_locale.runetype[c] : 0) & mask; }
static int d___tolower(int c) { return c >= 0 && c < 256 ? rune_locale.maplower[c] : c; }
static int d___toupper(int c) { return c >= 0 && c < 256 ? rune_locale.mapupper[c] : c; }

// ---------------------------------------------------------------- tiempo / sistema

static uint64_t d_mach_absolute_time(void) { return sceKernelGetProcessTimeWide(); }
typedef struct {
    uint32_t numer, denom;
} mach_timebase_info_t;
static int d_mach_timebase_info(mach_timebase_info_t *info) {
    info->numer = 1000; // mach_absolute_time en µs
    info->denom = 1;
    return 0;
}
static int d_usleep(unsigned us) {
    sceKernelDelayThread(us);
    return 0;
}
static int d_nanosleep(const struct timespec *t, struct timespec *rem) {
    sceKernelDelayThread(t->tv_sec * 1000000 + t->tv_nsec / 1000);
    return 0;
}
static int d_sched_yield(void) {
    sceKernelDelayThread(0);
    return 0;
}
static clock_t d_clock(void) { return sceKernelGetProcessTimeWide(); } // CLOCKS_PER_SEC = 1e6 en los dos

// struct tm de Darwin: los 9 int de siempre + long tm_gmtoff + char *tm_zone
typedef struct {
    int f[9];
    long gmtoff;
    const char *zone;
} darwin_tm;
static darwin_tm tm_static;
static darwin_tm *conv_tm(struct tm *t, darwin_tm *out) {
    if (!t)
        return NULL;
    memcpy(out->f, t, sizeof(int) * 9);
    out->gmtoff = 0;
    out->zone = "UTC";
    return out;
}
static darwin_tm *d_localtime(const time_t *t) { return conv_tm(localtime(t), &tm_static); }
static darwin_tm *d_gmtime(const time_t *t) { return conv_tm(gmtime(t), &tm_static); }
static darwin_tm *d_localtime_r(const time_t *t, darwin_tm *out) {
    struct tm tmp;
    return conv_tm(localtime_r(t, &tmp), out);
}

static const char *machine = PORT_DEVICE_MACHINE;
static int d_sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
    if (!strcmp(name, "hw.machine")) {
        size_t len = strlen(machine) + 1;
        if (oldp && *oldlenp >= len)
            memcpy(oldp, machine, len);
        *oldlenp = len;
        return 0;
    }
    if (!strcmp(name, "hw.physmem") || !strcmp(name, "hw.usermem") || !strcmp(name, "hw.memsize")) {
        if (oldp && *oldlenp >= 4)
            *(uint32_t *)oldp = 256 * 1024 * 1024;
        *oldlenp = 4;
        return 0;
    }
    if (!strcmp(name, "hw.ncpu") || !strcmp(name, "hw.cpufrequency")) {
        if (oldp && *oldlenp >= 4)
            *(uint32_t *)oldp = name[3] == 'n' ? 1 : 600000000;
        *oldlenp = 4;
        return 0;
    }
    errno = ENOENT;
    return -1;
}
static int d_sysctl(int *name, unsigned namelen, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
    errno = ENOENT;
    return -1;
}
// struct utsname de Darwin: 5 x char[256]
static int d_uname(char *u) {
    memset(u, 0, 5 * 256);
    strcpy(u, "Darwin");
    strcpy(u + 256, "vita");
    strcpy(u + 512, "9.4.1");
    strcpy(u + 768, "Darwin Kernel Version 9.4.1");
    strcpy(u + 1024, machine);
    return 0;
}
static int d_gethostname(char *n, size_t len) {
    snprintf(n, len, "vita");
    return 0;
}
static char *d_getenv(const char *n) {
    if (!strcmp(n, "HOME"))
        return DATA_PATH;
    if (!strcmp(n, "TMPDIR"))
        return DATA_PATH "tmp/";
    return NULL;
}
static void d_exit(int code) {
    l_info("el juego llamó exit(%d)", code);
    sceKernelExitProcess(0);
}
static int d_zero(void) { return 0; }
static int d_minus1(void) { return -1; }
static void *d_null(void) { return NULL; }

// Mach: el juego consulta memoria libre/prioridades
static int d_host_page_size(int host, uint32_t *out) {
    *out = 4096;
    return 0;
}
static int d_host_statistics(int host, int flavor, uint32_t *info, uint32_t *count) {
    // vm_statistics: free_count, active, inactive, wire... (en páginas)
    memset(info, 0, *count * 4);
    if (*count > 0)
        info[0] = 32768; // 128 MB libres
    return 0;
}

// ---------------------------------------------------------------- red (offline)

static int d_net_fail(void) {
    errno = ENETDOWN;
    return -1;
}
static int d_getaddrinfo(const char *n, const char *s, const void *h, void **res) {
    if (res)
        *res = NULL;
    return 8; // EAI_NONAME
}
static int d_select(int n, void *r, void *w, void *e, void *t) { return 0; }

// ---------------------------------------------------------------- pthread

// Darwin: pthread_mutex_t = {long sig; char opaque[40]}. El mutex de la Vita va en opaque.
// PTHREAD_MUTEX_INITIALIZER de Darwin: sig 0x32AAABA7 (se inicializa al primer uso).
#define DARWIN_MUTEX_SIG_INIT 0x32AAABA7
#define DARWIN_RMUTEX_SIG_INIT 0x32AAABA2
#define ML_MUTEX_SIG 0x4D4C5458
#define ML_MUTEXATTR_SIG 0x4D4C5441
typedef struct {
    uint32_t sig;
    pthread_mutex_t m;
} d_mutex;
typedef struct {
    uint32_t sig;
    int type;
} d_mutexattr;
static pthread_mutex_t lazy_lock = PTHREAD_MUTEX_INITIALIZER;

static int d_pthread_mutexattr_init(d_mutexattr *a) {
    a->sig = ML_MUTEXATTR_SIG;
    a->type = 0;
    return 0;
}
static int d_pthread_mutexattr_settype(d_mutexattr *a, int type) {
    a->type = type; // Darwin: 0 normal, 1 errorcheck, 2 recursive
    return 0;
}
static int d_pthread_mutex_init(d_mutex *m, const d_mutexattr *a) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    int type = a && a->sig == ML_MUTEXATTR_SIG ? a->type : 0;
    // ponytail: NORMAL se crea recursivo; un juego que se re-bloquea por error en iOS se colgaría
    // igual, pero los mutex "normales" de Darwin no detectan nada y algunos juegos dependen de eso.
    pthread_mutexattr_settype(&attr, type == 1 ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_RECURSIVE);
    int r = pthread_mutex_init(&m->m, &attr);
    pthread_mutexattr_destroy(&attr);
    m->sig = ML_MUTEX_SIG;
    return r;
}
static void mutex_lazy(d_mutex *m) {
    if (m->sig == ML_MUTEX_SIG)
        return;
    pthread_mutex_lock(&lazy_lock);
    if (m->sig != ML_MUTEX_SIG) {
        if (m->sig != DARWIN_MUTEX_SIG_INIT && m->sig != DARWIN_RMUTEX_SIG_INIT)
            l_error("mutex sin inicializar (sig 0x%08X) en %p", (unsigned)m->sig, m);
        d_pthread_mutex_init(m, NULL);
    }
    pthread_mutex_unlock(&lazy_lock);
}
static int d_pthread_mutex_lock(d_mutex *m) {
    mutex_lazy(m);
    return pthread_mutex_lock(&m->m);
}
static int d_pthread_mutex_trylock(d_mutex *m) {
    mutex_lazy(m);
    return pthread_mutex_trylock(&m->m) ? 16 /* EBUSY de Darwin */ : 0;
}
static int d_pthread_mutex_unlock(d_mutex *m) {
    mutex_lazy(m);
    return pthread_mutex_unlock(&m->m);
}
static int d_pthread_mutex_destroy(d_mutex *m) {
    if (m->sig != ML_MUTEX_SIG)
        return 0;
    m->sig = 0;
    return pthread_mutex_destroy(&m->m);
}

// pthread_cond_t de Darwin: {long sig; char opaque[24]}; initializer sig 0x3CB0B1BB
#define ML_COND_SIG 0x4D4C434E
typedef struct {
    uint32_t sig;
    pthread_cond_t c;
} d_cond;
static void cond_lazy(d_cond *c) {
    if (c->sig == ML_COND_SIG)
        return;
    pthread_mutex_lock(&lazy_lock);
    if (c->sig != ML_COND_SIG) {
        pthread_cond_init(&c->c, NULL);
        c->sig = ML_COND_SIG;
    }
    pthread_mutex_unlock(&lazy_lock);
}
static int d_pthread_cond_init(d_cond *c, const void *a) {
    c->sig = 0;
    cond_lazy(c);
    return 0;
}
static int d_pthread_cond_wait(d_cond *c, d_mutex *m) {
    cond_lazy(c);
    mutex_lazy(m);
    return pthread_cond_wait(&c->c, &m->m);
}
static int d_pthread_cond_timedwait(d_cond *c, d_mutex *m, const struct timespec *t) {
    cond_lazy(c);
    mutex_lazy(m);
    return pthread_cond_timedwait(&c->c, &m->m, t) ? 60 /* ETIMEDOUT de Darwin */ : 0;
}
static int d_pthread_cond_signal(d_cond *c) {
    cond_lazy(c);
    return pthread_cond_signal(&c->c);
}
static int d_pthread_cond_broadcast(d_cond *c) {
    cond_lazy(c);
    return pthread_cond_broadcast(&c->c);
}
static int d_pthread_cond_destroy(d_cond *c) {
    if (c->sig == ML_COND_SIG)
        pthread_cond_destroy(&c->c);
    c->sig = 0;
    return 0;
}

// pthread_once_t de Darwin: {long sig = 0x30B1BCBA; char opaque[4]}
static int d_pthread_once(uint32_t *once, void (*fn)(void)) {
    pthread_mutex_lock(&lazy_lock);
    int run = once[1] != 0x4F4E4345; // 'ONCE'
    once[1] = 0x4F4E4345;
    pthread_mutex_unlock(&lazy_lock);
    if (run)
        call_guest((void *)fn, 0, 0, 0, 0);
    return 0;
}

// pthread_attr_t de Darwin: {long sig; char opaque[36]}: guardamos stack y detach
#define ML_ATTR_SIG 0x4D4C4154
typedef struct {
    uint32_t sig, stack, detached, prio;
} d_attr;
static int d_pthread_attr_init(d_attr *a) {
    *a = (d_attr){ML_ATTR_SIG, 512 * 1024, 0, 0};
    return 0;
}
static int d_pthread_attr_setstacksize(d_attr *a, size_t s) {
    a->stack = s;
    return 0;
}
static int d_pthread_attr_getstacksize(d_attr *a, size_t *s) {
    *s = a->stack;
    return 0;
}
static int d_pthread_attr_setdetachstate(d_attr *a, int d) {
    a->detached = d == 2; // PTHREAD_CREATE_DETACHED de Darwin
    return 0;
}
static int d_pthread_attr_getschedparam(d_attr *a, int *p) {
    *p = 31;
    return 0;
}

typedef struct {
    void *fn, *arg;
} guest_thread;
static void *thread_tramp(void *p) {
    guest_thread t = *(guest_thread *)p;
    free(p);
    return (void *)call_guest(t.fn, (uint32_t)t.arg, 0, 0, 0);
}
static int d_pthread_create(pthread_t *thr, const d_attr *da, void *fn, void *arg) {
    guest_thread *t = malloc(sizeof(*t));
    t->fn = fn;
    t->arg = arg;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    uint32_t stack = da && da->sig == ML_ATTR_SIG && da->stack >= 64 * 1024 ? da->stack : 512 * 1024;
    pthread_attr_setstacksize(&attr, stack < 256 * 1024 ? 256 * 1024 : stack);
    int r = pthread_create(thr, &attr, thread_tramp, t);
    pthread_attr_destroy(&attr);
    if (!r && da && da->sig == ML_ATTR_SIG && da->detached)
        pthread_detach(*thr);
    l_info("pthread_create(0x%08X, stack %u KB) -> %d", (unsigned)((uintptr_t)fn - MACHO_SLIDE), (unsigned)(stack / 1024), r);
    return r;
}
static int d_pthread_detach(pthread_t t) { return t ? pthread_detach(t) : 0; }
static pthread_t d_pthread_self(void) { return pthread_self(); }
static int d_pthread_equal(pthread_t a, pthread_t b) { return a == b; }
static int d_pthread_setschedparam(pthread_t t, int policy, const void *param) { return 0; }
static int d_pthread_getschedparam(pthread_t t, int *policy, int *param) {
    *policy = 1;
    *param = 31;
    return 0;
}
static uint32_t d_pthread_mach_thread_np(pthread_t t) { return (uint32_t)t; }

// Semáforos con nombre: sem_t* de Darwin -> UID de la Vita.
typedef struct {
    SceUID id;
} d_sem;
static d_sem *d_sem_open(const char *name, int oflag, ...) {
    va_list ap;
    va_start(ap, oflag);
    va_arg(ap, int); // mode
    unsigned value = va_arg(ap, unsigned);
    va_end(ap);
    d_sem *s = malloc(sizeof(*s));
    s->id = sceKernelCreateSema("ml_sem", 0, value, 0x7FFFFFFF, NULL);
    if (s->id < 0) {
        free(s);
        return (d_sem *)-1; // SEM_FAILED
    }
    return s;
}
static int d_sem_close(d_sem *s) {
    if (!s || s == (d_sem *)-1)
        return -1;
    sceKernelDeleteSema(s->id);
    free(s);
    return 0;
}
static int d_sem_unlink(const char *name) { return 0; }
static int d_sem_wait(d_sem *s) {
    if (!s || s == (d_sem *)-1)
        return -1;
    return sceKernelWaitSema(s->id, 1, NULL) < 0 ? -1 : 0;
}
static int d_sem_trywait(d_sem *s) {
    if (!s || s == (d_sem *)-1)
        return -1;
    return sceKernelPollSema(s->id, 1) < 0 ? -1 : 0;
}
static int d_sem_post(d_sem *s) {
    if (!s || s == (d_sem *)-1)
        return -1;
    return sceKernelSignalSema(s->id, 1) < 0 ? -1 : 0;
}
// Semáforos Mach (semaphore_create(task, &sem, policy, value)).
static int d_semaphore_create(int task, SceUID *out, int policy, int value) {
    *out = sceKernelCreateSema("ml_msem", 0, value, 0x7FFFFFFF, NULL);
    return *out < 0;
}
static int d_semaphore_signal(SceUID s) { return sceKernelSignalSema(s, 1) < 0; }
static int d_semaphore_wait(SceUID s) { return sceKernelWaitSema(s, 1, NULL) < 0; }
static int d_semaphore_destroy(int task, SceUID s) { return sceKernelDeleteSema(s) < 0; }

// ---------------------------------------------------------------- C++ / excepciones / bloques

static int d___cxa_atexit(void *f, void *a, void *d) { return 0; } // el juego nunca termina "limpio"
static int d___cxa_guard_acquire(uint8_t *g) { return *g == 0; }   // guarda de 64 bits de Apple
static void d___cxa_guard_release(uint8_t *g) { *g = 1; }
static void d___cxa_guard_abort(uint8_t *g) {}
static void d___cxa_pure_virtual(void) { fatal_error("__cxa_pure_virtual"); }
static void d_sjlj_noop(void *ctx) {}
static void d_sjlj_resume(void *ctx) { fatal_error("excepción C++/ObjC lanzada (_Unwind_SjLj_Resume)"); }
static void *d___cxa_allocate_exception(size_t n) { return calloc(1, n + 128); }
// operator new en cero: en iOS los bloques grandes salen de páginas nuevas (ya en cero) y los juegos dependen de
// eso (Real Soccer: CAnimationManager, 238 KB, nunca inicializa +0x39e50 y lo borra con delete virtual).
static void *d_new(size_t n) {
    uintptr_t r4;
    __asm__ volatile("mov %0, r4" : "=r"(r4)); // si entró por ml_align_call, r4 = su marco (lr del juego en +28)
    void *p = calloc(1, n ? n : 1);
    if (!p) {
        uintptr_t lr = (uintptr_t)__builtin_return_address(0);
        if (!is_guest_addr(lr))
            lr = ((uint32_t *)r4)[7];
        fatal_error("operator new(%u): sin memoria (llamado desde 0x%08X del Mach-O)", (unsigned)n,
                    (unsigned)(lr - MACHO_SLIDE));
    }
    return p;
}
static void d___cxa_throw(void *obj, void *tinfo, void *dtor) {
    fatal_error("throw de C++ del juego (SjLj de iOS no soportado): typeinfo 0x%08X",
                (unsigned)((uintptr_t)tinfo - MACHO_SLIDE));
}
static void *d___cxa_begin_catch(void *e) { return e; }
static void d_noop(void) {}


// ---------------------------------------------------------------- libgcc / compiler-rt

// Con float/double: la Vita también es softfp, así que estas funciones reciben y devuelven en
// r0-r3 igual que iOS. Las comparaciones *vfp devuelven 1 si la relación es verdadera.
static float f_add(float a, float b) { return a + b; }
static float f_sub(float a, float b) { return a - b; }
static float f_mul(float a, float b) { return a * b; }
static float f_div(float a, float b) { return a / b; }
static double dd_add(double a, double b) { return a + b; }
static double dd_sub(double a, double b) { return a - b; }
static double dd_mul(double a, double b) { return a * b; }
static double dd_div(double a, double b) { return a / b; }
static int f_eq(float a, float b) { return a == b; }
static int f_ne(float a, float b) { return !(a == b); }
static int f_gt(float a, float b) { return a > b; }
static int f_ge(float a, float b) { return a >= b; }
static int f_lt(float a, float b) { return a < b; }
static int f_le(float a, float b) { return a <= b; }
static int f_un(float a, float b) { return a != a || b != b; }
static int dd_eq(double a, double b) { return a == b; }
static int dd_ne(double a, double b) { return !(a == b); }
static int dd_gt(double a, double b) { return a > b; }
static int dd_ge(double a, double b) { return a >= b; }
static int dd_lt(double a, double b) { return a < b; }
static int dd_le(double a, double b) { return a <= b; }
static int dd_un(double a, double b) { return a != a || b != b; }
static double f_ext(float a) { return a; }
static float d_trunc(double a) { return (float)a; }
static int f_toi(float a) { return (int)a; }
static int d_toi(double a) { return (int)a; }
static unsigned f_tou(float a) { return (unsigned)a; }
static unsigned d_tou(double a) { return (unsigned)a; }
static long long f_toll(float a) { return (long long)a; }
static long long d_toll(double a) { return (long long)a; }
static unsigned long long f_toull(float a) { return (unsigned long long)a; }
static unsigned long long d_toull(double a) { return (unsigned long long)a; }
static float i_tof(int a) { return (float)a; }
static float u_tof(unsigned a) { return (float)a; }
static double i_tod(int a) { return (double)a; }
static double u_tod(unsigned a) { return (double)a; }
static double ll_tod(long long a) { return (double)a; }
static double ull_tod(unsigned long long a) { return (double)a; }
static float ll_tof(long long a) { return (float)a; }
static float ull_tof(unsigned long long a) { return (float)a; }
static float f_neg(float a) { return -a; }
static double d_neg(double a) { return -a; }

static int i_div(int a, int b) { return b ? a / b : 0; }
static int i_mod(int a, int b) { return b ? a % b : 0; }
static unsigned u_div(unsigned a, unsigned b) { return b ? a / b : 0; }
static unsigned u_mod(unsigned a, unsigned b) { return b ? a % b : 0; }
static long long ll_div(long long a, long long b) { return b ? a / b : 0; }
static long long ll_mod(long long a, long long b) { return b ? a % b : 0; }
static long long ll_mul(long long a, long long b) { return a * b; }
static unsigned long long ull_div(unsigned long long a, unsigned long long b) { return b ? a / b : 0; }
static unsigned long long ull_mod(unsigned long long a, unsigned long long b) { return b ? a % b : 0; }
static int ll_cmp(long long a, long long b) { return a < b ? 0 : a == b ? 1 : 2; }
static int ull_cmp(unsigned long long a, unsigned long long b) { return a < b ? 0 : a == b ? 1 : 2; }
static long long ll_ashr(long long a, int b) { return a >> b; }
static long long ll_lshl(long long a, int b) { return a << b; }
static unsigned long long ll_lshr(unsigned long long a, int b) { return a >> b; }

// lrintf/lrint de newlib = mismo ABI. ldexp(double, int): double r0:r1, int r2: igual en los dos.
static float m_fmaxf(float a, float b) { return a > b ? a : b; }
static float m_fminf(float a, float b) { return a < b ? a : b; }

static void d_CC_MD5(const void *data, uint32_t len, uint8_t *md) {
    // ponytail: no hay MD5 real (lo usan los SDK de publicidad/IGP para IDs); un hash estable alcanza.
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < len; i++)
        h = (h ^ ((const uint8_t *)data)[i]) * 16777619u;
    for (int i = 0; i < 16; i++)
        md[i] = h >> ((i & 3) * 8);
}

// setjmp/longjmp en asm.S (sobre el jmp_buf de Darwin, sin pasar por ml_align_call)
extern int ml_setjmp(void *);
extern void ml_longjmp(void *, int);

// ---------------------------------------------------------------- tabla

const import_entry darwin_imports[] = {
    // memoria / cadenas
    D(malloc), D(calloc), D(realloc), D(free), D(memcpy), D(memmove), D(memset), D(memcmp), D(memchr),
    D(memccpy), D(bzero), D(strlen), D(strnlen), D(strcmp), D(strncmp), D(strcpy), D(strncpy), D(strcat),
    D(strncat), D(strchr), D(strrchr), D(strstr), D(strtok), D(strtok_r), D(strdup), D(strcasecmp),
    D(strncasecmp), D(strlcpy), D(strlcat), D(strspn), D(strcspn), D(strpbrk), D(strerror), D(atoi), D(atol),
    D(atoll), D(atof), D(strtol), D(strtoul), D(strtoll), D(strtoull), D(strtod), D(strtof), D(rand), D(srand),
    D(random), D(srandom), D(abs), D(labs), D(wcslen), D(wcscpy), D(wcsncpy), D(wcscmp), D(wcsncmp), D(wcscat),
    D(wcschr), D(mbstowcs), D(wcstombs), D(towlower), D(towupper), D(tolower), D(toupper), D(isalpha), D(isdigit),
    D(isspace), D(isupper), D(islower), D(isalnum), D(isprint), D(ispunct), D(isxdigit), D(sscanf),
    F(sprintf, d_sprintf), F(snprintf, d_snprintf), F(vsprintf, d_vsprintf), F(vsnprintf, d_vsnprintf),
    F(swprintf, d_swprintf), F(qsort, d_qsort), D(bsearch), F(__memcpy_chk, d___memcpy_chk),
    F(__memmove_chk, d___memmove_chk), F(__memset_chk, d___memset_chk), F(__strcpy_chk, d___strcpy_chk),
    F(__strcat_chk, d___strcat_chk), F(__strncpy_chk, d___strncpy_chk), F(__sprintf_chk, d___sprintf_chk),
    F(__snprintf_chk, d___snprintf_chk), F(__assert_rtn, d___assert_rtn), F(__error, d___error),
    F(abort, d_abort), V(__stack_chk_guard, &d_stack_chk_guard), F(__stack_chk_fail, d___stack_chk_fail),
    V(_DefaultRuneLocale, &rune_locale), F(__maskrune, d___maskrune), F(__tolower, d___tolower),
    F(__toupper, d___toupper),
    // stdio / archivos
    F(fopen, d_fopen), F(freopen, d_freopen), D(fclose), D(fread), F(fwrite, d_fwrite), D(fseek), D(ftell),
    D(feof), D(ferror), D(fflush), F(fputs, d_fputs), D(fgets), D(fgetc), D(getc), D(fputc), D(putc), D(ungetc),
    D(fscanf), D(rewind), D(clearerr), D(fileno), D(setvbuf), D(tmpfile), F(fprintf, d_fprintf),
    F(vfprintf, d_vfprintf), F(printf, d_printf), F(vprintf, d_vprintf), F(puts, d_puts), F(putchar, d_putchar),
    F(perror, d_puts), F(mkdir, d_mkdir), F(remove, d_remove), F(unlink, d_unlink), F(rmdir, d_rmdir),
    F(rename, d_rename), F(chdir, d_chdir), D(getcwd), F(access, d_access), F(realpath, d_realpath),
    F(realpath$DARWIN_EXTSN, d_realpath), F(stat, d_stat), F(lstat, d_stat), F(fstat, d_fstat),
    F(opendir, d_opendir), F(readdir, d_readdir), F(closedir, d_closedir), F(open, d_open), D(close), D(read),
    D(write), D(lseek), V(__stderrp, &d_stderrp), V(__stdoutp, &d_stdoutp), V(__stdinp, &d_stdinp),
    F(getenv, d_getenv),
    // tiempo / sistema
    D(gettimeofday), D(time), D(mktime), D(difftime), D(strftime), F(localtime, d_localtime),
    F(gmtime, d_gmtime), F(localtime_r, d_localtime_r), F(clock, d_clock), F(usleep, d_usleep),
    F(nanosleep, d_nanosleep), F(sched_yield, d_sched_yield), F(mach_absolute_time, d_mach_absolute_time),
    F(mach_timebase_info, d_mach_timebase_info), F(sysctlbyname, d_sysctlbyname), F(sysctl, d_sysctl),
    F(uname, d_uname), F(gethostname, d_gethostname), F(exit, d_exit), F(_exit, d_exit), F(atexit, d_zero),
    F(signal, d_null), F(sigaction, d_zero), F(raise, d_zero), F(getpid, d_zero), F(mach_host_self, d_zero),
    F(mach_task_self, d_zero), F(host_page_size, d_host_page_size), F(host_statistics, d_host_statistics),
    F(thread_info, d_minus1), F(thread_policy_set, d_zero), F(task_info, d_minus1), F(vm_deallocate, d_zero),
    F(CC_MD5, d_CC_MD5), V(setjmp, ml_setjmp), V(longjmp, ml_longjmp), V(_setjmp, ml_setjmp),
    V(_longjmp, ml_longjmp),
    // red: el juego queda offline
    F(socket, d_net_fail), F(connect, d_net_fail), F(bind, d_net_fail), F(listen, d_net_fail),
    F(accept, d_net_fail), F(send, d_net_fail), F(sendto, d_net_fail), F(recv, d_net_fail),
    F(recvfrom, d_net_fail), F(setsockopt, d_net_fail), F(getsockopt, d_net_fail), F(getsockname, d_net_fail),
    F(getpeername, d_net_fail), F(shutdown, d_net_fail), F(fcntl, d_net_fail), F(ioctl, d_net_fail),
    F(poll, d_zero), F(inet_addr, d_minus1), F(inet_aton, d_zero), F(inet_ntoa, d_null), F(inet_ntop, d_null),
    F(inet_pton, d_zero), F(gethostbyname, d_null), F(getprotobyname, d_null), F(getaddrinfo, d_getaddrinfo),
    F(freeaddrinfo, d_noop), F(select, d_select), F(getifaddrs, d_minus1), F(freeifaddrs, d_noop),
    // pthread / semáforos
    F(pthread_create, d_pthread_create), F(pthread_detach, d_pthread_detach), D(pthread_join),
    F(pthread_self, d_pthread_self), F(pthread_equal, d_pthread_equal), F(pthread_setschedparam, d_pthread_setschedparam),
    F(pthread_getschedparam, d_pthread_getschedparam), F(pthread_mach_thread_np, d_pthread_mach_thread_np),
    F(pthread_cancel, d_zero), F(pthread_testcancel, d_noop), F(pthread_setcanceltype, d_zero),
    F(pthread_setcancelstate, d_zero), F(pthread_attr_init, d_pthread_attr_init), F(pthread_attr_destroy, d_zero),
    F(pthread_attr_setstacksize, d_pthread_attr_setstacksize), F(pthread_attr_getstacksize, d_pthread_attr_getstacksize),
    F(pthread_attr_setdetachstate, d_pthread_attr_setdetachstate), F(pthread_attr_setschedparam, d_zero),
    F(pthread_attr_getschedparam, d_pthread_attr_getschedparam), F(pthread_attr_setschedpolicy, d_zero),
    F(pthread_mutex_init, d_pthread_mutex_init), F(pthread_mutex_lock, d_pthread_mutex_lock),
    F(pthread_mutex_trylock, d_pthread_mutex_trylock), F(pthread_mutex_unlock, d_pthread_mutex_unlock),
    F(pthread_mutex_destroy, d_pthread_mutex_destroy), F(pthread_mutexattr_init, d_pthread_mutexattr_init),
    F(pthread_mutexattr_settype, d_pthread_mutexattr_settype), F(pthread_mutexattr_destroy, d_zero),
    F(pthread_cond_init, d_pthread_cond_init), F(pthread_cond_wait, d_pthread_cond_wait),
    F(pthread_cond_timedwait, d_pthread_cond_timedwait), F(pthread_cond_signal, d_pthread_cond_signal),
    F(pthread_cond_broadcast, d_pthread_cond_broadcast), F(pthread_cond_destroy, d_pthread_cond_destroy),
    F(pthread_once, d_pthread_once), D(pthread_key_create), D(pthread_key_delete), D(pthread_getspecific),
    D(pthread_setspecific), F(sem_open, d_sem_open), F(sem_close, d_sem_close), F(sem_unlink, d_sem_unlink),
    F(sem_wait, d_sem_wait), F(sem_trywait, d_sem_trywait), F(sem_post, d_sem_post),
    F(semaphore_create, d_semaphore_create), F(semaphore_signal, d_semaphore_signal),
    F(semaphore_wait, d_semaphore_wait), F(semaphore_destroy, d_semaphore_destroy),
    // C++ / bloques / excepciones / GCD
    F(__cxa_atexit, d___cxa_atexit), F(__cxa_guard_acquire, d___cxa_guard_acquire),
    F(__cxa_guard_release, d___cxa_guard_release), F(__cxa_guard_abort, d___cxa_guard_abort),
    F(__cxa_pure_virtual, d___cxa_pure_virtual), F(__cxa_allocate_exception, d___cxa_allocate_exception),
    F(_Znwm, d_new), F(_Znam, d_new),
    F(__cxa_throw, d___cxa_throw), F(__cxa_rethrow, d_sjlj_resume), F(__cxa_begin_catch, d___cxa_begin_catch),
    F(__cxa_end_catch, d_noop), F(__cxa_free_exception, free), F(__gxx_personality_sj0, d_noop),
    D(__dynamic_cast), F(_Unwind_SjLj_Register, d_sjlj_noop), F(_Unwind_SjLj_Unregister, d_sjlj_noop),
    F(_Unwind_SjLj_Resume, d_sjlj_resume), F(_Unwind_SjLj_RaiseException, d_sjlj_resume),
    // compiler-rt / libgcc (softfp = mismo ABI)
    F(__addsf3vfp, f_add), F(__subsf3vfp, f_sub), F(__mulsf3vfp, f_mul), F(__divsf3vfp, f_div),
    F(__adddf3vfp, dd_add), F(__subdf3vfp, dd_sub), F(__muldf3vfp, dd_mul), F(__divdf3vfp, dd_div),
    F(__addsf3, f_add), F(__subsf3, f_sub), F(__mulsf3, f_mul), F(__divsf3, f_div), F(__adddf3, dd_add),
    F(__subdf3, dd_sub), F(__muldf3, dd_mul), F(__divdf3, dd_div), F(__negsf2, f_neg), F(__negdf2, d_neg),
    F(__eqsf2vfp, f_eq), F(__nesf2vfp, f_ne), F(__gtsf2vfp, f_gt), F(__gesf2vfp, f_ge), F(__ltsf2vfp, f_lt),
    F(__lesf2vfp, f_le), F(__unordsf2vfp, f_un), F(__eqdf2vfp, dd_eq), F(__nedf2vfp, dd_ne), F(__gtdf2vfp, dd_gt),
    F(__gedf2vfp, dd_ge), F(__ltdf2vfp, dd_lt), F(__ledf2vfp, dd_le), F(__unorddf2vfp, dd_un),
    F(__extendsfdf2vfp, f_ext), F(__extendsfdf2, f_ext), F(__truncdfsf2vfp, d_trunc), F(__truncdfsf2, d_trunc),
    F(__fixsfsivfp, f_toi), F(__fixdfsivfp, d_toi), F(__fixsfsi, f_toi), F(__fixdfsi, d_toi),
    F(__fixunssfsivfp, f_tou), F(__fixunsdfsivfp, d_tou), F(__fixunssfsi, f_tou), F(__fixunsdfsi, d_tou),
    F(__fixsfdi, f_toll), F(__fixdfdi, d_toll), F(__fixunssfdi, f_toull), F(__fixunsdfdi, d_toull),
    F(__floatsisfvfp, i_tof), F(__floatunssisfvfp, u_tof), F(__floatsidfvfp, i_tod), F(__floatunssidfvfp, u_tod),
    F(__floatsisf, i_tof), F(__floatunsisf, u_tof), F(__floatsidf, i_tod), F(__floatunsidf, u_tod),
    F(__floatdidf, ll_tod), F(__floatundidf, ull_tod), F(__floatdisf, ll_tof), F(__floatundisf, ull_tof),
    F(__divsi3, i_div), F(__modsi3, i_mod), F(__udivsi3, u_div), F(__umodsi3, u_mod), F(__divdi3, ll_div),
    F(__moddi3, ll_mod), F(__muldi3, ll_mul), F(__udivdi3, ull_div), F(__umoddi3, ull_mod), F(__cmpdi2, ll_cmp),
    F(__ucmpdi2, ull_cmp), F(__ashrdi3, ll_ashr), F(__ashldi3, ll_lshl), F(__lshrdi3, ll_lshr),
    // math (softfp en los dos lados: directo)
    D(acosf), D(asinf), D(atanf), D(atan2f), D(cosf), D(sinf), D(tanf), D(expf), D(logf), D(log10f), D(powf),
    D(sqrtf), D(floorf), D(ceilf), D(fabsf), D(fmodf), D(roundf), D(lrintf), D(rintf), D(truncf), D(coshf),
    D(sinhf), D(tanhf), D(acos), D(asin), D(atan), D(atan2), D(cos), D(sin), D(tan), D(exp), D(log), D(log2),
    D(log10), D(pow), D(sqrt), D(floor), D(ceil), D(fabs), D(fmod), D(round), D(lrint), D(rint), D(trunc),
    D(ldexp), D(frexp), D(modf), D(modff), D(fmax), D(fmin), D(hypot), D(hypotf), D(cosh), D(sinh), D(tanh),
    D(log2f), D(exp2f), D(cbrtf), D(llrint), D(lround), D(lroundf), F(fmaxf, m_fmaxf), F(fminf, m_fminf),
};
TABLE_END(darwin)

void darwin_init(void) {
    d_stderrp = stderr;
    d_stdoutp = stdout;
    d_stdinp = stdin;
    rune_init();
}
