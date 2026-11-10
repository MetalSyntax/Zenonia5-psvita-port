#!/usr/bin/env python3
"""Sincroniza ux0_data/<slug>/ con ux0:data/<slug>/ de la Vita por FTP (VitaShell, puerto 1337).

- Sube solo lo que falta o cambió de tamaño. En Documents/ (partidas) y logs/ nunca se sube ni
  se borra nada.
- --logs: baja los logs/<slug>_NNN.log nuevos o que cambiaron y los .psp2dmp de ux0:data/ a logs/.
- IP y slug salen de .psvita-toolkit.json (vita_ip, slug); --ip los pisa.

Uso: python3 -I tools/deploy_data.py [--ip 192.168.x.y] [--logs] [--dry-run]
"""
import ftplib, json, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CFG = json.load(open(os.path.join(ROOT, '.psvita-toolkit.json')))
SLUG = CFG['slug']
LOCAL = os.path.join(ROOT, 'ux0_data', SLUG)
REMOTE = '/ux0:/data/' + SLUG
NO_PUSH = {'Documents', 'logs'}


def ls(ftp, path):
    out = {}
    try:  # el FTP de VitaShell ignora el argumento de LIST: hay que entrar al directorio
        ftp.cwd(path)
        lines = []
        ftp.retrlines('LIST', lines.append)
    except ftplib.error_perm:
        return None
    for l in lines:
        p = l.split(None, 8)
        if len(p) == 9 and p[8] not in ('.', '..'):
            out[p[8]] = (l[0] == 'd', int(p[4]))
    return out


def push(ftp, local, remote, dry):
    have = ls(ftp, remote)
    if have is None:
        print('  mkdir', remote)
        if not dry:
            ftp.mkd(remote)
        have = {}
    for n in sorted(os.listdir(local)):
        if n.startswith('._') or n == '.DS_Store':
            continue
        lp, rp = os.path.join(local, n), remote + '/' + n
        if os.path.isdir(lp):
            if n not in NO_PUSH or remote != REMOTE:
                push(ftp, lp, rp, dry)
        elif n not in have or have[n][1] != os.path.getsize(lp):
            print('  put', rp, os.path.getsize(lp))
            if not dry:
                with open(lp, 'rb') as f:
                    ftp.storbinary('STOR ' + rp, f)


def pull_logs(ftp):
    dst = os.path.join(ROOT, 'logs')
    os.makedirs(dst, exist_ok=True)
    for n, (d, sz) in (ls(ftp, REMOTE + '/logs') or {}).items():
        lp = os.path.join(dst, n)
        if not d and (not os.path.exists(lp) or os.path.getsize(lp) != sz or n.endswith('.tga')):  # tga: mismo tamaño siempre
            print('  get', n)
            with open(lp, 'wb') as f:
                ftp.retrbinary('RETR %s/logs/%s' % (REMOTE, n), f.write)
    for n, (d, sz) in (ls(ftp, '/ux0:/data') or {}).items():
        if n.endswith('.psp2dmp') and not os.path.exists(os.path.join(dst, n)):
            print('  get', n)
            with open(os.path.join(dst, n), 'wb') as f:
                ftp.retrbinary('RETR /ux0:/data/' + n, f.write)


def main():
    a = sys.argv[1:]
    ip = a[a.index('--ip') + 1] if '--ip' in a else CFG.get('vita_ip')
    ftp = ftplib.FTP()
    ftp.connect(ip, CFG.get('vita_port', 1337), timeout=20)
    ftp.login()
    if '--logs' in a:
        pull_logs(ftp)
    else:
        push(ftp, LOCAL, REMOTE, '--dry-run' in a)
    ftp.quit()


if __name__ == '__main__':
    main()
