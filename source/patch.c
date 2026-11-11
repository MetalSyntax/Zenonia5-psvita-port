/**
 * @file patch.c
 * @brief Game-specific hooks, memory patches, and custom symbol intercepts.
 *
 * Red emulada (Zenonia 5): Reachability dice "hay WiFi" y los sockets BSD son falsos (conectan siempre).
 * Servidor emulado de zn555.gamevil.com:10000: cada paquete es [u16 largo total LE][u16 cmd LE][datos];
 * la respuesta es cmd+1 con un byte de resultado (0 = OK) al inicio de los datos.
 * Layout de SC_CONNECT: iOS 0xd1c06 (igual a Android API_ZN5_SC_CONNECT 0x159ff0).
 * Errores en errno con valores de Darwin (el juego los lee con __error y compara 35 = EAGAIN).
 */
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <errno.h>

#include "foundation.h"
#include "imports.h"
#include "log.h"
#include "macho.h"
#include "objc.h"
#include "patch.h"

// ---------------------------------------------------------------- Reachability: siempre WiFi
static int reach_obj;
static void *re_create(void) { return &reach_obj; }
static int re_flags(void *ref, uint32_t *flags) {
    if (flags)
        *flags = 2; // kSCNetworkReachabilityFlagsReachable (sin WWAN -> ReachableViaWiFi)
    return 1;
}
static int re_one(void) { return 1; }

// ---------------------------------------------------------------- sockets falsos
#define DARWIN_EAGAIN 35
#define DARWIN_EBADF 9
#define NET_FD0 900 // por debajo de FD_SETSIZE (1024) para que select con fd_set funcione
#define NET_MAX 16

#define NET_QMAX 8192
typedef struct {
    int used, nonblock;
    char host[64];
    int port;
    uint8_t q[NET_QMAX]; // respuestas del servidor emulado pendientes de recv
    int qlen;
} net_sock;
static net_sock socks[NET_MAX];
static char last_host[64] = "?";

static net_sock *ns_get(int fd) {
    int i = fd - NET_FD0;
    return i >= 0 && i < NET_MAX && socks[i].used ? &socks[i] : NULL;
}

static void hexlog(const char *tag, int fd, const uint8_t *b, int n) {
    char line[3 * 32 + 1];
    l_info("[NET] %s fd %d: %d bytes", tag, fd, n);
    for (int o = 0; o < n && o < 512; o += 32) {
        int k = 0;
        for (int j = o; j < n && j < o + 32; j++)
            k += sprintf(line + k, "%02x ", b[j]);
        l_info("[NET]   %04x: %s", o, line);
    }
}

// addrinfo / sockaddr_in de Darwin (32 bits): {flags, family, socktype, protocol, addrlen, canonname, addr, next}
typedef struct darwin_addrinfo {
    int flags, family, socktype, protocol;
    uint32_t addrlen;
    char *canonname;
    void *addr;
    struct darwin_addrinfo *next;
} darwin_addrinfo;
typedef struct {
    uint8_t len, family;
    uint16_t port; // big-endian
    uint32_t addr;
    uint8_t zero[8];
} darwin_sockaddr_in;

static int n_getaddrinfo(const char *node, const char *service, const darwin_addrinfo *hints,
                         darwin_addrinfo **res) {
    darwin_addrinfo *ai = calloc(1, sizeof(*ai) + sizeof(darwin_sockaddr_in));
    darwin_sockaddr_in *sa = (darwin_sockaddr_in *)(ai + 1);
    int port = service ? atoi(service) : 0;
    *sa = (darwin_sockaddr_in){16, 2, (uint16_t)((port >> 8) | (port << 8)), 0x0100007F};
    *ai = (darwin_addrinfo){0, 2, 1, 6, 16, NULL, sa, NULL};
    snprintf(last_host, sizeof(last_host), "%s", node ? node : "?");
    l_info("[NET] getaddrinfo(%s, %s)", last_host, service ? service : "-");
    *res = ai;
    return 0;
}
static void n_freeaddrinfo(darwin_addrinfo *ai) { free(ai); }

// hostent (mismo layout en 32 bits): {name, aliases, addrtype, length, addr_list}
static uint32_t he_addr = 0x0100007F;
static char *he_list[2] = {(char *)&he_addr, NULL};
static char *he_alias[1] = {NULL};
static struct {
    char *name;
    char **aliases;
    int addrtype, length;
    char **addr_list;
} he = {last_host, he_alias, 2, 4, he_list};
static void *n_gethostbyname(const char *name) {
    snprintf(last_host, sizeof(last_host), "%s", name ? name : "?");
    l_info("[NET] gethostbyname(%s)", last_host);
    return &he;
}
static const char *n_inet_ntoa(uint32_t a) { return "127.0.0.1"; }

static int n_socket(int domain, int type, int proto) {
    for (int i = 0; i < NET_MAX; i++)
        if (!socks[i].used) {
            memset(&socks[i], 0, sizeof(socks[i]));
            socks[i].used = 1;
            l_info("[NET] socket(%d, %d, %d) -> %d", domain, type, proto, NET_FD0 + i);
            return NET_FD0 + i;
        }
    errno = 24; // EMFILE
    return -1;
}
static int n_connect(int fd, const darwin_sockaddr_in *sa, int len) {
    net_sock *s = ns_get(fd);
    if (!s) {
        errno = DARWIN_EBADF;
        return -1;
    }
    snprintf(s->host, sizeof(s->host), "%s", last_host);
    s->port = sa ? ((sa->port >> 8) | ((sa->port & 0xFF) << 8)) : 0;
    l_info("[NET] connect fd %d -> %s:%d", fd, s->host, s->port);
    return 0;
}
static int n_ok(void) { return 0; }
static int n_fcntl(int fd, int cmd, int arg) {
    net_sock *s = ns_get(fd);
    if (!s)
        return 0;
    if (cmd == 4) // F_SETFL
        s->nonblock = (arg & 4) != 0; // O_NONBLOCK de Darwin
    return cmd == 3 ? (s->nonblock ? 4 : 0) : 0; // F_GETFL
}
#define EMU_USER_ID 1000001 // ID de usuario por defecto que da el servidor emulado
#define EMU_NICK "jugador01" // apodo por defecto (el mismo que devuelve getLoginId en tools/iphone/z5nolive.m)

static void emu_reply(net_sock *s, int cmd, const uint8_t *data, int n) {
    if (s->qlen + 4 + n > NET_QMAX) {
        l_error("[EMU] cola llena, se descarta 0x%x", cmd);
        return;
    }
    uint8_t *p = s->q + s->qlen;
    p[0] = (4 + n) & 0xFF, p[1] = (4 + n) >> 8, p[2] = cmd & 0xFF, p[3] = cmd >> 8;
    memcpy(p + 4, data, n);
    s->qlen += 4 + n;
    l_info("[EMU] S>C 0x%03x (%d bytes)", cmd, 4 + n);
}
static void put_u4(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static void emu_handle(net_sock *s, int cmd, const uint8_t *body, int n) {
    uint8_t r[256] = {0}; // r[0] = resultado 0 (OK)
    switch (cmd) {
    case 0x100: // CS_CONNECT -> SC_CONNECT (86 bytes de datos)
        r[1] = 1;                       // conexión aceptada (0 = el cliente se desconecta)
        r[4] = 1;                       // 0 + estado 6 = error -3
        put_u4(r + 8, EMU_USER_ID);     // ID de usuario (se guarda si el cliente no tiene)
        put_u4(r + 79, (uint32_t)time(NULL)); // hora del servidor
        // r[16..63]: apodo (CMvNet+0xc4, iOS 0xd1d0c). Vacío -> OnNetSuccess abre "crear ID".
        strcpy((char *)r + 16, EMU_NICK);
        emu_reply(s, 0x101, r, 86);
        break;
    case 0x102: // CS_CREATE_USER_ID -> SC 0x103 (iOS 0xd0e56 no lee datos; el cliente vuelve a mandar 0x100)
        emu_reply(s, 0x103, r, 1);
        break;
    default:
        // ponytail: respuesta genérica en cero para comandos sin emular; armar cada layout cuando el log
        // muestre que el juego lo necesita.
        l_info("[EMU] C>S 0x%03x sin respuesta emulada: se responde 0x%03x en cero", cmd, cmd + 1);
        emu_reply(s, cmd + 1, r, sizeof(r));
        break;
    }
}

static int n_send(int fd, const void *buf, int len, int flags) {
    net_sock *s = ns_get(fd);
    if (!s) {
        errno = DARWIN_EBADF;
        return -1;
    }
    hexlog("send", fd, buf, len);
    // ponytail: asume un paquete completo por send (así lo hace el bucle de 0x5eed8); juntar si llegan partidos.
    const uint8_t *b = buf;
    for (int o = 0; o + 4 <= len;) {
        int plen = b[o] | b[o + 1] << 8, cmd = b[o + 2] | b[o + 3] << 8;
        if (plen < 4 || o + plen > len)
            break;
        l_info("[EMU] C>S 0x%03x (%d bytes)", cmd, plen);
        emu_handle(s, cmd, b + o + 4, plen - 4);
        o += plen;
    }
    return len;
}
static int n_recv(int fd, void *buf, int len, int flags) {
    net_sock *s = ns_get(fd);
    if (!s) {
        errno = DARWIN_EBADF;
        return -1;
    }
    if (s->qlen) {
        int n = len < s->qlen ? len : s->qlen;
        memcpy(buf, s->q, n);
        memmove(s->q, s->q + n, s->qlen - n);
        s->qlen -= n;
        return n;
    }
    if (!s->nonblock)
        sceKernelDelayThread(50 * 1000);
    errno = DARWIN_EAGAIN;
    return -1;
}
static int n_close(int fd) {
    net_sock *s = ns_get(fd);
    if (!s)
        return close(fd);
    l_info("[NET] close fd %d (%s:%d)", fd, s->host, s->port);
    s->used = 0;
    return 0;
}
// select: los sockets falsos siempre se pueden escribir y nunca tienen datos para leer.
static int n_select(int n, uint32_t *r, uint32_t *w, uint32_t *e, void *t) {
    int ready = 0;
    for (int fd = 0; fd < n; fd++) {
        uint32_t bit = 1u << (fd & 31);
        if (r && (r[fd >> 5] & bit)) {
            net_sock *s = ns_get(fd);
            if (s && s->qlen)
                ready++;
            else
                r[fd >> 5] &= ~bit;
        }
        if (e && (e[fd >> 5] & bit))
            e[fd >> 5] &= ~bit;
        if (w && (w[fd >> 5] & bit)) {
            if (ns_get(fd))
                ready++;
            else
                w[fd >> 5] &= ~bit;
        }
    }
    if (!ready)
        sceKernelDelayThread(16 * 1000);
    return ready;
}

const import_entry patch_imports[] = {
    F(SCNetworkReachabilityCreateWithAddress, re_create), F(SCNetworkReachabilityCreateWithName, re_create),
    F(SCNetworkReachabilityGetFlags, re_flags), F(SCNetworkReachabilityScheduleWithRunLoop, re_one),
    F(SCNetworkReachabilitySetCallback, re_one), F(SCNetworkReachabilityUnscheduleFromRunLoop, re_one),
    F(getaddrinfo, n_getaddrinfo), F(freeaddrinfo, n_freeaddrinfo), F(gethostbyname, n_gethostbyname),
    F(inet_ntoa, n_inet_ntoa), F(socket, n_socket), F(connect, n_connect), F(setsockopt, n_ok),
    F(getsockopt, n_ok), F(shutdown, n_ok), F(fcntl, n_fcntl), F(send, n_send), F(recv, n_recv),
    F(close, n_close), F(select, n_select),
};
const int patch_imports_count = sizeof(patch_imports) / sizeof(patch_imports[0]);

// ---------------------------------------------------------------- Gamevil Live fuera
// Port de tools/iphone/z5nolive.m (tweak probado en iPhone con jailbreak): live.gamevil.com ya no existe
// y la ventana de Gamevil Live ("crear ID") queda en bucle. Se anulan GamevilNeo y la fachada gamevilLive.
static id gv_nil(id self, SEL _c) { return NULL; }
static int gv_yes(id self, SEL _c) { return 1; }
static int gv_no(id self, SEL _c) { return 0; }
static id gv_login(id self, SEL _c) { return ns_str("jugador01"); }
// Sin tienda en la Vita: -[ZN5_IAP requestProductData:/buyProduct:] (0x165473/0x165663) muestran
// "In-app purchases are not allowed." cuando el estado [global+0x8374] no es 2; se anulan.
static void iap_noop(id self, SEL _c, int product) { l_info("IAP: %s(%d) anulado", _c, product); }

void patch_objc(void) {
    static const struct {
        const char *cls;
        int meta;
        const char *sel;
        void *imp;
    } t[] = {
        {"GamevilNeo", 0, "isLogined", gv_yes},       {"GamevilNeo", 0, "IsLiveDlg", gv_no},
        {"GamevilNeo", 0, "IsDlg", gv_no},            {"GamevilNeo", 0, "checkLogin", gv_nil},
        {"GamevilNeo", 0, "requestLogin", gv_nil},    {"GamevilNeo", 0, "NeoGoLive", gv_nil},
        {"GamevilNeo", 0, "OpenGamevilLive:", gv_nil}, {"GamevilNeo", 0, "ShowWebView", gv_nil},
        {"GamevilNeo", 0, "setVisible:", gv_nil},     {"GamevilNeo", 0, "getLoginId", gv_login},
        {"GamevilNeo", 0, "getLoginString", gv_login}, {"gamevilLive", 1, "IsLogined", gv_yes},
        {"gamevilLive", 1, "CheckLogin", gv_nil},     {"gamevilLive", 1, "RequestLogin", gv_nil},
        {"gamevilLive", 1, "GoGamevilLive", gv_nil},
        {"ZN5_IAP", 0, "requestProductData:", iap_noop}, {"ZN5_IAP", 0, "buyProduct:", iap_noop},
    };
    int ok = 0;
    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        if (objc_override(t[i].cls, t[i].meta, t[i].sel, t[i].imp))
            ok++;
        else
            l_error("Gamevil Live: no existe %c[%s %s]", t[i].meta ? '+' : '-', t[i].cls, t[i].sel);
    }
    l_info("Gamevil Live e IAP anulados (%d/%d métodos)", ok, (int)(sizeof(t) / sizeof(t[0])));
}

void patch_init(void) {
    l_info("Initializing game-specific patches...");
    // Conexión al servidor: tras lanzar el hilo de connect el juego espera en pasos de
    // [NSThread sleepForTimeInterval:1.0] (0x5ede0, hasta 30). Con el servidor emulado el connect es
    // instantáneo y cada conexión perdía 1 s entero (~5 s del título al menú): paso de 0.125 s.
    // vmov.f64 d16, #1.0 (f7 ee 00 0b) -> #0.125 (f4 ee 00 0b).
    static const uint8_t vmov_0125[4] = {0xf4, 0xee, 0x00, 0x0b};
    if (macho_patch_text(0x5ede0, vmov_0125, 4))
        l_error("parche de espera de conexión (0x5ede0) no aplicado");
    // Place macho_hook(...) calls here to redirect internal guest functions
}

void patch_frame(void) {
    // Per-frame logic if required by target engine
}
