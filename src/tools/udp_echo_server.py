#!/usr/bin/env python3
"""Minimal UDP echo server for raw round-trip latency measurement.
No privileges required - plain unprivileged UDP socket."""
import socket
import sys

port = int(sys.argv[1]) if len(sys.argv) > 1 else 15678
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("0.0.0.0", port))
print(f"udp echo listening on :{port}", flush=True)
while True:
    data, addr = sock.recvfrom(65536)
    sock.sendto(data, addr)
