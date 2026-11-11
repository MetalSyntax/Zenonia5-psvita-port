#!/usr/bin/env python3
"""Proxy TCP que registra (hex) el tráfico de Zenonia 5 con npk.gamevil.com:26380 (registro de ID).
El iPhone resuelve npk.gamevil.com a la Mac (/etc/hosts); esto reenvía al servidor real.
Uso: python3 npk_proxy.py   (Ctrl+C para salir)
"""
import socket
import threading
import time

LISTEN = 26380
UPSTREAM = ("218.145.70.122", 26380)  # npk.gamevil.com (dig, 2026-11-11)


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


def pipe(src, dst, tag):
    try:
        while True:
            data = src.recv(65536)
            if not data:
                break
            log(f"{tag} {len(data)} B\n  hex: {data.hex()}\n  txt: {data.decode('latin-1')!r}")
            dst.sendall(data)
    except OSError as e:
        log(f"{tag} cerrado: {e}")
    finally:
        for s in (src, dst):
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


def main():
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", LISTEN))
    srv.listen(4)
    log(f"proxy npk en :{LISTEN} -> {UPSTREAM[0]}:{UPSTREAM[1]}")
    while True:
        c, addr = srv.accept()
        log(f"conexión de {addr[0]}")
        u = socket.create_connection(UPSTREAM, timeout=15)
        u.settimeout(None)
        threading.Thread(target=pipe, args=(c, u, "JUEGO>GV"), daemon=True).start()
        threading.Thread(target=pipe, args=(u, c, "GV>JUEGO"), daemon=True).start()


if __name__ == "__main__":
    main()
