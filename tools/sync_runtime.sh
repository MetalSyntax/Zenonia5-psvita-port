#!/bin/sh
# Copia el runtime iOS común (loader Mach-O + ObjC + Foundation/UIKit/GLES/OpenAL/AudioToolbox) de
# este port a otro de la misma familia, sin tocar lo propio del destino (port_config.h, patch.c).
# Regenera cxx_imports.c para el ejecutable del destino.
# Uso: tools/sync_runtime.sh <ruta-del-port-destino>
set -e
SRC="$(cd "$(dirname "$0")/.." && pwd)"
DST="$(cd "$1" && pwd)"
[ "$SRC" = "$DST" ] && { echo "origen = destino"; exit 1; }
[ -f "$DST/source/port_config.h" ] || { echo "$DST/source/port_config.h no existe (crearlo primero)"; exit 1; }
for f in "$SRC"/source/*.c "$SRC"/source/*.h "$SRC"/source/*.S; do
  b=$(basename "$f")
  case "$b" in port_config.h|patch.c|cxx_imports.c) continue ;; esac
  cp "$f" "$DST/source/$b"
done
[ -f "$DST/source/patch.c" ] || cp "$SRC/source/patch.c" "$DST/source/patch.c"
for t in ios_prep.py gen_cxx.py nib_prep.py stage_data.sh deploy_data.py controls.example.txt annotate.py macho_prep.py \
         cgbi_check.py printf_check.py sync_runtime.sh; do
  cp "$SRC/tools/$t" "$DST/tools/$t"
done
# CMakeLists: el del origen con nombre/TITLEID/slug del destino (de .psvita-toolkit.json)
python3 -I - "$SRC" "$DST" <<'PY'
import json, sys
src, dst = sys.argv[1:3]
s = json.load(open(src + '/.psvita-toolkit.json')); d = json.load(open(dst + '/.psvita-toolkit.json'))
t = open(src + '/CMakeLists.txt').read()
t = t.replace('project(%s ' % s['slug'], 'project(%s ' % d['slug'])
t = t.replace('set(VITA_APP_NAME "%s")' % s['game_name'], 'set(VITA_APP_NAME "%s")' % d['game_name'])
t = t.replace('set(VITA_TITLEID "%s")' % s['titleid'], 'set(VITA_TITLEID "%s")' % d['titleid'])
t = t.replace('set(VITA_VPKNAME "%s")' % s['slug'], 'set(VITA_VPKNAME "%s")' % d['slug'])
open(dst + '/CMakeLists.txt', 'w').write(t)
PY
[ -f "$DST/vendor/vitaGL/libvitaGL.a" ] || cp "$SRC/vendor/vitaGL/libvitaGL.a" "$DST/vendor/vitaGL/"
sed -i '' 's#^Makefile$#/Makefile#' "$DST/.gitignore"
echo "runtime sincronizado: $SRC -> $DST"
