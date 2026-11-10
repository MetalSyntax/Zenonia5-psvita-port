#!/usr/bin/env python3
"""MainWindow.nib (NSKeyedArchiver binario de iPhone OS 2/3) -> nib.txt para uikit.c.

Solo el patrón de las plantillas de Xcode de la época: File's Owner (UIApplication) -> delegate,
delegate.window -> UIWindow, delegate.<outlet> -> vista (UIClassSwapper, p. ej. EAGLView).
Salida (ux0:data/<slug>/nib.txt):
    delegate <Clase>
    window <w> <h>
    view <Clase> <w> <h>
    outlet <origen> <label> <destino>      (roles: owner delegate window view)
Uso: python3 -I nib_prep.py <MainWindow.nib> <nib.txt>
"""
import plistlib, re, sys


def main():
    src, out = sys.argv[1:3]
    p = plistlib.load(open(src, 'rb'))
    o = p['$objects']

    def R(x):
        return o[x.data] if isinstance(x, plistlib.UID) else x

    def cls(obj):
        return R(obj['$class'])['$classname'] if isinstance(obj, dict) and '$class' in obj else None

    def size(obj):
        m = re.match(r'\{\{.*?\}, \{([\d.]+), ([\d.]+)\}\}', R(obj.get('UIBounds', '{{0, 0}, {320, 480}}')))
        return (float(m.group(1)), float(m.group(2))) if m else (320.0, 480.0)

    role = {}
    lines = []
    for i, obj in enumerate(o):
        c = cls(obj)
        if c == 'UIProxyObject' and R(obj.get('UIProxiedObjectIdentifier')) == 'IBFilesOwner':
            role[i] = 'owner'
        elif c == 'UIClassSwapper' and R(obj.get('UIOriginalClassName')) == 'UICustomObject':
            role[i] = 'delegate'
            lines.append('delegate %s' % R(obj['UIClassName']))
        elif c == 'UIWindow':
            role[i] = 'window'
            lines.append('window %g %g' % size(obj))
        elif c == 'UIClassSwapper' and R(obj.get('UIOriginalClassName')) in ('UIView', 'UIImageView'):
            role[i] = 'view'
            lines.append('view %s %g %g' % ((R(obj['UIClassName']),) + size(obj)))
    for obj in o:
        if cls(obj) == 'UIRuntimeOutletConnection':
            s, d = obj['UISource'].data, obj['UIDestination'].data
            if s in role and d in role:
                lines.append('outlet %s %s %s' % (role[s], R(obj['UILabel']), role[d]))
    # el orden importa: delegate y window antes que la vista (se agrega a la ventana)
    order = {'delegate': 0, 'window': 1, 'view': 2, 'outlet': 3}
    lines.sort(key=lambda l: order[l.split()[0]])
    open(out, 'w').write('\n'.join(lines) + '\n')
    print('\n'.join(lines))


if __name__ == '__main__':
    main()
