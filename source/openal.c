// OpenAL de iOS -> openal-soft de VitaSDK. Mismo ABI (softfp, enteros/punteros); solo hay que
// envolver lo que el juego obtiene por alcGetProcAddress (entra sin alinear la pila) y la extensión
// de Apple alBufferDataStatic (datos sin copiar -> acá se copian).
#include <AL/al.h>
#include <AL/alc.h>
#include <psp2/kernel/cpu.h>
#include <string.h>

#include "imports.h"
#include "log.h"
#include "macho.h"

extern void ml_align_call(void);

// Hilo mezclador de openal-soft (backend vita.c los lee como símbolos débiles): por defecto hereda la prioridad
// del hilo del juego - 1 y ningún núcleo fijo, así que compite con el juego y el audio sale entrecortado. Como en
// Asphalt 5: núcleo propio (2; el juego y vitaGL van en el 0/1) y prioridad alta (64 = mínimo que acepta).
int _oal_thread_priority = 64;
int _oal_thread_affinity = SCE_KERNEL_CPU_MASK_USER_2;

static void al_BufferDataStatic(ALuint buf, ALenum fmt, const ALvoid *data, ALsizei size, ALsizei freq) {
    alBufferData(buf, fmt, data, size, freq);
}
static void *al_GetProcAddress(ALCdevice *dev, const ALCchar *name) {
    void *fn = NULL;
    if (!strcmp(name, "alBufferDataStatic") || !strcmp(name, "alBufferDataStaticProc"))
        fn = (void *)al_BufferDataStatic;
    else
        fn = alcGetProcAddress(dev, name);
    l_info("alcGetProcAddress(%s) -> %s", name, fn ? "ok" : "NULL");
    return fn ? (void *)macho_thunk((uint32_t)fn, ml_align_call) : NULL;
}
static void *al_GetProcAddress2(const ALchar *name) { return al_GetProcAddress(NULL, name); }
static ALCdevice *al_OpenDevice(const ALCchar *name) {
    ALCdevice *d = alcOpenDevice(NULL); // el nombre de dispositivo de iOS no existe acá
    l_info("alcOpenDevice -> %p", d);
    return d;
}
// SoundEngine (Apple) llama alGetError justo después de alcOpenDevice, sin contexto: openal-soft da
// AL_INVALID_OPERATION (0xA004) y el juego aborta el audio entero; iOS devuelve AL_NO_ERROR.
static ALenum al_GetError(void) { return alcGetCurrentContext() ? alGetError() : AL_NO_ERROR; }

const import_entry openal_imports[] = {
    D(alBufferData), D(alBufferf), D(alBufferi), D(alDeleteBuffers), D(alDeleteSources), D(alGenBuffers),
    D(alGenSources), F(alGetError, al_GetError), D(alGetSourcei), D(alGetSourcef), D(alGetSourcefv), D(alGetSourceiv),
    D(alGetSource3f), D(alGetBufferi), D(alGetBufferf), D(alListener3f), D(alListenerfv), D(alListenerf),
    D(alListeneri), D(alGetListenerf), D(alGetListenerfv), D(alGetListener3f), D(alSource3f), D(alSource3i),
    D(alSourcefv), D(alSourceiv), D(alSourcePause), D(alSourcePlay), D(alSourceQueueBuffers), D(alSourceRewind),
    D(alSourceStop), D(alSourceUnqueueBuffers), D(alSourcef), D(alSourcei), D(alSourcePlayv), D(alSourceStopv),
    D(alSourcePausev), D(alSourceRewindv), D(alIsSource), D(alIsBuffer), D(alGetString), D(alGetInteger),
    D(alGetFloat), D(alGetBoolean), D(alDistanceModel), D(alDopplerFactor), D(alDopplerVelocity),
    D(alSpeedOfSound), D(alEnable), D(alDisable), D(alIsEnabled), D(alIsExtensionPresent), D(alGetEnumValue),
    D(alcCloseDevice), D(alcCreateContext), D(alcDestroyContext), D(alcGetContextsDevice), D(alcGetCurrentContext),
    D(alcMakeContextCurrent), D(alcProcessContext), D(alcSuspendContext), D(alcGetError), D(alcGetString),
    D(alcGetIntegerv), D(alcIsExtensionPresent), F(alcOpenDevice, al_OpenDevice),
    F(alcGetProcAddress, al_GetProcAddress), F(alGetProcAddress, al_GetProcAddress2),
};
TABLE_END(openal)
