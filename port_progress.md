# Zenonia 5 (iOS) — Progreso del port

## 2026-11-10 — Repo creado (offline, sin consola)
- `psvita-toolkit init --ipa` (slice armv7 del binario fat) + runtime iOS maestro de Ferrari (`tools/sync_runtime.sh`).
- Maestro Ferrari: soporte `LC_MAIN` en `macho.c` (binarios iOS 6+ sin `_main`), sincronizado aquí.
- `ios_prep.py --arch armv7`: 33785 rebase, 2889 binds / 689 símbolos; C++ 39/0. Build debug OK.
- Imports sin cubrir: 156 (`docs/imports_sin_cubrir.txt`). GLES1: la ruta gráfica de Ferrari aplica directo.
- `font.ttf` (NanumGothic de Zenonia 4) copiado para el futuro puente CGFont/CoreText.
- Port Android movido a `../Incompletes/` como histórico; lo aprovechable está en PORTING_PLAN §2.
- **Próximo:** fase 1 del plan → primer arranque en consola.
