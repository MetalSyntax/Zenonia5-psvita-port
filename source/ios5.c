// Runtime de iOS 4/5 que los juegos de iPhone OS 2/3 no usan: bloques, GCD, ARC, OSAtomic, funciones
// de introspección del runtime ObjC, sqlite (SceSqlite), ExtAudioFile y stubs de lo que no tiene
// sentido en la Vita (reportero de crashes con puertos Mach, Keychain, SSL).
#include <dirent.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#include "foundation.h"
#include "hle.h"
#include "imports.h"
#include "log.h"
#include "macho.h"
#include "objc.h"

// ================================================================ bloques (ABI de Apple)

typedef struct {
    uint32_t reserved, size;
    void *copy, *dispose; // si BLOCK_HAS_COPY_DISPOSE
} block_desc;
typedef struct {
    void *isa;
    volatile int32_t flags;
    int32_t reserved;
    void *invoke;
    block_desc *desc;
} block;
#define BLOCK_REFCOUNT_MASK 0xFFFE
#define BLOCK_NEEDS_FREE (1 << 24)
#define BLOCK_HAS_COPY_DISPOSE (1 << 25)
#define BLOCK_IS_GLOBAL (1 << 28)

static objc_class *c_stack_block, *c_malloc_block, *c_global_block;

void *ml_Block_copy(block *b) {
    if (!b)
        return NULL;
    if (b->flags & BLOCK_IS_GLOBAL)
        return b;
    if (b->flags & BLOCK_NEEDS_FREE) {
        __sync_fetch_and_add(&b->flags, 2);
        return b;
    }
    block *n = malloc(b->desc->size);
    memcpy(n, b, b->desc->size);
    n->isa = c_malloc_block;
    n->flags = (b->flags & ~BLOCK_REFCOUNT_MASK) | BLOCK_NEEDS_FREE | 2;
    if (b->flags & BLOCK_HAS_COPY_DISPOSE)
        call_guest(b->desc->copy, (uint32_t)n, (uint32_t)b, 0, 0);
    return n;
}
void ml_Block_release(block *b) {
    if (!b || !(b->flags & BLOCK_NEEDS_FREE))
        return;
    if ((__sync_sub_and_fetch(&b->flags, 2) & BLOCK_REFCOUNT_MASK) == 0) {
        if (b->flags & BLOCK_HAS_COPY_DISPOSE)
            call_guest(b->desc->dispose, (uint32_t)b, 0, 0, 0);
        free(b);
    }
}
// Variables __block: {isa, forwarding, flags, size, keep, destroy, ...}
typedef struct byref {
    void *isa;
    struct byref *forwarding;
    volatile int32_t flags, size;
    void *keep, *destroy;
} byref;
static void Block_object_assign(void **dst, void *src, int flags) {
    switch (flags & 0x1F) {
    case 3: // BLOCK_FIELD_IS_OBJECT
        *dst = objc_retain(src);
        break;
    case 7: // BLOCK_FIELD_IS_BLOCK
        *dst = ml_Block_copy(src);
        break;
    case 8:
    case 24: { // BLOCK_FIELD_IS_BYREF (| WEAK)
        byref *s = src;
        if ((s->forwarding->flags & BLOCK_NEEDS_FREE) == 0) {
            byref *c = malloc(s->size);
            memcpy(c, s, s->size);
            c->flags = s->flags | BLOCK_NEEDS_FREE | 4;
            c->forwarding = c;
            s->forwarding = c;
            if (s->flags & BLOCK_HAS_COPY_DISPOSE)
                call_guest(s->keep, (uint32_t)c, (uint32_t)s, 0, 0);
        } else {
            __sync_fetch_and_add(&s->forwarding->flags, 2);
        }
        *dst = s->forwarding;
        break;
    }
    default:
        *dst = src;
    }
}
static void Block_object_dispose(void *obj, int flags) {
    if ((flags & 0x1F) == 3)
        objc_release(obj);
    else if ((flags & 0x1F) == 7)
        ml_Block_release(obj);
}
static id blk_copy(id self, SEL _c) { return ml_Block_copy(self); }
static void blk_release(id self, SEL _c) { ml_Block_release(self); }
static id blk_self(id self, SEL _c) { return self; }
static const nmethod block_i[] = {
    NM("copy", blk_copy), NM("retain", blk_copy), NM("release", blk_release), NM("autorelease", blk_self), {0}};

// invocar un bloque del juego desde nuestro C
static void block_invoke(block *b, uint32_t arg) { call_guest(b->invoke, (uint32_t)b, arg, 0, 0); }

// ================================================================ GCD

typedef struct dqueue_s {
    uint32_t magic; // 'DQUE'
    int main, serial;
    char label[32];
    pthread_mutex_t lock;
    pthread_cond_t cond;
    struct job *head, *tail;
    int worker;
} dqueue;
typedef struct job {
    struct job *next;
    void *fn, *ctx; // fn != NULL: función C (dispatch_*_f); si no, ctx es un bloque
    double when;
    struct dqueue_s *q; // dispatch_after: cola destino
} job;
static dqueue main_q = {0x44515545, 1, 1, "com.apple.main-thread", PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER};
static dqueue global_q = {0x44515545, 0, 0, "global", PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER};
static job *delayed;
static pthread_mutex_t delayed_lock = PTHREAD_MUTEX_INITIALIZER;

static double now_s(void) { return sceKernelGetProcessTimeWide() / 1e6; }
static void run_job(job *j) {
    objc_pool_push();
    if (j->fn)
        call_guest(j->fn, (uint32_t)j->ctx, 0, 0, 0);
    else {
        block_invoke(j->ctx, 0);
        ml_Block_release(j->ctx);
    }
    objc_pool_pop();
    free(j);
}
static void *worker(void *p) {
    dqueue *q = p;
    for (;;) {
        pthread_mutex_lock(&q->lock);
        while (!q->head)
            pthread_cond_wait(&q->cond, &q->lock);
        job *j = q->head;
        q->head = j->next;
        if (!q->head)
            q->tail = NULL;
        pthread_mutex_unlock(&q->lock);
        run_job(j);
    }
    return NULL;
}
static void enqueue(dqueue *q, job *j) {
    if (!q || q->magic != 0x44515545)
        q = &global_q;
    j->next = NULL;
    pthread_mutex_lock(&q->lock);
    if (q->tail)
        q->tail->next = j;
    else
        q->head = j;
    q->tail = j;
    if (!q->main && !q->worker) {
        q->worker = 1;
        pthread_t t;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 512 * 1024);
        pthread_create(&t, &a, worker, q);
        pthread_attr_destroy(&a);
        pthread_detach(t);
    }
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->lock);
}
static job *mkjob(void *fn, void *ctx) {
    job *j = calloc(1, sizeof(*j));
    j->fn = fn;
    j->ctx = fn ? ctx : ml_Block_copy(ctx);
    return j;
}
// Lo llama el run loop principal (uikit.c): trabajos de la cola principal y dispatch_after vencidos.
void gcd_main_tick(void) {
    double t = now_s();
    pthread_mutex_lock(&delayed_lock);
    job **pp = &delayed;
    job *due = NULL;
    while (*pp) {
        job *j = *pp;
        if (j->when <= t) {
            *pp = j->next;
            j->next = due;
            due = j;
        } else {
            pp = &j->next;
        }
    }
    pthread_mutex_unlock(&delayed_lock);
    while (due) {
        job *j = due;
        due = j->next;
        enqueue(j->q ? (dqueue *)j->q : &main_q, j);
    }
    for (int n = 0; n < 64; n++) {
        pthread_mutex_lock(&main_q.lock);
        job *j = main_q.head;
        if (j) {
            main_q.head = j->next;
            if (!main_q.head)
                main_q.tail = NULL;
        }
        pthread_mutex_unlock(&main_q.lock);
        if (!j)
            break;
        run_job(j);
    }
}

static dqueue *d_get_global_queue(long prio, unsigned long flags) { return &global_q; }
static dqueue *d_get_main_queue(void) { return &main_q; }
static dqueue *d_queue_create(const char *label, void *attr) {
    dqueue *q = calloc(1, sizeof(*q));
    q->magic = 0x44515545;
    q->serial = 1;
    snprintf(q->label, sizeof(q->label), "%s", label ? label : "");
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->cond, NULL);
    return q;
}
static void d_async(dqueue *q, void *blk) { enqueue(q, mkjob(NULL, blk)); }
static void d_async_f(dqueue *q, void *ctx, void *fn) { enqueue(q, mkjob(fn, ctx)); }
static void d_sync(dqueue *q, block *blk) { block_invoke(blk, 0); } // ponytail: sin exclusión real
static void d_sync_f(dqueue *q, void *ctx, void *fn) { call_guest(fn, (uint32_t)ctx, 0, 0, 0); }
// dispatch_time_t = ns (uint64). dispatch_after(when, queue, block): when en r0:r1
static uint64_t d_time(uint32_t when_lo, uint32_t when_hi, uint32_t delta_lo, uint32_t delta_hi) {
    uint64_t when = (uint64_t)when_hi << 32 | when_lo;
    int64_t delta = (int64_t)((uint64_t)delta_hi << 32 | delta_lo);
    uint64_t base = when == 0 ? sceKernelGetProcessTimeWide() * 1000ULL : when;
    return base + delta;
}
static void d_after(uint32_t when_lo, uint32_t when_hi, dqueue *q, void *blk) {
    uint64_t when = (uint64_t)when_hi << 32 | when_lo;
    job *j = mkjob(NULL, blk);
    j->when = when == ~0ULL ? 1e18 : when / 1e9;
    j->q = (struct dqueue_s *)q;
    pthread_mutex_lock(&delayed_lock);
    j->next = delayed;
    delayed = j;
    pthread_mutex_unlock(&delayed_lock);
}
static void d_once(long *pred, block *blk) {
    static pthread_mutex_t once_lock = PTHREAD_MUTEX_INITIALIZER;
    if (*pred == ~0L)
        return;
    pthread_mutex_lock(&once_lock);
    if (*pred != ~0L) {
        block_invoke(blk, 0);
        *pred = ~0L;
    }
    pthread_mutex_unlock(&once_lock);
}
static void d_once_f(long *pred, void *ctx, void *fn) {
    if (*pred != ~0L) {
        *pred = ~0L;
        call_guest(fn, (uint32_t)ctx, 0, 0, 0);
    }
}
static SceUID d_sem_create(long value) { return sceKernelCreateSema("ml_dsem", 0, value, 0x7FFFFFFF, NULL); }
// dispatch_semaphore_wait(sema, dispatch_time_t timeout): Darwin pasa el uint64 en r1:r2
static long d_sem_wait(SceUID s, uint32_t lo, uint32_t hi) {
    uint64_t t = (uint64_t)hi << 32 | lo;
    if (t == ~0ULL)
        return sceKernelWaitSema(s, 1, NULL) < 0;
    uint64_t now = sceKernelGetProcessTimeWide() * 1000ULL;
    SceUInt us = t > now ? (t - now) / 1000 : 0;
    return sceKernelWaitSema(s, 1, &us) < 0;
}
static long d_sem_signal(SceUID s) { return sceKernelSignalSema(s, 1) >= 0; }
static void d_noop(void) {}

// ================================================================ ARC y runtime

static pthread_mutex_t sync_lock;
static int sync_init;
static int r_sync_enter(id o) {
    pthread_mutex_lock(&sync_lock);
    return 0;
}
static int r_sync_exit(id o) {
    pthread_mutex_unlock(&sync_lock);
    return 0;
}
static void *r_pool_push(void) {
    objc_pool_push();
    return (void *)1;
}
static void r_pool_pop(void *t) { objc_pool_pop(); }
static id r_retainAutorelease(id o) { return objc_autorelease(objc_retain(o)); }
static id r_retainAutoreleasedReturnValue(id o) { return objc_retain(o); }
static id r_autoreleaseReturnValue(id o) { return objc_autorelease(o); }
static void r_storeStrong(id *slot, id v) {
    id old = *slot;
    *slot = objc_retain(v);
    objc_release(old);
}
static id r_retainBlock(id b) { return ml_Block_copy(b); }
static void r_terminate(void) { fatal_error("objc_terminate"); }
static objc_class *r_class_getSuperclass(objc_class *c) { return c ? c->superclass : NULL; }
static size_t r_class_getInstanceSize(objc_class *c) { return c ? ((class_ro *)((uintptr_t)c->ro & ~3))->instanceSize : 0; }
static void *r_class_getInstanceMethod(objc_class *c, SEL s) {
    id fake = &c;
    return objc_responds(fake, sel_registerName(s)) ? (void *)1 : NULL;
}
static int r_class_addMethod(objc_class *c, SEL s, void *imp, const char *types) {
    l_error("class_addMethod(%s, %s): sin soporte", class_getName(c), s);
    return 0;
}
static void *r_null(void) { return NULL; }
static int r_zero(void) { return 0; }
static int r_one(void) { return 1; }
static int r_minus1(void) { return -1; }

// ================================================================ libc / Darwin extra

static int32_t os_add32(int32_t v, volatile int32_t *p) { return __sync_add_and_fetch(p, v); }
static int os_cas32(int32_t o, int32_t n, volatile int32_t *p) { return __sync_bool_compare_and_swap(p, o, n); }
static int os_casptr(void *o, void *n, void *volatile *p) { return __sync_bool_compare_and_swap(p, o, n); }
static void os_barrier(void) { __sync_synchronize(); }
static void os_spin_lock(volatile int32_t *l) {
    while (!__sync_bool_compare_and_swap(l, 0, 1))
        sceKernelDelayThread(0);
}
static void os_spin_unlock(volatile int32_t *l) {
    __sync_synchronize();
    *l = 0;
}
static uint32_t d_arc4random(void) { return (uint32_t)rand() ^ ((uint32_t)rand() << 16); }
static char *d_basename(char *p) {
    char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}
static unsigned d_sleep(unsigned s) {
    sceKernelDelayThread(s * 1000000);
    return 0;
}
static int d_strerror_r(int e, char *buf, size_t n) {
    snprintf(buf, n, "error %d", e);
    return 0;
}
static const char *d_getprogname(void) { return PORT_EXEC; }
static void d_memset_pattern16(void *b, const void *pat, size_t len) {
    for (size_t i = 0; i < len; i++)
        ((uint8_t *)b)[i] = ((const uint8_t *)pat)[i & 15];
}
static int d_NSGetExecutablePath(char *buf, uint32_t *size) {
    snprintf(buf, *size, BUNDLE_PATH PORT_EXEC);
    return 0;
}
static double d_CACurrentMediaTime(void) { return now_s(); }
static void d_hash_fill(const void *d, uint32_t len, uint8_t *md, int n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < len; i++)
        h = (h ^ ((const uint8_t *)d)[i]) * 16777619u;
    for (int i = 0; i < n; i++)
        md[i] = h >> ((i & 3) * 8);
}
static void *d_CC_SHA1(const void *d, uint32_t len, uint8_t *md) {
    d_hash_fill(d, len, md, 20);
    return md;
}
static int d_CCCrypt(void) { return -4300; } // kCCParamError

// sqlite3: la Vita trae SQLite (SceSqlite) con la misma API.
extern int sqlite3_open_v2(const char *, void **, int, const char *);
extern int sqlite3_close(void *), sqlite3_prepare_v2(void *, const char *, int, void **, const char **),
    sqlite3_step(void *), sqlite3_finalize(void *), sqlite3_bind_blob(void *, int, const void *, int, void *),
    sqlite3_bind_text(void *, int, const char *, int, void *), sqlite3_column_bytes(void *, int),
    sqlite3_column_count(void *);
extern const void *sqlite3_column_blob(void *, int);
extern const unsigned char *sqlite3_column_text(void *, int);
extern const char *sqlite3_errmsg(void *);
extern void sqlite3_free(void *);
extern void *sqlite3_malloc(int);
static int d_sqlite3_open_v2(const char *path, void **db, int flags, const char *vfs) {
    char p[512];
    extern const char *darwin_path(const char *in, char *out, size_t outlen);
    int r = sqlite3_open_v2(darwin_path(path, p, sizeof(p)), db, flags, NULL);
    l_info("sqlite3_open_v2(%s) -> %d", p, r);
    return r;
}

// ================================================================ ExtAudioFile (sobre AudioFile de audio.c)

typedef struct {
    double mSampleRate;
    uint32_t mFormatID, mFormatFlags, mBytesPerPacket, mFramesPerPacket, mBytesPerFrame, mChannelsPerFrame,
        mBitsPerChannel, mReserved;
} asbd;
typedef struct {
    void *af; // audio_file de audio.c
    asbd file_fmt, client_fmt;
    int64_t pos;
} ext_file;
extern int af_open_path(const char *path, void **out);
extern int af_close(void *a);
extern int af_format(void *a, asbd *out, int64_t *frames);
extern long af_read_frames(void *a, int64_t start, long n, int16_t *out);
typedef struct {
    uint32_t mNumberBuffers;
    struct {
        uint32_t mNumberChannels, mDataByteSize;
        void *mData;
    } b[1];
} buffer_list;

static int ext_OpenURL(id url, ext_file **out) {
    void *a = NULL;
    int r = af_open_path(ns_cstr((id)MSG0(url, "path")), &a);
    if (r) {
        *out = NULL;
        return r;
    }
    ext_file *e = calloc(1, sizeof(*e));
    e->af = a;
    int64_t frames;
    af_format(a, &e->file_fmt, &frames);
    e->client_fmt = e->file_fmt;
    *out = e;
    return 0;
}
static int ext_Dispose(ext_file *e) {
    if (e) {
        af_close(e->af);
        free(e);
    }
    return 0;
}
static int ext_GetProperty(ext_file *e, uint32_t pid, uint32_t *size, void *data) {
    int64_t frames;
    switch (pid) {
    case 0x66666d74: // 'ffmt' FileDataFormat
        af_format(e->af, data, &frames);
        *size = sizeof(asbd);
        return 0;
    case 0x63666d74: // 'cfmt' ClientDataFormat
        memcpy(data, &e->client_fmt, sizeof(asbd));
        *size = sizeof(asbd);
        return 0;
    case 0x2366726d: // '#frm' FileLengthFrames (SInt64)
        af_format(e->af, &e->file_fmt, &frames);
        *(int64_t *)data = frames;
        *size = 8;
        return 0;
    }
    return 0x7479703f; // 'typ?'
}
static int ext_SetProperty(ext_file *e, uint32_t pid, uint32_t size, const void *data) {
    if (pid == 0x63666d74) { // ClientDataFormat: aceptamos s16 con la misma frecuencia/canales
        memcpy(&e->client_fmt, data, sizeof(asbd));
        if (e->client_fmt.mBitsPerChannel != 16 || e->client_fmt.mSampleRate != e->file_fmt.mSampleRate)
            l_error("ExtAudioFile: formato cliente %u bits %.0f Hz (archivo %.0f Hz) sin conversión",
                    (unsigned)e->client_fmt.mBitsPerChannel, e->client_fmt.mSampleRate, e->file_fmt.mSampleRate);
    }
    return 0;
}
static int ext_Read(ext_file *e, uint32_t *frames, buffer_list *bl) {
    int fch = e->file_fmt.mChannelsPerFrame, cch = e->client_fmt.mChannelsPerFrame ? e->client_fmt.mChannelsPerFrame : fch;
    int16_t *tmp = malloc(*frames * fch * 2);
    long got = af_read_frames(e->af, e->pos, *frames, tmp);
    int16_t *out = bl->b[0].mData;
    for (long i = 0; i < got; i++)
        for (int c = 0; c < cch; c++)
            out[i * cch + c] = tmp[i * fch + (c < fch ? c : 0)];
    free(tmp);
    e->pos += got;
    *frames = got;
    bl->b[0].mDataByteSize = got * cch * 2;
    return 0;
}

// ================================================================ tabla / init

static uint32_t k_vm_page_size = 4096, k_NDR_record[2], k_mach_task_self_;
static int32_t k_UIBackgroundTaskInvalid = -1;
static float k_UIWindowLevelNormal = 0;

const import_entry ios5_imports[] = {
    F(_Block_copy, ml_Block_copy), F(_Block_release, ml_Block_release), F(_Block_object_assign, Block_object_assign),
    F(_Block_object_dispose, Block_object_dispose), V(_dispatch_main_q, &main_q),
    F(dispatch_get_global_queue, d_get_global_queue), F(dispatch_get_main_queue, d_get_main_queue),
    F(dispatch_queue_create, d_queue_create), F(dispatch_async, d_async), F(dispatch_async_f, d_async_f),
    F(dispatch_sync, d_sync), F(dispatch_sync_f, d_sync_f), F(dispatch_barrier_async, d_async),
    F(dispatch_after, d_after), F(dispatch_time, d_time), F(dispatch_once, d_once), F(dispatch_once_f, d_once_f),
    F(dispatch_semaphore_create, d_sem_create), F(dispatch_semaphore_wait, d_sem_wait),
    F(dispatch_semaphore_signal, d_sem_signal), F(dispatch_release, d_noop), F(dispatch_retain, d_noop),
    F(dispatch_set_target_queue, d_noop), F(dispatch_suspend, d_noop), F(dispatch_resume, d_noop),
    F(objc_sync_enter, r_sync_enter), F(objc_sync_exit, r_sync_exit), F(objc_autoreleasePoolPush, r_pool_push),
    F(objc_autoreleasePoolPop, r_pool_pop), F(objc_retainAutorelease, r_retainAutorelease),
    F(objc_retainAutoreleasedReturnValue, r_retainAutoreleasedReturnValue),
    F(objc_autoreleaseReturnValue, r_autoreleaseReturnValue), F(objc_retainAutoreleaseReturnValue, r_retainAutorelease),
    F(objc_storeStrong, r_storeStrong), F(objc_retainBlock, r_retainBlock), F(objc_terminate, r_terminate),
    F(class_getSuperclass, r_class_getSuperclass), F(class_getInstanceSize, r_class_getInstanceSize),
    F(class_getInstanceMethod, r_class_getInstanceMethod), F(class_addMethod, r_class_addMethod),
    F(class_getInstanceVariable, r_null), F(class_getIvarLayout, r_null), F(ivar_getName, r_null),
    F(ivar_getOffset, r_zero), F(object_setIvar, d_noop), F(protocol_conformsToProtocol, r_one),
    F(protocol_copyMethodDescriptionList, r_null), F(protocol_copyProtocolList, r_null),
    F(protocol_getMethodDescription, r_null), F(OSAtomicAdd32, os_add32), F(OSAtomicAdd32Barrier, os_add32),
    F(OSAtomicCompareAndSwap32Barrier, os_cas32), F(OSAtomicCompareAndSwap32, os_cas32),
    F(OSAtomicCompareAndSwapPtrBarrier, os_casptr), F(OSAtomicCompareAndSwapIntBarrier, os_cas32),
    F(OSMemoryBarrier, os_barrier), F(OSSpinLockLock, os_spin_lock), F(OSSpinLockUnlock, os_spin_unlock),
    F(arc4random, d_arc4random), F(basename, d_basename), F(sleep, d_sleep), F(strerror_r, d_strerror_r),
    F(getprogname, d_getprogname), F(memset_pattern16, d_memset_pattern16), F(_NSGetExecutablePath, d_NSGetExecutablePath),
    F(CACurrentMediaTime, d_CACurrentMediaTime), F(CC_SHA1, d_CC_SHA1), F(CC_MD4, d_CC_SHA1), F(CC_MD5_Init, r_one),
    F(CC_MD5_Update, r_one), F(CC_MD5_Final, r_one), F(CCHmac, d_noop), F(CCCrypt, d_CCCrypt), D(gmtime_r), D(mkstemp),
    D(rewinddir), D(fsync), F(dup2, r_minus1), D(wcsncat), F(geteuid, r_zero), F(getpwuid, r_null), F(kill, r_minus1),
    F(fork, r_minus1), F(execl, r_minus1), F(waitpid, r_minus1), F(pthread_exit, d_noop), F(sigaltstack, r_zero),
    F(socketpair, r_minus1), F(if_nametoindex, r_zero), F(gai_strerror, r_null), F(setxattr, r_minus1),
    F(dladdr, r_zero), F(dlopen, r_null), F(dlsym, r_null), F(_dyld_register_func_for_add_image, d_noop),
    F(_dyld_register_func_for_remove_image, d_noop), F(NSSetUncaughtExceptionHandler, d_noop),
    F(NSGetUncaughtExceptionHandler, r_null), F(NSLogv, d_noop), F(__cxa_call_unexpected, r_terminate),
    // puertos Mach / reportero de crashes (PLCrashReporter): siempre falla sin efectos
    F(mach_port_allocate, r_one), F(mach_port_deallocate, r_zero), F(mach_port_insert_right, r_one),
    F(mach_port_mod_refs, r_zero), F(mach_port_move_member, r_one), F(mach_port_request_notification, r_one),
    F(mach_msg, r_one), F(mach_thread_self, r_zero), F(mach_make_memory_entry_64, r_one), F(task_threads, r_one),
    F(task_get_exception_ports, r_one), F(task_swap_exception_ports, r_one), F(thread_get_exception_ports, r_one),
    F(thread_swap_exception_ports, r_one), F(thread_get_state, r_one), F(thread_suspend, r_one),
    F(thread_resume, r_one), F(exception_raise, r_one), F(exception_raise_state, r_one),
    F(exception_raise_state_identity, r_one), F(vm_allocate, r_one), F(vm_map, r_one), F(vm_protect, r_one),
    F(vm_read_overwrite, r_one), V(vm_page_size, &k_vm_page_size), V(NDR_record, k_NDR_record),
    V(mach_task_self_, &k_mach_task_self_),
    // Keychain / SSL: sin datos
    F(SecItemAdd, r_minus1), F(SecItemCopyMatching, r_minus1), F(SecItemDelete, r_minus1), F(SecItemUpdate, r_minus1),
    F(SecTrustGetCertificateCount, r_zero), F(SecTrustGetCertificateAtIndex, r_null),
    F(SecCertificateCopySubjectSummary, r_null), F(SSLCreateContext, r_null), F(SSLClose, r_minus1),
    F(SSLHandshake, r_minus1), F(SSLRead, r_minus1), F(SSLWrite, r_minus1), F(SSLSetConnection, r_minus1),
    F(SSLSetIOFuncs, r_minus1), F(SSLSetPeerDomainName, r_minus1), F(SSLSetProtocolVersionMin, r_minus1),
    F(SSLSetProtocolVersionMax, r_minus1), F(SSLSetSessionOption, r_minus1), F(SSLGetSessionState, r_minus1),
    F(SSLGetBufferedReadSize, r_minus1), F(SSLGetNegotiatedCipher, r_minus1),
    F(SSLGetNegotiatedProtocolVersion, r_minus1), F(SSLCopyPeerTrust, r_minus1),
    F(CFNetworkCopySystemProxySettings, r_null),
    // sqlite (SceSqlite)
    F(sqlite3_open_v2, d_sqlite3_open_v2), D(sqlite3_close), D(sqlite3_prepare_v2), D(sqlite3_step),
    D(sqlite3_finalize), D(sqlite3_bind_blob), D(sqlite3_bind_text), D(sqlite3_column_blob), D(sqlite3_column_bytes),
    D(sqlite3_column_count), D(sqlite3_column_text), D(sqlite3_errmsg), D(sqlite3_free), D(sqlite3_malloc),
    F(sqlite3_vfs_register, r_zero), F(sqlite3_snprintf, r_null),
    // ExtAudioFile
    F(ExtAudioFileOpenURL, ext_OpenURL), F(ExtAudioFileDispose, ext_Dispose), F(ExtAudioFileGetProperty, ext_GetProperty),
    F(ExtAudioFileSetProperty, ext_SetProperty), F(ExtAudioFileRead, ext_Read),
    V(UIBackgroundTaskInvalid, &k_UIBackgroundTaskInvalid), V(UIWindowLevelNormal, &k_UIWindowLevelNormal),
};
TABLE_END(ios5)

void ios5_init(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&sync_lock, &a);
    sync_init = 1;
    c_stack_block = objc_define("__NSStackBlock__", "NSObject", 4, block_i, NULL);
    c_malloc_block = objc_define("__NSMallocBlock__", "NSObject", 4, block_i, NULL);
    c_global_block = objc_define("__NSGlobalBlock__", "NSObject", 4, block_i, NULL);
    sceSysmoduleLoadModule(SCE_SYSMODULE_SQLITE);
}

// imports.c: __NSConcreteStackBlock etc. = la dirección de la "clase" del bloque
const import_entry *ios5_block_import(const char *name) {
    static import_entry e;
    objc_class *c = !strcmp(name, "__NSConcreteStackBlock")    ? c_stack_block
                    : !strcmp(name, "__NSConcreteMallocBlock") ? c_malloc_block
                    : !strcmp(name, "__NSConcreteGlobalBlock") ? c_global_block
                                                               : NULL;
    if (!c)
        return NULL;
    e = (import_entry){name, (uintptr_t)c, 1};
    return &e;
}
