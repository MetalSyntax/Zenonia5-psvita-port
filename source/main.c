// Arranque: subsistemas -> Mach-O -> runtime ObjC -> main() del juego (que llama a
// UIApplicationMain y nunca vuelve).
#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <psp2/system_param.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "foundation.h"
#include "gles.h"
#include "hle.h"
#include "input.h"
#include "log.h"
#include "macho.h"
#include "objc.h"
#include "patch.h"

int _newlib_heap_size_user = 300 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize = 2 * 1024 * 1024; // iOS: 1 MB el hilo principal

void darwin_init(void);
void objc_rt_init(void);
void uikit_init(void);
void ios5_init(void);

static int next_log_number(void) {
    sceIoMkdir(DATA_PATH, 0777);
    sceIoMkdir(LOGS_PATH, 0777);
    int last = 0, n;
    uint64_t newest = 0;
    SceUID d = sceIoDopen(LOGS_PATH);
    SceIoDirent e;
    while (d >= 0 && sceIoDread(d, &e) > 0) {
        const SceDateTime *t = &e.d_stat.st_mtime;
        uint64_t key = (uint64_t)t->year << 48 | (uint64_t)t->month << 40 | (uint64_t)t->day << 32 |
                       (uint64_t)t->hour << 24 | t->minute << 16 | t->second << 8 | t->microsecond / 10000;
        if (sscanf(e.d_name, PORT_SLUG "_%3d.log", &n) == 1 && n >= 1 && n <= 999 && key >= newest) {
            newest = key;
            last = n;
        }
    }
    if (d >= 0)
        sceIoDclose(d);
    return last % 999 + 1;
}

// Binario sin símbolos: el start de crt1 (ARM) termina con "bl/blx main" antes de exit.
static uintptr_t find_main_from_start(uintptr_t start) {
    if (!start || (start & 1))
        return 0;
    for (uint32_t *p = (uint32_t *)start; p < (uint32_t *)start + 48; p++) {
        uint32_t w = *p;
        int32_t off = (int32_t)(w << 8) >> 6; // imm24 * 4 con signo
        if ((w >> 24) == 0xEB) // bl (ARM -> ARM)
            return (uintptr_t)p + 8 + off;
        if ((w >> 25) == 0x7D) // blx imm (ARM -> Thumb), H = bit 24
            return ((uintptr_t)p + 8 + off + ((w >> 23) & 2)) | 1;
    }
    return 0;
}

int main(void) {
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    char log_path[128];
    snprintf(log_path, sizeof(log_path), LOGS_PATH PORT_SLUG "_%03d.log", next_log_number());
    log_open(log_path);
    l_info("%s (iOS -> PS Vita, loader Mach-O + runtime ObjC) iniciando", PORT_NAME);

    if (sceIoGetstat("ur0:/data/libshacccg.suprx", &(SceIoStat){0}) < 0 &&
        sceIoGetstat("ur0:/data/external/libshacccg.suprx", &(SceIoStat){0}) < 0)
        fatal_error("Falta libshacccg.suprx (instalarlo con ShaRKBR33D)");
    if (sceIoGetstat(BUNDLE_PATH PORT_EXEC, &(SceIoStat){0}) < 0)
        fatal_error("Falta " BUNDLE_PATH PORT_EXEC ": copiá la .app del IPA (tools/stage_data.sh + deploy_data.py)");

    SceAppUtilInitParam init;
    SceAppUtilBootParam boot;
    memset(&init, 0, sizeof(init));
    memset(&boot, 0, sizeof(boot));
    sceAppUtilInit(&init, &boot);
    SceCommonDialogConfigParam dlg;
    sceCommonDialogConfigParamInit(&dlg);
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, (int *)&dlg.language);
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, (int *)&dlg.enterButtonAssign);
    sceCommonDialogSetConfigParam(&dlg);
    sceIoMkdir(DOCS_PATH, 0777);
    chdir(BUNDLE_PATH); // iOS arranca con cwd = "/"; varios motores abren rutas relativas al bundle

    gles_init();
    darwin_init();
    objc_rt_init();
    foundation_init(); // clases nativas: antes de los binds (_OBJC_CLASS_$_...)
    uikit_init();
    ios5_init();
    input_init();

    macho_load(BUNDLE_PATH PORT_EXEC, DATA_PATH "rebase.bin", DATA_PATH "binds.bin");
    patch_init();
    macho_finalize(); // __TEXT recién ahora está en memoria: las cadenas ObjC viven ahí
    foundation_fix_cfstrings();
    objc_init_image();
    macho_run_inits();

    uintptr_t gmain = macho_sym("_main");
    if (!gmain)
        gmain = macho_entry_is_main() ? macho_entry() : find_main_from_start(macho_entry());
    if (!gmain)
        fatal_error("sin símbolo _main y no se encontró la llamada en start (0x%08X)",
                    (unsigned)(macho_entry() - MACHO_SLIDE));
    static char *argv[] = {"/var/mobile/Applications/00000000-0000-0000-0000-000000000000/" PORT_EXEC ".app/" PORT_EXEC,
                           NULL};
    static char *envp[] = {NULL};
    l_info("main() del juego en 0x%08X", (unsigned)(gmain - MACHO_SLIDE));
    int r = call_guest((void *)gmain, 1, (uint32_t)argv, (uint32_t)envp, (uint32_t)envp);
    l_info("main() volvió con %d", r);
    sceKernelExitProcess(0);
    return 0;
}
