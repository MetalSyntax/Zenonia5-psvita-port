// AudioToolbox de iOS: AudioSession (stubs), AudioFile (lector de .ogg/.wav) y AudioQueue (salida PCM).
//
// La música de estos juegos es .m4a (AAC) leída con AudioFile y reproducida con AudioQueue. En la
// Vita no hay decodificador AAC a mano, así que tools/stage_data.sh convierte cada .m4a/.mp3/.caf
// a .ogg con el mismo nombre; AudioFileOpenURL abre el .ogg y lo presenta como LPCM 16 bits
// intercalado (1 paquete = 1 frame). El juego arma la AudioQueue con ese formato y acá un hilo por
// cola mezcla (remuestreo lineal a 48 kHz estéreo) hacia un puerto MAIN de sceAudioOut, y llama al
// callback del juego cuando un buffer se consumió.
// AudioFileStream: parser de WAV PCM 16 bits en memoria (SFX de sounds.bar).
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vorbis/vorbisfile.h>

#include "foundation.h"
#include "hle.h"
#include "imports.h"
#include "log.h"
#include "macho.h"

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (b) << 16 | (c) << 8 | (d))
#define kAudioFormatLinearPCM FOURCC('l', 'p', 'c', 'm')
#define ERR_FILE_NOT_FOUND -43      // fnfErr
#define ERR_UNSUPPORTED FOURCC('t', 'y', 'p', '?')
#define ERR_EOF -39

typedef struct {
    double mSampleRate;
    uint32_t mFormatID, mFormatFlags, mBytesPerPacket, mFramesPerPacket, mBytesPerFrame, mChannelsPerFrame,
        mBitsPerChannel, mReserved;
} asbd;

// ---------------------------------------------------------------- AudioSession

static int as_ok(void) { return 0; }
static int as_GetProperty(uint32_t pid, uint32_t *size, void *data) {
    switch (pid) {
    case FOURCC('o', 't', 'h', 'r'): // OtherAudioIsPlaying
    case FOURCC('a', 'c', 'h', 'n'): // AudioInputAvailable (pre 3.0: 'aiav')
    case FOURCC('a', 'i', 'a', 'v'):
        *(uint32_t *)data = 0;
        *size = 4;
        return 0;
    case FOURCC('c', 'h', 's', 'r'): // CurrentHardwareSampleRate (Float64)
        *(double *)data = 44100.0;
        *size = 8;
        return 0;
    case FOURCC('c', 'h', 'o', 'c'): // CurrentHardwareOutputNumberChannels
        *(uint32_t *)data = 2;
        *size = 4;
        return 0;
    case FOURCC('c', 'h', 'o', 'v'): // CurrentHardwareOutputVolume (Float32)
        *(float *)data = 1.0f;
        *size = 4;
        return 0;
    case FOURCC('r', 'o', 'u', 't'): // AudioRoute (CFString)
        *(id *)data = ns_string_new("Speaker");
        *size = 4;
        return 0;
    }
    memset(data, 0, *size);
    return 0;
}

// ---------------------------------------------------------------- AudioFile (.ogg / .wav)

typedef struct {
    int kind; // 1 ogg, 2 wav
    OggVorbis_File vf;
    FILE *f;
    long data_off, data_len, pos_frames;
    int channels, rate;
    int64_t frames;
} audio_file;

static const char *try_ext(const char *in, const char *ext, char *out, int max) {
    snprintf(out, max, "%s", in);
    char *dot = strrchr(out, '.'), *sl = strrchr(out, '/');
    if (dot && (!sl || dot > sl))
        *dot = 0;
    strncat(out, ext, max - strlen(out) - 1);
    FILE *f = fopen(out, "rb");
    if (f)
        fclose(f);
    return f ? out : NULL;
}

static int wav_open(audio_file *a, const char *path) {
    FILE *f = fopen(path, "rb");
    uint8_t h[12];
    if (!f || fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        if (f)
            fclose(f);
        return -1;
    }
    uint8_t ch[8];
    int fmt_ok = 0;
    while (fread(ch, 1, 8, f) == 8) {
        uint32_t len = ch[4] | ch[5] << 8 | ch[6] << 16 | ch[7] << 24;
        if (!memcmp(ch, "fmt ", 4)) {
            uint8_t fm[16];
            fread(fm, 1, 16, f);
            fmt_ok = (fm[0] | fm[1] << 8) == 1 && (fm[14] | fm[15] << 8) == 16;
            a->channels = fm[2] | fm[3] << 8;
            a->rate = fm[4] | fm[5] << 8 | fm[6] << 16 | fm[7] << 24;
            fseek(f, len - 16, SEEK_CUR);
        } else if (!memcmp(ch, "data", 4)) {
            a->data_off = ftell(f);
            a->data_len = len;
            break;
        } else {
            fseek(f, (len + 1) & ~1, SEEK_CUR);
        }
    }
    if (!fmt_ok || !a->data_off) {
        fclose(f);
        return -1;
    }
    a->f = f;
    a->kind = 2;
    a->frames = a->data_len / (a->channels * 2);
    return 0;
}

// AudioFileOpenURL(CFURLRef url, perms, typeHint, AudioFileID *out)
static int af_OpenURL(id url, int perms, uint32_t hint, audio_file **out) {
    const char *darwin_path(const char *in, char *out, size_t outlen);
    char inb[512], p[512];
    const char *in = darwin_path(ns_cstr((id)MSG0(url, "path")), inb, sizeof(inb)); // X.app/ -> game_app/
    const char *path = try_ext(in, ".ogg", p, sizeof(p));
    audio_file *a = calloc(1, sizeof(*a));
    if (path && !ov_fopen(path, &a->vf)) {
        vorbis_info *vi = ov_info(&a->vf, -1);
        a->kind = 1;
        a->channels = vi->channels;
        a->rate = vi->rate;
        a->frames = ov_pcm_total(&a->vf, -1);
    } else if ((path = try_ext(in, ".wav", p, sizeof(p))) && !wav_open(a, path)) {
    } else if (!wav_open(a, in)) {
        path = in;
    } else {
        l_error("AudioFileOpenURL(%s): sin .ogg/.wav convertido (tools/stage_data.sh)", in);
        free(a);
        *out = NULL;
        return ERR_FILE_NOT_FOUND;
    }
    l_info("AudioFile %s: %d Hz, %d canales, %lld frames", path, a->rate, a->channels, (long long)a->frames);
    *out = a;
    return 0;
}
// Para ExtAudioFile (ios5.c)
static int af_OpenURL(id url, int perms, uint32_t hint, audio_file **out);
int af_open_path(const char *path, void **out) {
    return af_OpenURL(MSG1(objc_getClass("NSURL"), "fileURLWithPath:", ns_str(path)), 1, 0, (audio_file **)out);
}
static void fill_asbd(audio_file *a, asbd *d);
int af_format(void *a, asbd *d, int64_t *frames) {
    fill_asbd(a, d);
    *frames = ((audio_file *)a)->frames;
    return 0;
}
static long read_frames(audio_file *a, int64_t start, long nframes, int16_t *out);
long af_read_frames(void *a, int64_t start, long n, int16_t *out) { return read_frames(a, start, n, out); }
int af_close(void *a);

static int af_Close(audio_file *a) {
    if (!a)
        return 0;
    if (a->kind == 1)
        ov_clear(&a->vf);
    else if (a->f)
        fclose(a->f);
    free(a);
    return 0;
}
static void fill_asbd(audio_file *a, asbd *d) {
    *d = (asbd){a->rate, kAudioFormatLinearPCM, 0xC /* signed int | packed */, 2 * a->channels, 1,
                2 * a->channels, a->channels, 16, 0};
}
static int af_GetProperty(audio_file *a, uint32_t pid, uint32_t *size, void *data);
// Responde lo mismo que GetProperty: 'cmap'/'mgic' no existen (SoundEngine/BackgroundTrackMgr de Apple
// solo los pide si GetPropertyInfo da noErr; si no, "Error getting channel layout" y sin música).
static int af_GetPropertyInfo(audio_file *a, uint32_t pid, uint32_t *size, uint32_t *writable) {
    uint8_t tmp[64];
    uint32_t sz = sizeof(tmp);
    int r = af_GetProperty(a, pid, &sz, tmp);
    if (size)
        *size = r ? 0 : sz;
    if (writable)
        *writable = 0;
    return r;
}
static int af_GetProperty(audio_file *a, uint32_t pid, uint32_t *size, void *data) {
    if (!a)
        return -50;
    switch (pid) {
    case FOURCC('d', 'f', 'm', 't'): // DataFormat
        fill_asbd(a, data);
        *size = sizeof(asbd);
        return 0;
    case FOURCC('p', 'c', 'n', 't'): // AudioDataPacketCount (UInt64)
        *(uint64_t *)data = a->frames;
        *size = 8;
        return 0;
    case FOURCC('b', 'c', 'n', 't'): // AudioDataByteCount (UInt64)
        *(uint64_t *)data = a->frames * 2 * a->channels;
        *size = 8;
        return 0;
    case FOURCC('p', 's', 'z', 'e'): // MaximumPacketSize (UInt32)
    case FOURCC('p', 'k', 'u', 'b'): // PacketSizeUpperBound
        *(uint32_t *)data = 2 * a->channels;
        *size = 4;
        return 0;
    case FOURCC('e', 'd', 'u', 'r'): // EstimatedDuration (Float64)
        *(double *)data = (double)a->frames / a->rate;
        *size = 8;
        return 0;
    case FOURCC('f', 'f', 'm', 't'): // FileFormat
        *(uint32_t *)data = FOURCC('W', 'A', 'V', 'E');
        *size = 4;
        return 0;
    }
    l_info("AudioFileGetProperty('%c%c%c%c') sin soporte", pid >> 24, pid >> 16, pid >> 8, pid);
    return ERR_UNSUPPORTED;
}
static long read_frames(audio_file *a, int64_t start, long nframes, int16_t *out) {
    if (start >= a->frames)
        return 0;
    if (start + nframes > a->frames)
        nframes = a->frames - start;
    long got = 0;
    if (a->kind == 1) {
        if (a->pos_frames != start)
            ov_pcm_seek(&a->vf, start);
        int bs;
        while (got < nframes) {
            long r = ov_read(&a->vf, (char *)(out + got * a->channels), (nframes - got) * 2 * a->channels, 0, 2, 1, &bs);
            if (r <= 0)
                break;
            got += r / (2 * a->channels);
        }
    } else {
        fseek(a->f, a->data_off + start * 2 * a->channels, SEEK_SET);
        got = fread(out, 2 * a->channels, nframes, a->f);
    }
    a->pos_frames = start + got;
    return got;
}
// AudioFileReadPackets(file, useCache, UInt32 *outBytes, descs, SInt64 startPacket, UInt32 *ioNum, void *buf)
// SInt64 en el 5to argumento: Darwin lo pasa en la pila sin alinear (2 palabras seguidas).
static int af_ReadPackets(audio_file *a, int cache, uint32_t *out_bytes, void *descs, uint32_t start_lo,
                          uint32_t start_hi, uint32_t *io_num, void *buf) {
    if (!a)
        return -50;
    int64_t start = (int64_t)start_hi << 32 | start_lo;
    long got = read_frames(a, start, *io_num, buf);
    *io_num = got;
    if (out_bytes)
        *out_bytes = got * 2 * a->channels;
    return 0;
}
// AudioFileReadBytes(file, useCache, SInt64 startByte, UInt32 *ioNumBytes, void *buf): startByte en r2:r3
static int af_ReadBytes(audio_file *a, int cache, uint32_t start_lo, uint32_t start_hi, uint32_t *io_bytes, void *buf) {
    if (!a)
        return -50;
    int fb = 2 * a->channels;
    int64_t start = ((int64_t)start_hi << 32 | start_lo) / fb;
    long got = read_frames(a, start, *io_bytes / fb, buf);
    *io_bytes = got * fb;
    return got ? 0 : ERR_EOF;
}
// ---------------------------------------------------------------- AudioFileStream (WAV en memoria)
// Los SFX de Gameloft vienen en sounds.bar como WAV PCM 16 bits y se parsean con AudioFileStream
// (SoundDataParser: 'bcnt' -> malloc, 'dfmt' -> ASBD, paquetes -> memcpy). Se acumula hasta tener la
// cabecera completa; después el PCM se entrega en frames enteros.
// ponytail: solo RIFF/WAVE PCM 16 bits (todo lo que trae sounds.bar); otro formato -> 'typ?'.
typedef struct {
    void *ud, *prop_cb, *pkt_cb;
    uint8_t *hdr;
    uint32_t hlen, data_off, total, left, carry_len;
    int ready;
    asbd fmt;
    uint8_t carry[8];
} af_stream;

static int af_StreamOpen(void *ud, void *prop_cb, void *pkt_cb, uint32_t hint, af_stream **out) {
    af_stream *s = calloc(1, sizeof(*s));
    s->ud = ud, s->prop_cb = prop_cb, s->pkt_cb = pkt_cb;
    *out = s;
    return 0;
}

static void stream_prop(af_stream *s, uint32_t pid) {
    uint32_t flags = 0;
    call_guest(s->prop_cb, (uint32_t)s->ud, (uint32_t)s, pid, (uint32_t)&flags);
}

static void stream_packets(af_stream *s, const uint8_t *p, uint32_t n) {
    uint32_t bpf = s->fmt.mBytesPerFrame;
    if (n > s->left)
        n = s->left;
    s->left -= n;
    if (s->carry_len) { // completar el frame partido de la llamada anterior
        uint32_t k = bpf - s->carry_len < n ? bpf - s->carry_len : n;
        memcpy(s->carry + s->carry_len, p, k);
        s->carry_len += k, p += k, n -= k;
        if (s->carry_len < bpf)
            return;
        call_guest6(s->pkt_cb, (uint32_t)s->ud, bpf, 1, (uint32_t)s->carry, 0, 0);
        s->carry_len = 0;
    }
    uint32_t whole = n - n % bpf;
    if (whole)
        call_guest6(s->pkt_cb, (uint32_t)s->ud, whole, whole / bpf, (uint32_t)p, 0, 0);
    memcpy(s->carry, p + whole, n - whole);
    s->carry_len = n - whole;
}

// 1 = cabecera completa, 0 = faltan bytes, -1 = no es WAV PCM 16
static int stream_header(af_stream *s) {
    const uint8_t *h = s->hdr;
    if (s->hlen < 12)
        return 0;
    if (memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
        return -1;
    int ch = 0, rate = 0, fmt_ok = 0;
    for (uint32_t o = 12; o + 8 <= s->hlen;) {
        uint32_t len = h[o + 4] | h[o + 5] << 8 | h[o + 6] << 16 | (uint32_t)h[o + 7] << 24;
        if (!memcmp(h + o, "data", 4)) {
            if (!fmt_ok)
                return -1;
            s->data_off = o + 8;
            s->total = s->left = len;
            s->fmt = (asbd){rate, kAudioFormatLinearPCM, 0xC, 2 * ch, 1, 2 * ch, ch, 16, 0};
            return 1;
        }
        if (!memcmp(h + o, "fmt ", 4)) {
            if (o + 24 > s->hlen)
                return 0;
            const uint8_t *f = h + o + 8;
            ch = f[2] | f[3] << 8;
            rate = f[4] | f[5] << 8 | f[6] << 16 | f[7] << 24;
            fmt_ok = (f[0] | f[1] << 8) == 1 && (f[14] | f[15] << 8) == 16 && ch >= 1 && ch <= 2;
            if (!fmt_ok)
                return -1;
        }
        o += 8 + ((len + 1) & ~1u);
    }
    return 0;
}

// AudioFileStreamParseBytes(stream, UInt32 size, const void *data, UInt32 flags)
static int af_StreamParse(af_stream *s, uint32_t size, const uint8_t *data, uint32_t flags) {
    if (!s)
        return ERR_UNSUPPORTED;
    if (s->ready) {
        stream_packets(s, data, size);
        return 0;
    }
    s->hdr = realloc(s->hdr, s->hlen + size);
    memcpy(s->hdr + s->hlen, data, size);
    s->hlen += size;
    int r = stream_header(s);
    static int nlog; // ponytail: diagnóstico corrida 010 (SFX "Error parsing audio data"); quitar cuando suene
    if (nlog++ < 25)
        l_info("AudioFileStreamParseBytes(%p, %u bytes, %.4s...) -> cabecera %d, datos %u bytes, %.0f Hz %u ch", s,
               (unsigned)size, size >= 4 ? (const char *)data : "?", r, (unsigned)s->total, s->fmt.mSampleRate,
               (unsigned)s->fmt.mChannelsPerFrame);
    if (r < 0) {
        l_error("AudioFileStream: no es WAV PCM 16 bits");
        return ERR_UNSUPPORTED;
    }
    if (r == 0)
        return 0;
    s->ready = 1;
    stream_prop(s, FOURCC('d', 'f', 'm', 't'));
    stream_prop(s, FOURCC('d', 'o', 'f', 'f'));
    stream_prop(s, FOURCC('b', 'c', 'n', 't'));
    stream_prop(s, FOURCC('r', 'e', 'd', 'y'));
    if (s->hlen > s->data_off)
        stream_packets(s, s->hdr + s->data_off, s->hlen - s->data_off);
    free(s->hdr);
    s->hdr = NULL;
    return 0;
}

// AudioFileStreamGetProperty(stream, pid, UInt32 *ioSize, void *out)
static int af_StreamGetProperty(af_stream *s, uint32_t pid, uint32_t *size, void *out) {
    if (!s || !s->ready)
        return FOURCC('u', 'n', 'r', 'e'); // kAudioFileStreamError_NotReadyToProducePackets
    uint64_t v;
    const void *src = &v;
    uint32_t n = 8;
    switch (pid) {
    case FOURCC('d', 'f', 'm', 't'): src = &s->fmt, n = sizeof(s->fmt); break;
    case FOURCC('d', 'o', 'f', 'f'): v = s->data_off; break;
    case FOURCC('b', 'c', 'n', 't'): v = s->total; break;
    case FOURCC('p', 'c', 'n', 't'): v = s->total / s->fmt.mBytesPerFrame; break;
    case FOURCC('r', 'e', 'd', 'y'): v = 1, n = 4; break;
    default: return FOURCC('p', 't', 'y', '?');
    }
    if (*size > n)
        *size = n;
    memcpy(out, src, *size); // el juego pide 'bcnt' con 4 bytes: little-endian -> parte baja
    static int nlog;
    if (nlog++ < 25)
        l_info("AudioFileStreamGetProperty(%.4s) -> %u bytes", (const char *)&(uint32_t){__builtin_bswap32(pid)},
               (unsigned)*size);
    return 0;
}

static int af_StreamClose(af_stream *s) {
    if (s) {
        free(s->hdr);
        free(s);
    }
    return 0;
}

// ---------------------------------------------------------------- AudioQueue (salida PCM)

typedef struct {
    uint32_t mAudioDataBytesCapacity;
    void *mAudioData;
    uint32_t mAudioDataByteSize;
    void *mUserData;
    uint32_t mPacketDescriptionCapacity;
    void *mPacketDescriptions;
    uint32_t mPacketDescriptionCount;
} aq_buffer;

#define MAX_QBUF 16
#define OUT_RATE 48000
#define GRAIN 1024
typedef struct {
    asbd fmt;
    void *cb, *ud;
    aq_buffer *fifo[MAX_QBUF];
    int head, count;
    volatile int running, alive;
    float volume;
    SceUID thread, lock;
    int port;
    void *listener, *listener_ud; // kAudioQueueProperty_IsRunning
    double frac;                  // posición fraccional del remuestreo
    int pos;                      // frame dentro del buffer actual
} audio_queue;

static void aq_lock(audio_queue *q) { sceKernelLockMutex(q->lock, 1, NULL); }
static void aq_unlock(audio_queue *q) { sceKernelUnlockMutex(q->lock, 1); }

static void notify_running(audio_queue *q) {
    if (q->listener)
        call_guest(q->listener, (uint32_t)q->listener_ud, (uint32_t)q, FOURCC('a', 'q', 'r', 'n'), 0);
}

static int aq_thread(SceSize args, void *argp) {
    audio_queue *q = *(audio_queue **)argp;
    int16_t out[GRAIN * 2];
    int ch = q->fmt.mChannelsPerFrame ? q->fmt.mChannelsPerFrame : 2;
    double step = q->fmt.mSampleRate / OUT_RATE;
    while (q->alive) {
        if (!q->running) {
            sceKernelDelayThread(5000);
            continue;
        }
        int n = 0;
        while (n < GRAIN) {
            aq_lock(q);
            aq_buffer *b = q->count ? q->fifo[q->head] : NULL;
            aq_unlock(q);
            if (!b) {
                memset(out + n * 2, 0, (GRAIN - n) * 4); // sin datos: silencio
                break;
            }
            const int16_t *s = b->mAudioData;
            int frames = b->mAudioDataByteSize / (2 * ch);
            while (n < GRAIN && q->pos < frames) {
                int i = q->pos;
                int16_t l = s[i * ch], r = ch > 1 ? s[i * ch + 1] : l;
                out[n * 2] = l * q->volume;
                out[n * 2 + 1] = r * q->volume;
                n++;
                q->frac += step;
                while (q->frac >= 1.0) {
                    q->frac -= 1.0;
                    q->pos++;
                }
            }
            if (q->pos >= frames) { // buffer consumido: devolverlo al juego
                aq_lock(q);
                q->head = (q->head + 1) % MAX_QBUF;
                q->count--;
                aq_unlock(q);
                q->pos = 0;
                call_guest(q->cb, (uint32_t)q->ud, (uint32_t)q, (uint32_t)b, 0);
            }
        }
        sceAudioOutOutput(q->port, out);
    }
    return 0;
}

// AudioQueueNewOutput(const asbd *fmt, cb, ud, CFRunLoopRef, CFStringRef mode, UInt32 flags, AudioQueueRef *out)
static int aq_NewOutput(const asbd *fmt, void *cb, void *ud, void *rl, void *mode, uint32_t flags, audio_queue **out) {
    if (fmt->mFormatID != kAudioFormatLinearPCM || fmt->mBitsPerChannel != 16) {
        l_error("AudioQueueNewOutput: formato '%c%c%c%c' %u bits sin soporte", fmt->mFormatID >> 24,
                fmt->mFormatID >> 16, fmt->mFormatID >> 8, fmt->mFormatID, (unsigned)fmt->mBitsPerChannel);
        *out = NULL;
        return ERR_UNSUPPORTED;
    }
    audio_queue *q = calloc(1, sizeof(*q));
    q->fmt = *fmt;
    q->cb = cb;
    q->ud = ud;
    q->volume = 1.0f;
    q->alive = 1;
    q->lock = sceKernelCreateMutex("ml_aq", 0, 0, NULL);
    q->port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, GRAIN, OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (q->port < 0) {
        l_error("AudioQueue: sceAudioOutOpenPort = 0x%08X", q->port);
        free(q);
        *out = NULL;
        return -1;
    }
    q->thread = sceKernelCreateThread("ml_aq", aq_thread, 0x10000100, 0x10000, 0, 0, NULL);
    sceKernelStartThread(q->thread, sizeof(q), &q);
    l_info("AudioQueue: %.0f Hz, %u canales", fmt->mSampleRate, (unsigned)fmt->mChannelsPerFrame);
    *out = q;
    return 0;
}
static int aq_AllocateBuffer(audio_queue *q, uint32_t size, aq_buffer **out) {
    aq_buffer *b = calloc(1, sizeof(*b));
    b->mAudioDataBytesCapacity = size;
    b->mAudioData = calloc(1, size);
    *out = b;
    return 0;
}
static int aq_AllocateBufferPD(audio_queue *q, uint32_t size, uint32_t npd, aq_buffer **out) {
    aq_AllocateBuffer(q, size, out);
    (*out)->mPacketDescriptionCapacity = npd;
    (*out)->mPacketDescriptions = calloc(npd ? npd : 1, 16);
    return 0;
}
static int aq_FreeBuffer(audio_queue *q, aq_buffer *b) {
    if (b) {
        free(b->mAudioData);
        free(b->mPacketDescriptions);
        free(b);
    }
    return 0;
}
static int aq_Enqueue(audio_queue *q, aq_buffer *b, uint32_t npd, void *pd) {
    if (!q)
        return -50;
    aq_lock(q);
    int r = 0;
    if (q->count < MAX_QBUF) {
        q->fifo[(q->head + q->count) % MAX_QBUF] = b;
        q->count++;
    } else {
        r = -1;
    }
    aq_unlock(q);
    return r;
}
static int aq_Start(audio_queue *q, void *time) {
    if (!q)
        return -50;
    q->running = 1;
    notify_running(q);
    return 0;
}
static int aq_Stop(audio_queue *q, int immediate) {
    if (!q)
        return -50;
    q->running = 0;
    aq_lock(q);
    q->count = 0;
    q->pos = 0;
    aq_unlock(q);
    notify_running(q);
    return 0;
}
static int aq_Pause(audio_queue *q) {
    if (q)
        q->running = 0;
    return 0;
}
static int aq_Dispose(audio_queue *q, int immediate) {
    if (!q)
        return 0;
    q->running = 0;
    q->alive = 0;
    sceKernelWaitThreadEnd(q->thread, NULL, NULL);
    sceKernelDeleteThread(q->thread);
    sceAudioOutReleasePort(q->port);
    sceKernelDeleteMutex(q->lock);
    free(q);
    return 0;
}
// AudioQueueSetParameter(q, id, Float32 value): float en r2 (softfp)
static int aq_SetParameter(audio_queue *q, uint32_t pid, float v) {
    if (q && pid == 1) // kAudioQueueParam_Volume
        q->volume = v < 0 ? 0 : v > 1 ? 1 : v;
    return 0;
}
static int aq_GetParameter(audio_queue *q, uint32_t pid, float *v) {
    *v = q && pid == 1 ? q->volume : 0;
    return 0;
}
static int aq_GetProperty(audio_queue *q, uint32_t pid, void *data, uint32_t *size) {
    if (pid == FOURCC('a', 'q', 'r', 'n')) { // IsRunning
        *(uint32_t *)data = q && q->running;
        *size = 4;
        return 0;
    }
    if (pid == FOURCC('a', 'q', 'f', 't')) { // StreamDescription
        memcpy(data, &q->fmt, sizeof(asbd));
        *size = sizeof(asbd);
        return 0;
    }
    return ERR_UNSUPPORTED;
}
static int aq_AddListener(audio_queue *q, uint32_t pid, void *fn, void *ud) {
    if (q && pid == FOURCC('a', 'q', 'r', 'n'))
        q->listener = fn, q->listener_ud = ud;
    return 0;
}
static int aq_ok(void) { return 0; }

int af_close(void *a) { return af_Close(a); }

const import_entry audio_imports[] = {
    F(AudioSessionInitialize, as_ok), F(AudioSessionSetActive, as_ok), F(AudioSessionSetProperty, as_ok),
    F(AudioSessionGetProperty, as_GetProperty), F(AudioSessionAddPropertyListener, as_ok),
    F(AudioSessionRemovePropertyListener, as_ok), F(AudioSessionGetPropertySize, as_ok),
    F(AudioServicesPlayAlertSound, as_ok), F(AudioServicesPlaySystemSound, as_ok),
    F(AudioServicesCreateSystemSoundID, as_ok), F(AudioServicesDisposeSystemSoundID, as_ok),
    F(AudioFileOpenURL, af_OpenURL), F(AudioFileClose, af_Close), F(AudioFileGetProperty, af_GetProperty),
    F(AudioFileGetPropertyInfo, af_GetPropertyInfo), F(AudioFileReadPackets, af_ReadPackets),
    F(AudioFileReadPacketData, af_ReadPackets), F(AudioFileReadBytes, af_ReadBytes),
    F(AudioFileStreamOpen, af_StreamOpen), F(AudioFileStreamParseBytes, af_StreamParse),
    F(AudioFileStreamGetProperty, af_StreamGetProperty), F(AudioFileStreamClose, af_StreamClose),
    F(AudioQueueNewOutput, aq_NewOutput), F(AudioQueueAllocateBuffer, aq_AllocateBuffer),
    F(AudioQueueAllocateBufferWithPacketDescriptions, aq_AllocateBufferPD), F(AudioQueueFreeBuffer, aq_FreeBuffer),
    F(AudioQueueEnqueueBuffer, aq_Enqueue), F(AudioQueueStart, aq_Start), F(AudioQueueStop, aq_Stop),
    F(AudioQueuePause, aq_Pause), F(AudioQueueDispose, aq_Dispose), F(AudioQueueSetParameter, aq_SetParameter),
    F(AudioQueueGetParameter, aq_GetParameter), F(AudioQueueGetProperty, aq_GetProperty),
    F(AudioQueueSetProperty, aq_ok), F(AudioQueueAddPropertyListener, aq_AddListener),
    F(AudioQueueRemovePropertyListener, aq_ok), F(AudioQueuePrime, aq_ok), F(AudioQueueFlush, aq_ok),
    F(AudioQueueReset, aq_ok),
};
TABLE_END(audio)
