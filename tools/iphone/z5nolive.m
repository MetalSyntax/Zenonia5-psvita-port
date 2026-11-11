// Tweak para Zenonia 5 iOS 1.1.3 (iPhone con jailbreak): anula Gamevil Live (GamevilNeo / gamevilLive),
// cuyo servidor live.gamevil.com ya no existe y deja la ventana "crear ID" en bucle.
// Reemplaza implementaciones con el runtime de ObjC (no necesita Substrate para hookear, solo para cargarse).
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <syslog.h>

#define LOG(...) syslog(LOG_WARNING, "[Z5NOLIVE] " __VA_ARGS__)

static id r_nil(id self, SEL _cmd, ...) { LOG("%s anulado", sel_getName(_cmd)); return nil; }
static BOOL r_yes(id self, SEL _cmd, ...) { LOG("%s -> YES", sel_getName(_cmd)); return YES; }
static BOOL r_no(id self, SEL _cmd, ...) { LOG("%s -> NO", sel_getName(_cmd)); return NO; }
static id r_login(id self, SEL _cmd) { return @"jugador01"; }

static void rep(const char *cls, BOOL meta, const char *sel, IMP imp) {
    Class c = objc_getClass(cls);
    if (meta && c) c = object_getClass(c);
    Method m = c ? class_getInstanceMethod(c, sel_registerName(sel)) : NULL;
    if (!m) { LOG("no existe %s%s %s", meta ? "+" : "-", cls, sel); return; }
    method_setImplementation(m, imp);
}

// Diagnóstico: pila de llamadas (direcciones sin ASLR, comparables con objdump) al abrir teclado/alertas.
#include <execinfo.h>
#include <mach-o/dyld.h>
static void dump_stack(const char *why) {
    void *bt[24];
    int n = backtrace(bt, 24);
    intptr_t slide = _dyld_get_image_vmaddr_slide(0);
    char line[1024] = "";
    for (int i = 1; i < n; i++) {
        char a[24];
        snprintf(a, sizeof a, " %lx", (unsigned long)((intptr_t)bt[i] - slide));
        strlcat(line, a, sizeof line);
    }
    LOG("PILA %s:%s", why, line);
}
static IMP orig_bfr, orig_alert, orig_tfinit;
static id h_tfinit(id self, SEL _cmd, CGRect r) {
    dump_stack("UITextField initWithFrame");
    return ((id(*)(id, SEL, CGRect))orig_tfinit)(self, _cmd, r);
}
static BOOL h_bfr(id self, SEL _cmd) {
    dump_stack(class_getName(object_getClass(self)));
    return ((BOOL(*)(id, SEL))orig_bfr)(self, _cmd);
}
static void h_alert(id self, SEL _cmd) {
    dump_stack("UIAlertView show");
    ((void (*)(id, SEL))orig_alert)(self, _cmd);
}

__attribute__((constructor)) static void init(void) {
    Method m = class_getInstanceMethod(objc_getClass("UITextField"), @selector(becomeFirstResponder));
    if (m) orig_bfr = method_setImplementation(m, (IMP)h_bfr);
    m = class_getInstanceMethod(objc_getClass("UIAlertView"), @selector(show));
    if (m) orig_alert = method_setImplementation(m, (IMP)h_alert);
    m = class_getInstanceMethod(objc_getClass("UITextField"), @selector(initWithFrame:));
    if (m) orig_tfinit = method_setImplementation(m, (IMP)h_tfinit);
    // Instancia GamevilNeo
    rep("GamevilNeo", NO, "isLogined", (IMP)r_yes);
    rep("GamevilNeo", NO, "IsLiveDlg", (IMP)r_no);
    rep("GamevilNeo", NO, "IsDlg", (IMP)r_no);
    rep("GamevilNeo", NO, "checkLogin", (IMP)r_nil);
    rep("GamevilNeo", NO, "requestLogin", (IMP)r_nil);
    rep("GamevilNeo", NO, "NeoGoLive", (IMP)r_nil);
    rep("GamevilNeo", NO, "OpenGamevilLive:", (IMP)r_nil);
    rep("GamevilNeo", NO, "ShowWebView", (IMP)r_nil);
    rep("GamevilNeo", NO, "setVisible:", (IMP)r_nil);
    rep("GamevilNeo", NO, "getLoginId", (IMP)r_login);
    rep("GamevilNeo", NO, "getLoginString", (IMP)r_login);
    // Fachada de clase gamevilLive
    rep("gamevilLive", YES, "IsLogined", (IMP)r_yes);
    rep("gamevilLive", YES, "CheckLogin", (IMP)r_nil);
    rep("gamevilLive", YES, "RequestLogin", (IMP)r_nil);
    rep("gamevilLive", YES, "GoGamevilLive", (IMP)r_nil);
    LOG("cargado");
}
