#!/usr/bin/env python3
"""Genera source/cxx_imports.c: imports C++ del juego iOS (libstdc++ de GCC 4.2, ABI COW) ->
símbolos de la libstdc++/libsupc++ de VitaSDK (ABI vieja "_ZNSs", que sigue compilada).

En iOS 32 bits size_t es `unsigned long` (mangling 'm'); en la Vita es `unsigned int` ('j'). Se
compara por nombre desmanglado con "unsigned long" -> "unsigned int" y se emite la tabla con el
nombre iOS (con su '_' extra de Mach-O) apuntando al símbolo de la Vita.

Uso: python3 -I gen_cxx.py <report.txt de ios_prep> <salida.c>
"""
import os, re, subprocess, sys

VITASDK = os.environ.get('VITASDK', os.path.expanduser('~/vitasdk'))
LIBS = [VITASDK + '/arm-vita-eabi/lib/libstdc++.a', VITASDK + '/arm-vita-eabi/lib/libsupc++.a']
NM = VITASDK + '/bin/arm-vita-eabi-nm'
FILT = VITASDK + '/bin/arm-vita-eabi-c++filt'


def demangle(names):
    if not names:
        return []
    out = subprocess.run([FILT], input='\n'.join(names), capture_output=True, text=True).stdout.splitlines()
    return out


def norm(s):
    s = s.replace('unsigned long', 'unsigned int')
    return re.sub(r'\s+', ' ', s).strip()


def main():
    report, out = sys.argv[1:3]
    ios = []
    in_imp = False
    for line in open(report):
        if line.startswith('# imports'):
            in_imp = True
            continue
        if in_imp and line.startswith('__Z'):
            ios.append(line.split()[0])
    vita = {}
    for lib in LIBS:
        for line in subprocess.run([NM, lib], capture_output=True, text=True).stdout.splitlines():
            p = line.split()
            if len(p) == 3 and p[1] in 'TWVDRBu' and p[1] != 'u':
                vita.setdefault(p[2], p[1])
    vnames = sorted(vita)
    vdem = dict(zip(vnames, demangle(vnames)))
    by_dem = {}
    for n, d in vdem.items():
        by_dem.setdefault(norm(d), n)
    idem = demangle([n[1:] for n in ios])  # sin el '_' de Mach-O
    rows, missing = [], []
    for n, d in zip(ios, idem):
        v = n[1:] if n[1:] in vita else by_dem.get(norm(d))
        if v:
            rows.append((n, v, vita[v] in 'VDRB', d))
        else:
            missing.append((n, d))
    with open(out, 'w') as f:
        f.write('// Generado por tools/gen_cxx.py: imports C++ del juego -> libstdc++ de VitaSDK.\n')
        f.write('// No editar a mano (volver a correr gen_cxx.py con el report.txt de ios_prep.py).\n')
        f.write('#include <string.h>\n\n#include "imports.h"\n\n')
        for i, (n, v, data, d) in enumerate(rows):
            f.write('extern char cxx_%d __asm__("%s"); // %s\n' % (i, v, d))
        f.write('\nconst import_entry cxx_imports[] = {\n')
        for i, (n, v, data, d) in enumerate(rows):
            f.write('    {"%s", (uintptr_t)&cxx_%d, %d},\n' % (n, i, 1 if data else 0))
        if not rows:
            f.write('    {"", 0, 0},\n')
        f.write('};\nTABLE_END(cxx)\n\n')
        f.write('// Lo que no tiene equivalente queda en el log como "unimpl".\n')
        f.write('const import_entry *cxx_import(const char *name) { (void)name; return NULL; }\n')
        for n, d in missing:
            f.write('// FALTA %s  (%s)\n' % (n, d))
    print('C++: %d resueltos, %d sin equivalente' % (len(rows), len(missing)))
    for n, d in missing:
        print('  FALTA', d)


if __name__ == '__main__':
    main()
