#!/usr/bin/env python3
"""Tests for scripts/dodgc_master.py: protocol bytes mirror the engine C code."""
import os
import socket
import struct
import sys
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dodgc_master as dm  # noqa: E402


def heartbeat(hb_challenge):
    # NET_AnnounceToMaster: S2M_HEARTBEAT (2 bytes) + dword
    return b"q\xff" + struct.pack("<I", hb_challenge)


def info_reply(master_challenge, **over):
    # SV_AddToMaster: "0\n" + infostring, no -1 header
    f = {"protocol": "49", "challenge": str(master_challenge), "players": "1", "max": "2",
         "bots": "0", "gamedir": "dod", "map": "dod_flash", "gc_dod": "1", "nat": "0"}
    f.update(over)
    return b"0\n" + "".join("\\%s\\%s" % kv for kv in f.items()).encode()


def scan(**flt):
    # NET_BuildMasterServerScanRequest: "1\xff" "0.0.0.0:0\0" + infostring
    flt.setdefault("gamedir", "dod")
    flt.setdefault("gc_dod", "1")
    flt.setdefault("key", "1")
    return b"1\xff0.0.0.0:0\x00" + "".join("\\%s\\%s" % kv for kv in flt.items()).encode()


def parse_list(reply):
    # CL_ConnectionlessPacket: -1, line "f", then CL_ServerList
    assert reply[:4] == dm.HEADER and reply[4:6] == b"f\n"
    body = reply[6:]
    assert body[0] == 0x7F           # dead key extension the client consumes
    body = body[1 + 4 + 1:]
    out = []
    while len(body) >= 6:
        ip, port = socket.inet_ntoa(body[:4]), struct.unpack(">H", body[4:6])[0]
        body = body[6:]
        if port == 0:
            break
        out.append((ip, port))
    return out


class FakeClock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t


class MasterTests(unittest.TestCase):
    def setUp(self):
        self.clock = FakeClock()
        self.m = dm.Master(now=self.clock, rand=lambda: 0xCAFE)
        self.srv = ("203.0.113.5", 27015)
        self.cli = ("198.51.100.9", 40000)

    def register(self, addr=None, **over):
        addr = addr or self.srv
        (reply, dest), = self.m.handle(heartbeat(0x1234), addr)
        self.assertEqual(dest, addr)
        # engine reads "s\n" line then challenge, challenge2 dwords
        self.assertEqual(reply, dm.HEADER + b"s\n" + struct.pack("<II", 0xCAFE, 0x1234))
        self.assertEqual(self.m.handle(info_reply(0xCAFE, **over), addr), [])

    def query(self, **flt):
        (reply, dest), = self.m.handle(scan(**flt), self.cli)
        self.assertEqual(dest, self.cli)
        return parse_list(reply)

    def test_register_and_list(self):
        self.register()
        self.assertEqual(self.query(), [self.srv])

    def test_wrong_challenge_rejected(self):
        self.m.handle(heartbeat(1), self.srv)
        self.m.handle(info_reply(0xBAD), self.srv)
        self.assertEqual(self.query(), [])

    def test_info_without_heartbeat_rejected(self):
        self.m.handle(info_reply(0xCAFE), self.srv)
        self.assertEqual(self.query(), [])

    def test_non_dod_gc_server_not_registered(self):
        self.register(gc_dod="0")
        self.register(addr=("203.0.113.6", 27015), gc_dod="")
        self.assertEqual(self.query(), [])

    def test_other_gamedir_filtered(self):
        self.register(gamedir="valve")
        self.assertEqual(self.query(), [])
        self.clock.t += 1
        self.assertEqual(self.query(gamedir="valve"), [self.srv])

    def test_expiry_and_shutdown(self):
        self.register()
        self.clock.t += dm.SERVER_TTL + 1
        self.assertEqual(self.query(), [])
        self.register()
        self.m.handle(b"b\n", self.srv)
        self.clock.t += 1
        self.assertEqual(self.query(), [])

    def test_mainui_filters(self):
        self.register(players="0")
        self.assertEqual(self.query(empty="0"), [])
        self.clock.t += 1
        self.assertEqual(self.query(empty="1"), [self.srv])
        self.clock.t += 1
        self.assertEqual(self.query(map="other"), [])

    def test_per_ip_cap(self):
        for p in range(dm.MAX_PER_IP + 2):
            self.register(addr=("203.0.113.5", 27015 + p)) if p < dm.MAX_PER_IP else \
                self.assertEqual(self.m.handle(heartbeat(1), ("203.0.113.5", 27015 + p)), [])
        self.assertEqual(len(self.query()), dm.MAX_PER_IP)

    def test_query_rate_limit(self):
        self.register()
        self.assertEqual(len(self.m.handle(scan(), self.cli)), 1)
        self.assertEqual(self.m.handle(scan(), self.cli), [])

    def test_garbage_ignored(self):
        for junk in (b"", b"x", b"q\xff", b"0\n\\\\", b"1\xff"):
            self.m.handle(junk, self.srv)


class LoopbackTest(unittest.TestCase):
    """Real UDP sockets: heartbeat, challenge, info, query."""

    def test_end_to_end(self):
        master = dm.Master()
        ms = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        ms.bind(("127.0.0.1", 0))
        ms.settimeout(3)
        stop = threading.Event()

        def run():
            while not stop.is_set():
                try:
                    data, addr = ms.recvfrom(2048)
                except OSError:
                    return
                for payload, dest in master.handle(data, addr):
                    ms.sendto(payload, dest)

        t = threading.Thread(target=run, daemon=True)
        t.start()
        server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            server.bind(("127.0.0.1", 0))
            server.settimeout(3)
            server.sendto(heartbeat(77), ms.getsockname())
            data, _ = server.recvfrom(2048)
            self.assertEqual(data[:6], dm.HEADER + b"s\n")
            ch, ch2 = struct.unpack("<II", data[6:14])
            self.assertEqual(ch2, 77)
            server.sendto(info_reply(ch), ms.getsockname())

            client.settimeout(3)
            for _ in range(20):          # registration is asynchronous
                client.sendto(scan(), ms.getsockname())
                listed = parse_list(client.recvfrom(2048)[0])
                if listed:
                    break
                time.sleep(0.6)
            self.assertEqual(listed, [("127.0.0.1", server.getsockname()[1])])
        finally:
            stop.set()
            for s in (ms, server, client):
                s.close()


if __name__ == "__main__":
    unittest.main()
