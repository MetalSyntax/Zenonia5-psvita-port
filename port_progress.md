# Zenonia 5 (iOS) — Progreso del port

## 2026-11-10 — Repo creado (offline, sin consola)
- `psvita-toolkit init --ipa` (slice armv7 del binario fat) + runtime iOS maestro de Ferrari (`tools/sync_runtime.sh`).
- Maestro Ferrari: soporte `LC_MAIN` en `macho.c` (binarios iOS 6+ sin `_main`), sincronizado aquí.
- `ios_prep.py --arch armv7`: 33785 rebase, 2889 binds / 689 símbolos; C++ 39/0. Build debug OK.
- Imports sin cubrir: 156 (`docs/imports_sin_cubrir.txt`). GLES1: la ruta gráfica de Ferrari aplica directo.
- `font.ttf` (NanumGothic de Zenonia 4) copiado para el futuro puente CGFont/CoreText.
- Port Android movido a `../Incompletes/` como histórico; lo aprovechable está en PORTING_PLAN §2.
- **Próximo:** fase 1 del plan → primer arranque en consola.

## 2026-11-11 — Fase 1 del plan (offline): cobertura del runtime + texto
- **Texto** (`source/coretext.c`, nuevo): CGFont/CTFont/UIFont + `NSString` UIStringDrawing (`sizeWithFont:*`, `drawInRect:withFont:*`, `drawAtPoint:withFont:`) sobre FreeType con `font.ttf` (NanumGothic, va en el VPK como `app0:font.ttf`; respaldo `ux0:data/zenonia5ios/font.ttf`). Todas las fuentes pedidas → esa. Glifo con borde (`CTFontCreatePathForGlyph` + `FillPath`/`DrawPath`) con `FT_Stroker`. Clientes confirmados con otool: `CCImage::initWithString` de cocos2d-x (0x2525e), gestor de fuentes C++ de CCGX (0x65d2e–0x66410), FontLabel/ZFont. Prueba en la Mac: `tools/coretext_hosttest.c` (layout centrado en 2 líneas, coreano visible, borde) → OK.
- `uikit.c`: `UIGraphicsPush/PopContext` con pila real (antes no-op: `drawInRect:` no tenía contexto), `UIRectFill`, helpers `cg_ctx_map`/`cg_ctx_cover`/`ui_current_ctx` para coretext.c; `UIFont` se define en coretext.c.
- `foundation.c`: CFArray/CFData/CFDictionary/CFString(bytes, compare, UTF-16)/CFNumberCreate/CFHash/CFURLCopyPathExtension/NSUnionRange, `kCFCoreFoundationVersionNumber`=793 (iOS 6.1), `CFStreamCreatePairWithSocketToHost` = sin red. **Bug del maestro corregido**: `CFNumberGetValue` tenía mal el mapa de CFNumberType (12=Float escribía un double).
- `ios5.c`: zlib (`gzopen` con rutas mapeadas, `gzread/gzclose/crc32/uncompress/inflateInit2_`), `asprintf`, `__vsnprintf_chk`, `bcopy`, `reallocf`, `memset_pattern8`, `backtrace*`, GL `glDiscardFramebufferEXT`/MSAA APPLE, objc `object_setClass`/`objc_getMetaClass`/`objc_getRequiredClass`/`class_isMetaClass`/`class_getClassMethod`/`objc_setProperty_nonatomic`.
- Imports sin cubrir: 156 → **39** (`docs/imports_sin_cubrir.txt`), todos a propósito en el thunk `unimpl` (loguea, devuelve 0) o bloque de datos en cero:
  - AddressBook `AB*`/`kABPerson*` (FriendDataManager, invitar amigos), `hash_create/hash_search`, `if_nameindex`, `objc_*ClassPair`, `class_addProperty/addProtocol/replaceMethod`, `method_get/setImplementation`, `property_copyAttributeList`, `object_getIvar/IndexedIvars`, `objc_constructInstance/copyClassNamesForImage/getProtocol`, `protocol_getName` (SDKs, anti-crack `gamevilCircle checkDevice`: revisa el retorno de `dladdr`, seguro).
  - `kCFNull`, `kCFAllocatorNull`, `kCFType*CallBacks` (datos en cero; `[NSNull null]` también es nil en este runtime).
  - Falsos positivos / sin llamadas: `__gxx_personality_v0`, `_Unwind_Resume` (el juego usa SjLj), `OBJC_EHTYPE_$_NSException`, `__sincos(f)_stret` (0 llamadas), `dyld_stub_binder` (binds ya resueltos por ios_prep).
- Build debug OK (`build/zenonia5ios.vpk`, con `font.ttf`). `ux0_data/zenonia5ios/` armado con `tools/stage_data.sh`.

### Agregados del runtime a subir al maestro (Ferrari-GT-Evolution-vita/source) después
No se tocó el maestro ni se corrió `sync_runtime.sh` (otro agente lo está editando). Al sincronizar, **no pisar** estos cambios locales; subirlos (o fusionarlos con el CoreText que haga el otro agente para Six Guns):
1. `source/coretext.c` (nuevo) + `CMakeLists.txt` (`source/coretext.c`, `FILE font.ttf font.ttf`), `imports.c` (tabla `coretext` justo después de `patch`), `imports.h` (`IMPORT_TABLE(coretext)`), `main.c` (`coretext_init()` después de `uikit_init()`). Si el otro agente ya trae su CoreText: quedarse con uno solo (los nombres de import chocan; gana el primero de la tabla).
2. `uikit.c`: pila de `UIGraphicsPush/PopContext`, `UIRectFill`, `cg_ctx_map`/`cg_ctx_cover`/`px_alpha`/`ui_current_ctx`, quitar `UIFont`/`uifont_c` (ahora en coretext.c) y las entradas no-op `CGContextBeginPath/FillPath/SetLineWidth`.
3. `foundation.c`/`foundation.h`: bloque "CF de Zenonia 5", `ns_data_new()`, fix de `CFNumberGetValue`, prefijo `_GK` y sufijos nuevos en `const_string_import`.
4. `ios5.c`: bloque "Zenonia 5: libc/zlib/GL/objc que faltaban" + sus entradas de tabla (incluye `#include <zlib.h>`, `darwin.h`, `darwin_printf.h`).

## PENDIENTE EN CONSOLA (corrida iOS #1)

1. Subir los datos: `python3 -I tools/deploy_data.py` (sube `ux0_data/zenonia5ios/` → `ux0:data/zenonia5ios/`: `game_app/` (349 MB en total) con `.ogg` junto a cada `.caf`/`.mp3`, `rebase.bin`, `binds.bin`, `nibs/`, `controls.txt`).
2. Instalar `build/zenonia5ios.vpk` (TITLEID `PSVZ5I001`; convive con el Android `PSVZ00005`). Requiere kubridge.
3. Abrir el juego, traer logs y dump con `python3 -I tools/deploy_data.py --logs`. Decir si crasheó, quedó congelado o en negro.
4. Qué mirar en `logs/zenonia5ios_001.log`, en orden (gate de la fase 2):
   - `coretext: font.ttf NanumGothic, 13297 glifos` (si no, `fatal_error` por font.ttf).
   - entrada por **LC_MAIN** → `UIApplicationMain` → `AppController` → `applicationDidFinishLaunching: volvió`.
   - líneas `unimpl:` y `objc: ... sin implementar`: anotar las primeras antes del crash (una por corrida).
5. En un crash: PC − 0x98000000 = dirección en el Mach-O (`otool -arch armv7 -tV game_app/Zenonia5`); dirección de datos < 0x02000000 = rebase faltante (`ux0_data/zenonia5ios/report.txt`). Binario sin símbolos: nombrar funciones cruzando cadenas con `libgameDSO.so` del Android.
6. Si llega a imagen: confirmar orientación con SELECT+START (2 s) y fijar `PORT_ROTATION`; si el texto sale invertido/corrido, es el mapeo de `cg_ctx_map` (coretext.c asume CTM sin escala).

### Riesgos de la corrida #1
- **CCGXMainFrameBuffer / UpdateWIPIBuffer** (framebuffer de CPU "WIPI" subido como textura): principal riesgo de rendimiento (Zenonia 4 quedó en 15-20 FPS por esto). Medir en la fase 7.
- Texto: todo pasa por `drawInRect:withFont:` (UIFont siempre existe, así que FontLabel/ZFont no debería usarse). Si el juego mide texto con un tamaño y dibuja con otro, cortes de línea distintos a iOS. Escala/rotación del CTM no se aplica a glifos.
- JSON (GVJK/JSONKit) crea objetos con `object_setClass` sobre memoria propia; solo se usa en red/Gamevil Live, que está sin red. Si se usa al arrancar (config), es el sospechoso.
- `controls.txt` sale del ejemplo genérico (coordenadas 480x320); Zenonia 5 es 568x320 puntos: ajustar después de ver la UI.
- Audio (fase 5): `sound/NNN.caf` y `sound/NNN.mp3` comparten nombre y `stage_data.sh`/AudioFile usan `NNN.ogg` para ambos: quedó una sola conversión por número (150 `.ogg`). Si SFX y BGM suenan cambiados, separar los nombres (p. ej. `NNN.caf.ogg`) en `stage_data.sh` + `audio.c`.
