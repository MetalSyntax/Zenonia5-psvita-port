# Zenonia 5 (iOS) → PS Vita

> Port plan (psvita-toolkit `init --ipa`, 2026-11-10). Source: iOS IPA, 32-bit Mach-O (armv7 slice of a fat
> armv7+arm64 binary), loaded on the Vita by the shared iOS runtime (Mach-O loader + ObjC runtime; master copy
> in `Ferrari-GT-Evolution-vita/source`, synced with `tools/sync_runtime.sh`).
>
> **For AI agents:** source of truth for this port. Tick `[x]` for console items only with evidence
> (log line, dump, commit). Never record an engine fact without symbol/disassembly evidence. Keep in English.
> This is NOT an Android port: no `.so`, no JNI, no FalsoJNI. One console bug per run, guided by `logs/zenonia5ios_NNN.log`.
>
> **Historical Android port:** `../Incompletes/Zenonia5-vita` (soloader, APK 1.2.1, VPK built 2026-11-06,
> never run on console). **Sibling reference:** `../Zenonia4-vita` (Android, released, console-proven).

## 0. Game

| | |
|---|---|
| IPA | `zenonia-5-v1.1.3.ipa` (v1.1.3, `com.gamevil.zenonia5free`, MinimumOSVersion 6.1, 48 MB / 620 MB extracted) |
| Executable | `Zenonia5`, fat armv7+arm64 → **armv7 slice**, PIE, cryptid 0, stripped (no `_main`, entry via **LC_MAIN**) |
| TITLEID / slug | `PSVZ5I001` / `zenonia5ios` (Android keeps `PSVZ00005`) |
| Engine | Gamevil Nexus2 logic + **cocos2d-x (iOS) GLES1** through Gamevil's CCGX/ccpzx layer, all statically linked. ObjC shell: `AppController`, `RootViewController`, `EAGLView`, `CCDirectorCaller`, CocosDenshion (`CDLongAudioSource`). |
| Graphics | **GLES1** (`glMatrixMode`, `glVertexPointer`, …), 60 GL imports: the Ferrari runtime's GLES1 path applies directly. Extra: `glDiscardFramebufferEXT`, MSAA APPLE. |
| Text | CoreGraphics/CoreText glyph rendering (`CGFontCreateWithFontName("AppleGothic"/"Helvetica-Bold"/…)`, `CGContextShowGlyphsAtPoint`, `CTFont*`) for `CCGXCachedFont`/`CCGXCharCache`. No TTF in the bundle → back it with `font.ttf` (NanumGothic, the font from Z4, OFL, Korean + Latin; already in the repo). |
| Data | `*.zt1` tables (749), `*.pzc` sprite packs (514), PNG; loose files in the bundle |
| Audio | `sound/%03d.caf` (150 `.caf`) + 149 `.mp3`: CocosDenshion → OpenAL / AVAudioPlayer |
| Orientation | `LandscapeRight`, `Default-568h@2x` (16:9) |

## 1. Bootstrap state (2026-11-10, offline)

- [x] `psvita-toolkit init --ipa` → repo, `decompiled/macho/` dumps (Ghidra skipped).
- [x] Ferrari master runtime synced; `source/port_config.h` (iPhone5,1 / iOS 6.1.3 / 320x568@2x).
- [x] **Master runtime fix:** `macho.c` now reads `LC_MAIN` (iOS 6+ binaries), so stripped executables without `_main` boot via `entryoff`. Done in Ferrari, then synced here.
- [x] `tools/ios_prep.py --arch armv7`: PIE exact → **33785 rebase words, 2889 bind slots / 689 symbols**.
- [x] `tools/gen_cxx.py`: 39 libstdc++ imports resolved, 0 missing.
- [x] `psvita-toolkit build --preset debug` → `build/zenonia5ios.vpk` builds.
- [x] ~~156 imports not covered~~ → **39 left** after phase 1 (2026-11-11), all deliberately left to the logging `unimpl` thunk/zero data block: `docs/imports_sin_cubrir.txt`, classified in `port_progress.md`.

## 2. What carries over from Android (Zenonia5-vita) and Zenonia 4

| Android finding | iOS |
|---|---|
| Load chain `libcocosdenshion.so` → `libcocos2d.so` → `libgameDSO.so`, symbol lookups across 3 `.so` | **Gone**: one static Mach-O. |
| FalsoJNI tables: 105 `ccgx*`/`osl*`/`DLC*`/`Cocos2dxActivity` methods | **Gone**: the same calls are direct C++ or ObjC inside the binary. Only Apple frameworks need shims. |
| `gfa.c` + `font.c` (GFA font bridge, text widths, wordwrap estimates) + `ksc5601_table.h` | **Partially reusable**: on iOS the font bridge is CGFont/CTFont. Reuse the FreeType + glyph metrics code from `font.c` to implement `CGFontGetGlyphAdvances/BBoxes`, `CTFontGetGlyphsForCharacters`, `CGContextShowGlyphsAtPoint` over `font.ttf`. The KSC5601 table is only needed if the iOS build still passes KS C 5601 text (check: iOS strings are probably UTF-16 via NSString). |
| `audio.c` (Z4 Tremor OGG mixer, keyed by path) | Not needed: CocosDenshion uses OpenAL + ExtAudioFile/AVAudioPlayer, already in the runtime; `stage_data.sh` converts `.caf`/`.mp3` → `.ogg`. |
| Clet HAL key codes (Z4) for D-pad/buttons | **To verify**: the CCGX layer (`CCGXWipiCletLayer`) still has a key path (`keyBackClicked`). If iOS never feeds keys, fall back to synthesized touches on the on-screen pad (`controls.txt`). |
| Saves `saves/`, `files/` | iOS sandbox `Documents/` + `Library/` (runtime handles). |
| Online/IAP return "unavailable" (`netBillcomSocketConnect`=0, DLC*=0) | Same idea through StoreKit/GameKit/Facebook stubs (auto empty classes). |
| **Zenonia 4 lesson:** 100% CPU framebuffer rendering → ~15-20 FPS ceiling after 134 phases | Z5 draws sprites as GPU textures, but `CCGXMainFrameBuffer` / `UpdateWIPIBuffer` (a CPU "WIPI" framebuffer uploaded as a texture) also exists in this binary. **Measure early how much of the frame goes through it**: it is the main performance risk. |

## 3. Phases

### Phase 1: Runtime coverage for this binary (offline)
- [x] **CGFont/CoreText glyph path** (2026-11-11, `source/coretext.c`, FreeType over `font.ttf`, packed in the VPK as `app0:font.ttf`; every font name maps to it). Evidence of the clients (otool, armv7): `CCImage::initWithString` at 0x2525e (UIFont `fontWithName:size:` → `sizeWithFont:constrainedToSize:` → `drawInRect:withFont:lineBreakMode:alignment:`; one-glyph stroke branch with `CTFontCreatePathForGlyph` at 0x257f2), CCGX C++ font manager at 0x65d2e–0x66410 (CTFont metrics, `CTFontGetGlyphsForCharacters`/`GetAdvancesForGlyphs`), FontLabel/ZFont (`CGContextShowGlyphsAtPoint` 0x2a38c, cmap via `CGFontCopyTableForTag`). Host test: `tools/coretext_hosttest.c`. Kept local (not in the Ferrari master yet).
- [x] CoreGraphics text/path calls, `CGDataProviderCreateWithURL`, `CGRectNull`, `UIRectFill`, `UIGraphicsPush/PopContext` (real stack now). Color-with-CGColor calls are no-ops (UIColor carries no color in this runtime; cocos draws text white).
- [x] CoreFoundation (`foundation.c`): `CFData*`, `CFArrayCreate/GetValues`, `CFDictionary*`, `CFString*` bytes/encodings, `CFNumberCreate` (+ fixed `CFNumberGetValue` type map), `CFHash`, `CFURLCopyPathExtension`, `NSUnionRange`, `kCFCoreFoundationVersionNumber`=793; `CFStreamCreatePairWithSocketToHost` = no network.
- [x] libc/zlib (`ios5.c`): `gzopen` (path-mapped)/`gzread/gzclose/crc32/uncompress/inflateInit2_`, `asprintf`, `__vsnprintf_chk`, `bcopy`, `reallocf`, `memset_pattern8`, `backtrace*`. `__sincos(f)_stret` have 0 call sites: left.
- [x] ObjC runtime: `object_setClass` (GVJK/JSONKit), `objc_getMetaClass`, `objc_getRequiredClass`, `class_isMetaClass`, `class_getClassMethod`, `objc_setProperty_nonatomic`. Swizzling/dynamic-class calls (`method_*Implementation`, `class_replaceMethod`, `objc_*ClassPair`, `class_addProperty/Protocol`) stay as logging `unimpl` → 0: only `gamevilCircle checkDevice` (anti-crack, checks `dladdr` result → safe) and the GamevilWebView C helper use them.
- [x] Constants: NSString constants via `const_string_import` (added `_GK` prefix and Ambient/Playback/PlayAndRecord/Code/Fade/Label/Attributes suffixes).
- [x] GL: `glDiscardFramebufferEXT` no-op, `glRenderbufferStorageMultisampleAPPLE` → `glRenderbufferStorageOES`, resolve no-op.
- [x] Rerun the coverage grep (39 left, see above). Build debug + VPK OK.

### Phase 2: First console boot (GATE: log reaches the delegate)
- [ ] `tools/stage_data.sh` (620 MB, `.caf`/`.mp3` → `.ogg`) + `python3 -I tools/deploy_data.py`; install the VPK.
- [ ] Log: `entry` printed as LC_MAIN main → `UIApplicationMain` → `AppController` → `applicationDidFinishLaunching: volvió`.
- [ ] Crash → `psvita-toolkit analyze`; PC − 0x98000000 in `otool -arch armv7 -tV game_app/Zenonia5`. Binary is stripped: name functions by matching strings/constants against the Android `libgameDSO.so` (6,526 exported symbols).

### Phase 3: Graphics (GATE: Gamevil logo / title)
- [ ] `EAGLView` renderbuffer 1136x640 → 960x544; orientation via SELECT+START, then hard-code `PORT_ROTATION`.
- [ ] `.pzc` sprites and `.png` textures (CgBI handled) visible.
- [ ] Text visible (title menu, dialogs, Korean names).

### Phase 4: Input
- [ ] Touch → `touchesBegan/Moved/Ended` → cocos2d (menus respond).
- [ ] D-pad/stick/buttons: test the CCGX key path first; otherwise `controls.txt` (touch synthesis on the virtual pad and skill buttons).

### Phase 5: Audio
- [ ] `sound/NNN.caf` SFX via OpenAL; BGM `.mp3` → `.ogg` via AVAudioPlayer/`CDLongAudioSource`; loops OK.

### Phase 6: Saves, language, offline
- [ ] Saves persist in `Documents/`; language from `AppleLanguages`.
- [ ] Gamevil Live / Facebook / IAP / ads fail offline without blocking the title screen.

### Phase 7: Performance (the reason for going iOS)
- [ ] Measure FPS in town and in a crowded fight (same protocol as `Zenonia4-vita/plan_rendimiento_z4_z5.md`).
- [ ] Count the frame time spent in `CCGXMainFrameBuffer::UpdateWIPIBuffer` / texture upload vs GPU sprites. If the CPU framebuffer dominates, apply Z4's lessons (multi-core, dirty rectangles) only there.

### Phase 8: Release
- [ ] LiveArea from the IPA icons (`Zicon_180.png`, `Default-568h@2x.png`) with `psvita-toolkit livearea --validate` (the Android repo only had placeholders).
- [ ] README (psvita-readme skill), trophies (optional, psvita-trophies skill).

## 4. Known risks
- CPU "WIPI" framebuffer path (section 2, last row).
- Text pipeline is new runtime surface (CGFont/CoreText).
- Stripped binary: triage depends on cross-matching the Android `.so` symbols.
