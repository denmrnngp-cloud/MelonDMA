#!/usr/bin/env python3
"""Compares raw UDP round-trip vs raw TCP round-trip vs ICMP ping to the
same host, on the same link, back to back - isolates protocol overhead
from compute/model overhead. No privileges required."""
import socket
import struct
import sys
import time
import statistics

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.100.2"
UDP_PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 15678
TCP_PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 15679
N = 200
PAYLOAD = b"x" * 64  # small control-message-sized payload, like a GRAPH_COMPUTE ack


def udp_rtt():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(2)
    times = []
    for i in range(N):
        t0 = time.perf_counter()
        s.sendto(PAYLOAD, (HOST, UDP_PORT))
        s.recvfrom(65536)
        times.append((time.perf_counter() - t0) * 1000)
    s.close()
    return times


def tcp_rtt():
    times = []
    for i in range(N):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(2)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        t0 = time.perf_counter()
        s.connect((HOST, TCP_PORT))
        s.sendall(PAYLOAD)
        s.recv(65536)
        times.append((time.perf_counter() - t0) * 1000)
        s.close()
    return times


def tcp_rtt_persistent():
    """Persistent connection (no handshake per request) - fairer comparison
    to what a real long-lived RPC connection looks like."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(2)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.connect((HOST, TCP_PORT))
    times = []
    for i in range(N):
        t0 = time.perf_counter()
        s.sendall(PAYLOAD)
        s.recv(65536)
        times.append((time.perf_counter() - t0) * 1000)
    s.close()
    return times


def report(name, times):
    times.sort()
    print(f"{name:32s} min={times[0]:.3f}ms  p50={times[len(times)//2]:.3f}ms  "
          f"p99={times[int(len(times)*0.99)]:.3f}ms  mean={statistics.mean(times):.3f}ms")


if __name__ == "__main__":
    print(f"target: {HOST}  udp_port={UDP_PORT}  tcp_port={TCP_PORT}  n={N}\n")
    report("UDP echo (raw)", udp_rtt())
    report("TCP new-connection-per-req", tcp_rtt())
    report("TCP persistent (fair vs RPC)", tcp_rtt_persistent())
