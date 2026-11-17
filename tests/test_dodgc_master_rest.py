#!/usr/bin/env python3
"""Tests for scripts/dodgc_master.py (REST master): rules and a live HTTP round trip."""
import http.client
import json
import os
import sys
import threading
import unittest
from http.server import ThreadingHTTPServer

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts"))
import dodgc_master as dm  # noqa: E402

PEER = "93.184.216.34"


def good(**over):
    d = {"port": 27015, "gamedir": "dod", "map": "dod_charlie", "players": 3, "max": 16, "name": "My server"}
    d.update(over)
    return d


class Clock:
    t = 1000.0

    def __call__(self):
        return self.t


class RulesTests(unittest.TestCase):
    def setUp(self):
        self.clock = Clock()
        self.m = dm.Master(now=self.clock)

    def post(self, doc, peer=PEER, raw=None):
        body = raw if raw is not None else json.dumps(doc).encode()
        self.clock.t += 5  # stay under the rate limit
        return self.m.handle("POST", "/v1/servers", body, peer)

    def test_register_and_list_uses_peer_address(self):
        self.assertEqual(self.post(good())[0], 200)
        st, doc = self.m.handle("GET", "/v1/servers?gamedir=dod", b"", "93.184.216.99")
        self.assertEqual(st, 200)
        self.assertEqual(doc["servers"], [{"ip": PEER, "port": 27015, "gamedir": "dod",
                                           "map": "dod_charlie", "players": 3, "max": 16,
                                           "name": "My server"}])

    def test_payload_cannot_choose_address(self):
        self.assertEqual(self.post(good(ip="8.8.8.8"))[0], 400)

    def test_rejects_bad_fields(self):
        bad = [good(port=80), good(port=70000), good(port="27015"), good(port=True),
               good(port=27015.0), good(gamedir="DOD"), good(gamedir="../x"), good(map="a b"),
               good(map=""), good(map="x" * 33), good(players=17), good(players=-1),
               good(max=0), good(max=33), good(name="<script>"), good(name="a" * 33),
               good(name="café"), good(name='q"q'), good(name="a\\b"), good(name=5),
               good(map="dod_x\n"), good(gamedir="dod\n"), good(name="x\n")]
        for doc in bad:
            self.assertEqual(self.post(doc)[0], 400, doc)
        missing = good()
        del missing["map"]
        self.assertEqual(self.post(missing)[0], 400)

    def test_rejects_non_json_and_wrong_shapes(self):
        for raw in (b"", b"{", b"[]", b"1", b"null", b"\xff\xfe", b"[" * 400, b'{"port":1e999}'):
            self.assertEqual(self.post(None, raw=raw)[0], 400, raw[:20])
        self.assertEqual(self.post(None, raw=b" " * 600)[0], 413)

    def test_non_public_peers_rejected(self):
        for peer in ("127.0.0.1", "10.0.0.5", "192.168.1.2", "169.254.1.1", "::1", "fe80::1", "garbage"):
            self.assertEqual(self.post(good(), peer=peer)[0], 400, peer)
        self.assertEqual(self.post(good(), peer="::ffff:93.184.216.34")[0], 200)

    def test_allow_private_for_testing(self):
        m = dm.Master(now=self.clock, allow_private=True)
        st, _ = m.handle("POST", "/v1/servers", json.dumps(good()).encode(), "127.0.0.1")
        self.assertEqual(st, 200)

    def test_expiry_and_heartbeat_refresh(self):
        self.post(good())
        self.clock.t += dm.SERVER_TTL - 1
        self.post(good(players=4))
        self.clock.t += dm.SERVER_TTL - 1
        self.assertEqual(len(self.m.handle("GET", "/v1/servers", b"", "93.184.216.99")[1]["servers"]), 1)
        self.clock.t += 2
        self.assertEqual(self.m.handle("GET", "/v1/servers", b"", "93.184.216.99")[1]["servers"], [])

    def test_caps(self):
        for p in range(dm.MAX_PER_IP):
            self.clock.t += 5
            self.assertEqual(self.post(good(port=27015 + p))[0], 200)
        self.clock.t += 5
        self.assertEqual(self.post(good(port=30000))[0], 429)
        self.clock.t += 5
        self.assertEqual(self.post(good(port=27015))[0], 200)  # refresh of a known entry still works
        m = dm.Master(now=self.clock)
        for i in range(dm.MAX_SERVERS):
            m.servers[("1.1.%d.%d" % (i // 250, i % 250 + 1), 27015)] = dict(good(), last=self.clock.t)
        self.assertEqual(m.handle("POST", "/v1/servers", json.dumps(good()).encode(), PEER)[0], 503)

    def test_rate_limit(self):
        codes = [self.m.handle("GET", "/healthz", b"", PEER)[0] for _ in range(dm.RATE_BURST + 5)]
        self.assertEqual(codes[:dm.RATE_BURST], [200] * dm.RATE_BURST)
        self.assertEqual(codes[-1], 429)
        self.clock.t += 10
        self.assertEqual(self.m.handle("GET", "/healthz", b"", PEER)[0], 200)

    def test_delete_only_own_entry(self):
        self.post(good())
        self.m.handle("DELETE", "/v1/servers/27015", b"", "93.184.216.99")
        self.assertEqual(len(self.m.servers), 1)
        self.m.handle("DELETE", "/v1/servers/27015", b"", PEER)
        self.assertEqual(len(self.m.servers), 0)

    def test_filters_validated_and_not_reflected(self):
        self.post(good())
        st, doc = self.m.handle("GET", "/v1/servers?map=%3Cscript%3E", b"", PEER)
        self.assertEqual(st, 400)
        self.assertNotIn("script", json.dumps(doc))
        st, doc = self.m.handle("GET", "/v1/servers?map=other", b"", PEER)
        self.assertEqual(doc["servers"], [])

    def test_routes(self):
        self.assertEqual(self.m.handle("GET", "/nope", b"", PEER)[0], 404)
        self.assertEqual(self.m.handle("PATCH", "/v1/servers", b"", PEER)[0], 405)
        self.assertEqual(self.m.handle("GET", "/v1/servers/" + "9" * 300, b"", PEER)[0], 414)


class LiveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.master = dm.Master(allow_private=True)
        cls.httpd = ThreadingHTTPServer(("127.0.0.1", 0), dm.make_handler(cls.master, False))
        cls.httpd.daemon_threads = True
        threading.Thread(target=cls.httpd.serve_forever, daemon=True).start()
        cls.port = cls.httpd.server_address[1]

    @classmethod
    def tearDownClass(cls):
        cls.httpd.shutdown()

    def req(self, method, path, body=None, headers=None):
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        c.request(method, path, body=body, headers=headers or {})
        r = c.getresponse()
        data = r.read()
        c.close()
        return r.status, r.getheader("Content-Type"), data

    def test_round_trip_and_headers(self):
        st, ctype, data = self.req("POST", "/v1/servers", json.dumps(good()),
                                   {"Content-Type": "application/json"})
        self.assertEqual((st, ctype), (200, "application/json"))
        st, _, data = self.req("GET", "/v1/servers?gamedir=dod")
        servers = json.loads(data)["servers"]
        self.assertEqual(servers[0]["ip"], "127.0.0.1")
        self.assertEqual(servers[0]["port"], 27015)

    def test_oversize_and_malformed(self):
        try:
            st = self.req("POST", "/v1/servers", "x" * 2000, {"Content-Length": "2000"})[0]
            self.assertEqual(st, 413)
        except (ConnectionError, http.client.RemoteDisconnected):
            pass  # server refused before reading the body: fine
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        c.putrequest("POST", "/v1/servers")
        c.putheader("Content-Length", "abc")
        c.endheaders()
        self.assertEqual(c.getresponse().status, 411)
        self.assertEqual(self.req("PUT", "/v1/servers")[0], 405)
        self.assertEqual(self.req("GET", "/v1/servers?" + "a=1&" * 50)[0] in (200, 400), True)


if __name__ == "__main__":
    unittest.main()
