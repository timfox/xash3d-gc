#!/usr/bin/env python3
"""Minimal Day of Defeat GameCube master server (Xash3D UDP master protocol).

Hosts announce themselves with a heartbeat; clients ask for the list. Only
servers that carry the DoD GC marker (gc_dod=<version>) are registered or
listed, so Find servers never shows other Xash3D games.

Wire format (see engine/common/masterlist.c, engine/server/sv_main.c
SV_AddToMaster and engine/client/cl_main.c CL_ServerList):

  server -> master  "q\\xff" + u32le heartbeat_challenge
  master -> server  FFFFFFFF "s\\n" + u32le master_challenge + u32le heartbeat_challenge
  server -> master  "0\\n" + infostring (protocol, challenge, players, max,
                    gamedir, map, gc_dod, ...; challenge = master_challenge)
  server -> master  "b\\n"                                 (shutdown)
  client -> master  "1\\xff" "0.0.0.0:0\\0" + infostring filter (gamedir, gc_dod,
                    map, ...)
  master -> client  FFFFFFFF "f\\n" 7F u32 key 00 then 6-byte (ip4, port be)
                    entries, ended by 00 00 00 00 00 00

Run:  scripts/dodgc_master.py [--port 27010] [--bind 0.0.0.0]
Hosts:   set  dodgc_master "<this host>[:27010]"   (cvar, archived)
Clients: same cvar; Find servers > Internet queries this master.
A host behind a home router must forward its game port (UDP 27015).
"""
import argparse
import os
import socket
import struct
import sys
import time

HEADER = b"\xff\xff\xff\xff"
DEFAULT_PORT = 27010
DODGC_KEY = "gc_dod"
DODGC_VERSION = "1"
PROTOCOL_VERSION = "49"

SERVER_TTL = 660.0       # engine heartbeats every 300 s (60 s with sv_nat)
CHALLENGE_TTL = 30.0     # sv_master_response_timeout is 4 s; allow slack
MAX_SERVERS = 2000
MAX_PER_IP = 4
MAX_REPLY_ENTRIES = 190  # 6 bytes each: keeps the reply under one MTU-ish packet
QUERY_MIN_INTERVAL = 0.5  # per source address


def parse_info(text):
    """Parse a \\key\\value\\key\\value infostring into a dict."""
    out = {}
    parts = text.split("\\")
    if parts and parts[0] == "":
        parts = parts[1:]
    for i in range(0, len(parts) - 1, 2):
        out[parts[i]] = parts[i + 1]
    return out


class Master:
    def __init__(self, ttl=SERVER_TTL, now=time.monotonic, rand=None):
        self.ttl = ttl
        self.now = now
        self.rand = rand or (lambda: struct.unpack("<I", os.urandom(4))[0])
        self.pending = {}   # addr -> (master_challenge, heartbeat_challenge, expiry)
        self.servers = {}   # addr -> dict(info, last)
        self.last_query = {}

    # -- helpers
    def expire(self):
        t = self.now()
        for addr in [a for a, s in self.servers.items() if t - s["last"] > self.ttl]:
            del self.servers[addr]
        for addr in [a for a, p in self.pending.items() if t > p[2]]:
            del self.pending[addr]
        for addr in [a for a, q in self.last_query.items() if t - q > 60]:
            del self.last_query[addr]

    def handle(self, data, addr):
        """Process one datagram; return a list of (payload, addr) replies."""
        self.expire()
        if data.startswith(b"q\xff") and len(data) >= 6:
            return self._heartbeat(data, addr)
        if data.startswith(b"0\n"):
            self._server_info(data, addr)
            return []
        if data.startswith(b"b\n"):
            self.servers.pop(addr, None)
            return []
        if data.startswith(b"1\xff"):
            return self._query(data, addr)
        return []

    # -- server side
    def _heartbeat(self, data, addr):
        (heartbeat_challenge,) = struct.unpack_from("<I", data, 2)
        known = addr in self.servers
        if not known:
            if len(self.servers) >= MAX_SERVERS:
                return []
            if sum(1 for a in self.servers if a[0] == addr[0]) >= MAX_PER_IP:
                return []
        master_challenge = self.rand()
        self.pending[addr] = (master_challenge, heartbeat_challenge, self.now() + CHALLENGE_TTL)
        return [(HEADER + b"s\n" + struct.pack("<II", master_challenge, heartbeat_challenge), addr)]

    def _server_info(self, data, addr):
        pend = self.pending.get(addr)
        if not pend:
            return
        try:
            info = parse_info(data[2:].split(b"\0", 1)[0].decode("latin-1"))
        except Exception:
            return
        if info.get("challenge") != str(pend[0]):
            return  # not the challenge we issued: spoofed or stale
        if info.get(DODGC_KEY) != DODGC_VERSION:
            return  # not a DoD GC server
        if info.get("protocol") != PROTOCOL_VERSION:
            return
        del self.pending[addr]
        self.servers[addr] = {"info": info, "last": self.now()}

    # -- client side
    @staticmethod
    def _matches(server_info, flt):
        if flt.get("gamedir") and flt["gamedir"].lower() != server_info.get("gamedir", "").lower():
            return False
        if flt.get(DODGC_KEY, DODGC_VERSION) != server_info.get(DODGC_KEY):
            return False
        if flt.get("map") and flt["map"] != server_info.get("map"):
            return False
        try:
            players = int(server_info.get("players", "0"))
            maxp = int(server_info.get("max", "0"))
        except ValueError:
            return False
        if flt.get("empty") == "0" and players == 0:   # mainui: hide empty
            return False
        if flt.get("full") == "0" and players >= maxp:  # mainui: hide full
            return False
        return True

    def _query(self, data, addr):
        t = self.now()
        if t - self.last_query.get(addr, -1e9) < QUERY_MIN_INTERVAL:
            return []
        self.last_query[addr] = t
        body = data[2:]
        nul = body.find(b"\0")
        flt = parse_info(body[nul + 1:].split(b"\0", 1)[0].decode("latin-1")) if nul >= 0 else {}
        reply = HEADER + b"f\n" + b"\x7f" + struct.pack("<I", int(flt.get("key", "0") or "0", 16) if _is_hex(flt.get("key", "0")) else 0) + b"\x00"
        n = 0
        for (ip, port), srv in self.servers.items():
            if n >= MAX_REPLY_ENTRIES:
                break
            if not self._matches(srv["info"], flt):
                continue
            try:
                reply += socket.inet_aton(ip) + struct.pack(">H", port)
            except OSError:
                continue
            n += 1
        reply += b"\x00" * 6
        return [(reply, addr)]


def _is_hex(s):
    try:
        int(s, 16)
        return True
    except (TypeError, ValueError):
        return False


def serve(bind, port):
    master = Master()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((bind, port))
    print("dodgc master listening on %s:%d" % (bind, port), flush=True)
    while True:
        data, addr = sock.recvfrom(2048)
        if data.startswith(HEADER):
            data = data[4:]
        for payload, dest in master.handle(data, addr):
            sock.sendto(payload, dest)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    args = ap.parse_args(argv)
    try:
        serve(args.bind, args.port)
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
