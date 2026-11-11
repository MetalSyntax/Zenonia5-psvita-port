#pragma once
// Configuración propia de este juego (el resto de source/ es común a los ports iOS; se sincroniza
// con tools/sync_runtime.sh desde Ferrari-GT-Evolution-vita).
#define PORT_NAME "Zenonia 5"
#define PORT_SLUG "zenonia5ios"
#define PORT_EXEC "Zenonia5"             // CFBundleExecutable (fat armv7+arm64: se carga el slice armv7)
#define PORT_ROTATION 0                // Info.plist: LandscapeRight; confirmar en consola con SELECT+START
#define PORT_STRETCH 0
#define PORT_SYSTEM_VERSION "6.1.3"    // MinimumOSVersion 6.1; iOS 6 evita caminos de iOS 7+
#define PORT_DEVICE_MACHINE "iPhone5,1" // Default-568h@2x: 16:9 ≈ Vita
#define GLES_POOL_MB 32
#define PORT_ACCEL_FROM_STICK 0
#define PORT_SCREEN_W 320
#define PORT_SCREEN_H 568
#define PORT_SCREEN_SCALE 2.0f
#define PORT_STAT_INO64 1      // _stat con layout ino64 (st_size en +0x3c, visto en 0x5e34a)
#define PORT_LANDSCAPE_VIEWS 1 // EAGLView 568x320 como tras la rotación del RootViewController de iOS 6
// Controles táctiles del juego ocultos (L+R los muestra/oculta): sprites del atlas com/Keypad.png que caen
// fuera de estas franjas visibles no se dibujan. Rects en píxeles del FBO 568x320, origen abajo-izquierda:
// arriba todo el ancho (vida, SKIP, tienda, pausa, minimapa) y abajo el centro; quedan fuera el joystick
// (abajo-izq) y el ataque + habilidades (abajo-der).
#define PORT_HUD_TEX "/com/Keypad.png"
#define PORT_HUD_KEEP {{0, 140, 568, 180}, {150, 0, 265, 140}}
