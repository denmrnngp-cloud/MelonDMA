#!/usr/bin/env python3
"""Minimal RoCEv2 RC client for macOS - plain userspace UDP sockets, no
entitlement, no kernel extension, no special hardware.

Two independent test paths, because they need two different peers:

  --mode send   Wire-compatible with the STOCK `ibv_rc_pingpong -d rxe0 -g 0`
                 tool already installed on Spark (SEND/RECV semantics, no
                 rkey/va on the wire - confirmed by reading rc_pingpong.c:
                 it calls IBV_WR_SEND, never IBV_WR_RDMA_WRITE). Use this
                 first: it validates BTH packing, ICRC, and the TCP
                 handshake against a real, unmodified RDMA stack with zero
                 custom server code.

  --mode write  Targets code/spark_write_server.c (a small custom server
                 that DOES publish rkey/va, since the stock pingpong tool
                 never does). This is the actual RDMA_WRITE path relevant
                 to a future ggml-rpc integration.

Protocol details were extracted from the Linux `rxe` (SoftRoCE) driver
source and from rdma-core's rc_pingpong.c and ib_pack.h - see
research/rxe-reference/ in this project. Opcode values cross-checked
against the authoritative kernel header (ib_pack.h), not reconstructed
from memory.

STATUS: unverified prototype - not yet run against a live rxe0 (this
session had no root on the Spark box to bring rxe0 up). Before trusting
this, follow the pcap-comparison procedure in
notes/07-path-c-protocol-implementation.md.
"""
from __future__ import annotations

import argparse
import os
import socket
import struct
import zlib
from dataclasses import dataclass

ROCE_V2_UDP_PORT = 4791
ICRC_SEED = 0xDEBB20E3

# RC opcodes, base 0x00 for the RC transport class (research/rxe-reference/ib_pack.h).
RC_SEND_ONLY = 0x04
RC_RDMA_WRITE_ONLY = 0x0A
RC_RDMA_WRITE_ONLY_WITH_IMMEDIATE = 0x0B
RC_ACKNOWLEDGE = 0x11

BTH_LEN = 12
RETH_LEN = 16
ICRC_LEN = 4

# rc_pingpong.c's handshake buffer is a FIXED size:
#   sizeof "0000:000000:000000:00000000000000000000000000000000"
# = 4 + 1 + 6 + 1 + 6 + 1 + 32 + 1(nul) = 52 bytes, sent/received verbatim
# via write()/read() of exactly that many bytes - not newline-terminated.
PINGPONG_MSG_SIZE = len("0000:000000:000000:00000000000000000000000000000000") + 1


# --------------------------------------------------------------------------
# Header packing/unpacking
# --------------------------------------------------------------------------

def pack_bth(opcode: int, qpn: int, psn: int, *, ack_req: bool = True,
             pkey: int = 0xFFFF) -> bytes:
    apsn = (psn & 0x00FFFFFF) | (0x80000000 if ack_req else 0)
    return struct.pack("!BBHII", opcode, 0x00, pkey, qpn & 0x00FFFFFF, apsn)


def unpack_bth(data: bytes) -> tuple[int, int, int]:
    opcode, _flags, _pkey, qpn, apsn = struct.unpack_from("!BBHII", data, 0)
    return opcode, qpn & 0x00FFFFFF, apsn & 0x00FFFFFF


def pack_reth(va: int, rkey: int, length: int) -> bytes:
    return struct.pack("!QII", va, rkey, length)


def ipv4_mapped_gid(ipv4_addr: str) -> bytes:
    return b"\x00" * 10 + b"\xff\xff" + socket.inet_aton(ipv4_addr)


def gid_to_hex(gid: bytes) -> str:
    return gid.hex()


def hex_to_gid(s: str) -> bytes:
    return bytes.fromhex(s)


# --------------------------------------------------------------------------
# ICRC - ported from research/rxe-reference/rxe_icrc.c
# --------------------------------------------------------------------------

def compute_icrc(ip_src: str, ip_dst: str, udp_src_port: int, header_after_bth_incl: bytes,
                  payload: bytes) -> int:
    """header_after_bth_incl = BTH + (RETH if present). NOTE: this rebuilds a
    plausible IPv4 header rather than capturing the real on-wire bytes -
    see the WARNING in notes/07. Byte-compare against a real pcap before
    trusting this against a real rxe0 (fields like IP identification/flags
    are not covered by rxe's ICRC computation but must still be consistent
    with what actually goes on the wire for the packet to parse correctly
    upstream of the ICRC check)."""
    total_len = 20 + 8 + len(header_after_bth_incl) + len(payload) + ICRC_LEN
    ip_hdr = struct.pack("!BBHHHBBH4s4s", 0x45, 0xFF, total_len, 0, 0, 0xFF,
                         socket.IPPROTO_UDP, 0, socket.inet_aton(ip_src),
                         socket.inet_aton(ip_dst))
    udp_hdr = struct.pack("!HHHH", udp_src_port, ROCE_V2_UDP_PORT,
                          8 + len(header_after_bth_incl) + len(payload) + ICRC_LEN, 0)

    bth = bytearray(header_after_bth_incl[:BTH_LEN])
    bth[4] |= 0xFF  # mask bth.qpn's reserved top byte (exclude resv8a from CRC)
    pseudo = bytes(ip_hdr) + bytes(udp_hdr) + bytes(bth)

    crc = zlib.crc32(pseudo, ICRC_SEED)
    rest = header_after_bth_incl[BTH_LEN:]
    if rest:
        crc = zlib.crc32(rest, crc)
    crc = zlib.crc32(payload, crc)
    return (~crc) & 0xFFFFFFFF


# --------------------------------------------------------------------------
# Handshake, path 1: wire-compatible with stock ibv_rc_pingpong (SEND/RECV)
# --------------------------------------------------------------------------

@dataclass
class PingpongDest:
    lid: int
    qpn: int
    psn: int
    gid: bytes


def pingpong_handshake(server_host: str, server_port: int, my_dest: PingpongDest) -> PingpongDest:
    msg = f"{my_dest.lid:04x}:{my_dest.qpn:06x}:{my_dest.psn:06x}:{gid_to_hex(my_dest.gid)}"
    msg_bytes = msg.encode("ascii").ljust(PINGPONG_MSG_SIZE, b"\x00")
    assert len(msg_bytes) == PINGPONG_MSG_SIZE, f"{len(msg_bytes)} != {PINGPONG_MSG_SIZE}"

    with socket.create_connection((server_host, server_port), timeout=5) as s:
        s.sendall(msg_bytes)
        reply = b""
        while len(reply) < PINGPONG_MSG_SIZE:
            chunk = s.recv(PINGPONG_MSG_SIZE - len(reply))
            if not chunk:
                break
            reply += chunk
        # rc_pingpong's client then writes "done" (5 bytes incl. NUL) before
        # closing, and the server waits for it before proceeding to RTR/RTS.
        s.sendall(b"done\x00")

    text = reply.rstrip(b"\x00").decode("ascii")
    lid_s, qpn_s, psn_s, gid_s = text.split(":")
    return PingpongDest(int(lid_s, 16), int(qpn_s, 16), int(psn_s, 16), hex_to_gid(gid_s))


def run_send_test(args: argparse.Namespace) -> None:
    qpn_local = os.getpid() & 0x00FFFFFF
    psn_local = int.from_bytes(os.urandom(3), "big")
    my_dest = PingpongDest(0, qpn_local, psn_local, ipv4_mapped_gid(args.my_ip))
    print(f"local:  qpn={qpn_local:06x} psn={psn_local:06x} gid={gid_to_hex(my_dest.gid)}")

    dest = pingpong_handshake(args.spark_ip, args.handshake_port, my_dest)
    print(f"remote: qpn={dest.qpn:06x} psn={dest.psn:06x} gid={gid_to_hex(dest.gid)}")

    payload = (b"hello from macOS software RoCEv2" + b"\x00" * 64)[: args.payload_size]
    bth = pack_bth(RC_SEND_ONLY, dest.qpn, psn_local)
    _send_and_wait_ack(args.spark_ip, args.my_ip, bth, payload)


# --------------------------------------------------------------------------
# Handshake, path 2: custom extended format with rkey/va (spark_write_server.c)
# --------------------------------------------------------------------------

def write_handshake(server_host: str, server_port: int, my_dest: PingpongDest) -> tuple[PingpongDest, int, int]:
    msg = f"{my_dest.lid:04x}:{my_dest.qpn:06x}:{my_dest.psn:06x}:{gid_to_hex(my_dest.gid)}\n"
    with socket.create_connection((server_host, server_port), timeout=5) as s:
        s.sendall(msg.encode("ascii"))
        s.shutdown(socket.SHUT_WR)
        reply = b""
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            reply += chunk
    text = reply.decode("ascii").strip()
    lid_s, qpn_s, psn_s, gid_s, rkey_s, va_s = text.split(":")
    dest = PingpongDest(int(lid_s, 16), int(qpn_s, 16), int(psn_s, 16), hex_to_gid(gid_s))
    return dest, int(rkey_s, 16), int(va_s, 16)


def run_write_test(args: argparse.Namespace) -> None:
    qpn_local = os.getpid() & 0x00FFFFFF
    psn_local = int.from_bytes(os.urandom(3), "big")
    my_dest = PingpongDest(0, qpn_local, psn_local, ipv4_mapped_gid(args.my_ip))
    print(f"local:  qpn={qpn_local:06x} psn={psn_local:06x} gid={gid_to_hex(my_dest.gid)}")

    dest, rkey, va = write_handshake(args.spark_ip, args.handshake_port, my_dest)
    print(f"remote: qpn={dest.qpn:06x} psn={dest.psn:06x} rkey=0x{rkey:08x} va=0x{va:016x}")

    payload = (b"RDMA WRITE from macOS software RoCEv2 client!" + b"\x00" * 64)[: args.payload_size]
    bth = pack_bth(RC_RDMA_WRITE_ONLY, dest.qpn, psn_local)
    reth = pack_reth(va, rkey, len(payload))
    _send_and_wait_ack(args.spark_ip, args.my_ip, bth + reth, payload)


# --------------------------------------------------------------------------
# Shared send path
# --------------------------------------------------------------------------

def _send_and_wait_ack(spark_ip: str, my_ip: str, header: bytes, payload: bytes) -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((my_ip, 0))
    udp_src_port = sock.getsockname()[1]

    icrc = compute_icrc(my_ip, spark_ip, udp_src_port, header, payload)
    packet = header + payload + struct.pack("!I", icrc)
    print(f"sending {len(packet)}-byte RoCEv2 packet to {spark_ip}:{ROCE_V2_UDP_PORT} "
          f"(icrc=0x{icrc:08x})")
    sock.sendto(packet, (spark_ip, ROCE_V2_UDP_PORT))

    sock.settimeout(2.0)
    try:
        ack, _ = sock.recvfrom(65536)
        opcode, qpn, psn = unpack_bth(ack)
        print(f"ACK received: opcode=0x{opcode:02x} qpn={qpn:06x} psn={psn:06x}")
    except socket.timeout:
        print("no ACK within 2s - either dropped, or ICRC mismatch caused a "
              "silent drop on the rxe side. Compare against a real pcap "
              "(notes/07) to debug field-by-field.")
    finally:
        sock.close()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mode", choices=["send", "write"], required=True)
    ap.add_argument("--spark-ip", default="192.168.100.2")
    ap.add_argument("--my-ip", default="192.168.100.1")
    ap.add_argument("--handshake-port", type=int, default=18515)
    ap.add_argument("--payload-size", type=int, default=64)
    args = ap.parse_args()

    if args.mode == "send":
        run_send_test(args)
    else:
        run_write_test(args)


if __name__ == "__main__":
    main()
