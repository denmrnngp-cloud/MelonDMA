#!/usr/bin/env python3
"""Minimal TCP echo server (persistent connections, TCP_NODELAY) for
round-trip latency comparison against UDP. No privileges required."""
import socket
import sys
import threading

port = int(sys.argv[1]) if len(sys.argv) > 1 else 15679


def handle(conn):
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    with conn:
        while True:
            data = conn.recv(65536)
            if not data:
                return
            conn.sendall(data)


srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("0.0.0.0", port))
srv.listen(16)
print(f"tcp echo listening on :{port}", flush=True)
while True:
    conn, addr = srv.accept()
    threading.Thread(target=handle, args=(conn,), daemon=True).start()
