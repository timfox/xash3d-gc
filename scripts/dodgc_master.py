#!/usr/bin/env python3
"""Day of Defeat GameCube master server: a tiny REST service (standard library only).

  GET    /v1/servers[?gamedir=dod&map=dod_charlie]   list live servers
  POST   /v1/servers                                  register / heartbeat
  DELETE /v1/servers/<port>                           unregister (server shutdown)
  GET    /healthz                                     "ok"

POST body (JSON, at most 512 bytes, exactly these keys, every one validated):

  {"port": 27015, "gamedir": "dod", "map": "dod_charlie",
   "players": 3, "max": 16, "name": "My server"}

The server address is the TCP peer address of the request, never a value from
the payload, so nobody can list somebody else's machine. Behind a reverse
proxy start with --trust-proxy to use the last X-Forwarded-For hop instead.

GET answers {"servers": [{"ip": "203.0.113.7", "port": 27015, "gamedir": "dod",
"map": "dod_charlie", "players": 3, "max": 16, "name": "My server"}]}. The
console never trusts these descriptions: it only takes ip and port and then asks
each server itself for its info packet.

    python3 scripts/dodgc_master.py --port 8080 [--bind 0.0.0.0] [--trust-proxy]
    docker build -f scripts/Dockerfile.master -t dodgc-master . && docker run -p 8080:8080 dodgc-master
"""
import argparse
import ipaddress
import json
import re
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

DEFAULT_PORT = 8080
SERVER_TTL = 660.0        # engine heartbeats every 300 s (60 s with sv_nat)
MAX_SERVERS = 1000
MAX_PER_IP = 4
MAX_BODY = 512
MAX_REPLY = 100           # servers per list reply
RATE_BURST = 20           # requests per address ...
RATE_PER_SEC = 0.5        # ... refilled at this rate
MAX_RATE_ENTRIES = 10000
MAX_CONNECTIONS = 64
SOCKET_TIMEOUT = 5.0

RE_GAMEDIR = re.compile(r"^[a-z0-9_]{1,16}$")
RE_MAP = re.compile(r"^[A-Za-z0-9_\-]{1,32}$")
RE_NAME = re.compile(r"^[A-Za-z0-9 _.\-\[\]()+!?#:'@]{0,32}$")
POST_KEYS = {"port", "gamedir", "map", "players", "max", "name"}


class ApiError(Exception):
    def __init__(self, status, message):
        super().__init__(message)
        self.status = status
        self.message = message


def _int(value, lo, hi, field):
    if type(value) is not int or not lo <= value <= hi:  # bool is not an int here
        raise ApiError(400, "%s must be an integer %d..%d" % (field, lo, hi))
    return value


def _text(value, regex, field, required=True):
    if type(value) is not str or not regex.match(value) or (required and not value):
        raise ApiError(400, "%s is missing or has invalid characters" % field)
    return value


def validate_server(doc):
    """Return a clean server record from a decoded POST body, or raise ApiError."""
    if type(doc) is not dict:
        raise ApiError(400, "body must be a JSON object")
    unknown = set(doc) - POST_KEYS
    if unknown:
        raise ApiError(400, "unknown field")
    for key in ("port", "gamedir", "map", "players", "max"):
        if key not in doc:
            raise ApiError(400, "%s is required" % key)
    rec = {
        "port": _int(doc["port"], 1024, 65535, "port"),
        "gamedir": _text(doc["gamedir"], RE_GAMEDIR, "gamedir"),
        "map": _text(doc["map"], RE_MAP, "map"),
        "players": _int(doc["players"], 0, 32, "players"),
        "max": _int(doc["max"], 1, 32, "max"),
        "name": _text(doc.get("name", ""), RE_NAME, "name", required=False),
    }
    if rec["players"] > rec["max"]:
        raise ApiError(400, "players exceeds max")
    return rec


def public_ip(text, allow_private):
    """Parse an IPv4 peer address; reject anything a client could not reach."""
    try:
        ip = ipaddress.ip_address(text)
    except ValueError:
        raise ApiError(400, "bad peer address")
    if ip.version == 6 and ip.ipv4_mapped:
        ip = ip.ipv4_mapped
    if ip.version != 4:
        raise ApiError(400, "IPv4 servers only")
    if not allow_private and not ip.is_global:
        raise ApiError(400, "address is not publicly routable")
    return str(ip)


class Master:
    """All state and rules, with no sockets: easy to test."""

    def __init__(self, ttl=SERVER_TTL, now=time.monotonic, allow_private=False):
        self.ttl = ttl
        self.now = now
        self.allow_private = allow_private
        self.servers = {}   # (ip, port) -> record + "last"
        self.buckets = {}   # ip -> (tokens, stamp)
        self.lock = threading.Lock()

    def _expire(self):
        t = self.now()
        for key in [k for k, s in self.servers.items() if t - s["last"] > self.ttl]:
            del self.servers[key]
        if len(self.buckets) > MAX_RATE_ENTRIES:
            self.buckets.clear()

    def _rate_ok(self, ip):
        t = self.now()
        tokens, stamp = self.buckets.get(ip, (RATE_BURST, t))
        tokens = min(RATE_BURST, tokens + (t - stamp) * RATE_PER_SEC)
        if tokens < 1:
            self.buckets[ip] = (tokens, t)
            return False
        self.buckets[ip] = (tokens - 1, t)
        return True

    def handle(self, method, target, body, peer):
        """Return (status, json-able). Never raises for bad input."""
        with self.lock:
            self._expire()
            try:
                ip = public_ip(peer, self.allow_private)
            except ApiError as e:
                return e.status, {"error": e.message}
            if not self._rate_ok(ip):
                return 429, {"error": "slow down"}
            try:
                return self._route(method, target, body, ip)
            except ApiError as e:
                return e.status, {"error": e.message}
            except Exception:  # never leak internals or kill the handler thread
                return 500, {"error": "internal error"}

    def _route(self, method, target, body, ip):
        if len(target) > 256:
            raise ApiError(414, "request target too long")
        url = urlsplit(target)
        path = url.path
        if path == "/healthz" and method == "GET":
            return 200, {"ok": True}
        if path == "/v1/servers":
            if method == "GET":
                return self._list(url.query)
            if method == "POST":
                return self._register(body, ip)
            raise ApiError(405, "method not allowed")
        m = re.fullmatch(r"/v1/servers/([0-9]{1,5})", path)
        if m and method == "DELETE":
            self.servers.pop((ip, int(m.group(1))), None)
            return 200, {"ok": True}
        if m:
            raise ApiError(405, "method not allowed")
        raise ApiError(404, "not found")

    def _register(self, body, ip):
        if len(body) > MAX_BODY:
            raise ApiError(413, "body too large")
        try:
            doc = json.loads(body.decode("utf-8"))
        except (UnicodeDecodeError, ValueError, RecursionError):
            raise ApiError(400, "body is not valid JSON")
        rec = validate_server(doc)
        key = (ip, rec["port"])
        if key not in self.servers:
            if len(self.servers) >= MAX_SERVERS:
                raise ApiError(503, "server list is full")
            if sum(1 for k in self.servers if k[0] == ip) >= MAX_PER_IP:
                raise ApiError(429, "too many servers from this address")
        rec["last"] = self.now()
        self.servers[key] = rec
        return 200, {"ok": True, "ttl": int(self.ttl)}

    def _list(self, query):
        try:
            q = parse_qs(query, max_num_fields=8)
        except ValueError:
            raise ApiError(400, "too many query fields")
        gamedir = q.get("gamedir", [""])[0]
        mapname = q.get("map", [""])[0]
        if gamedir:
            _text(gamedir, RE_GAMEDIR, "gamedir")
        if mapname:
            _text(mapname, RE_MAP, "map")
        out = []
        for (ip, port), s in self.servers.items():
            if gamedir and s["gamedir"] != gamedir:
                continue
            if mapname and s["map"] != mapname:
                continue
            out.append({"ip": ip, "port": port, "gamedir": s["gamedir"], "map": s["map"],
                        "players": s["players"], "max": s["max"], "name": s["name"]})
            if len(out) >= MAX_REPLY:
                break
        return 200, {"servers": out}


def make_handler(master, trust_proxy):
    slots = threading.BoundedSemaphore(MAX_CONNECTIONS)

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"   # one request per connection: no keep-alive state
        timeout = SOCKET_TIMEOUT
        server_version = "dodgc-master"
        sys_version = ""

        def _peer(self):
            if trust_proxy:
                fwd = self.headers.get("X-Forwarded-For", "")
                if fwd:
                    return fwd.split(",")[-1].strip()
            return self.client_address[0]

        def _serve(self):
            if not slots.acquire(blocking=False):
                return self._send(503, {"error": "busy"})
            try:
                body = b""
                if self.command == "POST":
                    try:
                        length = int(self.headers.get("Content-Length", ""))
                    except ValueError:
                        return self._send(411, {"error": "Content-Length required"})
                    if length < 0 or length > MAX_BODY:
                        return self._send(413, {"error": "body too large"})
                    body = self.rfile.read(length)
                    if len(body) != length:
                        return self._send(400, {"error": "short body"})
                status, doc = master.handle(self.command, self.path, body, self._peer())
                self._send(status, doc)
            finally:
                slots.release()

        do_GET = do_POST = do_DELETE = _serve

        def do_PUT(self):
            self._send(405, {"error": "method not allowed"})

        def _send(self, status, doc):
            data = json.dumps(doc, separators=(",", ":")).encode("ascii")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(data)

        def send_error(self, code, message=None, explain=None):
            self._send(code, {"error": "bad request"})

        def log_message(self, fmt, *args):
            pass

    return Handler


def serve(bind, port, trust_proxy, allow_private):
    master = Master(allow_private=allow_private)
    httpd = ThreadingHTTPServer((bind, port), make_handler(master, trust_proxy))
    httpd.daemon_threads = True
    print("dodgc master listening on http://%s:%d" % (bind, port), flush=True)
    httpd.serve_forever()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--trust-proxy", action="store_true",
                    help="take the server address from the last X-Forwarded-For hop")
    ap.add_argument("--allow-private", action="store_true",
                    help="accept LAN/loopback servers (testing only)")
    args = ap.parse_args(argv)
    try:
        serve(args.bind, args.port, args.trust_proxy, args.allow_private)
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
