#!/usr/bin/env python3
"""DoD GC master as a CGI / WSGI script for ordinary shared hosting.

Same rules as dodgc_master.py (it imports them, so upload both files next to
each other), but state lives in a JSON file instead of memory, because a CGI
process dies after every request.

CGI (Apache, cPanel "Python" or plain cgi-bin):
    chmod 755 dodgc_master_cgi.py   # first line must point at python3
    master URL for the console:  http://example.com/cgi-bin/dodgc_master_cgi.py
WSGI (Passenger / cPanel "Setup Python App"): point the app at this file, the
callable is `application`.

Settings are environment variables (set them in .htaccess with SetEnv, or in the
hosting panel); everything has a safe default:
    DODGC_DATA        state directory, default "<script dir>/dodgc_data" (keep it
                      outside the web root, or keep the .htaccess this script writes)
    DODGC_PEER_HEADER header to read the client address from when a proxy or CDN
                      sits in front, e.g. HTTP_CF_CONNECTING_IP or HTTP_X_FORWARDED_FOR
                      (the last hop is used). Unset = REMOTE_ADDR.
    DODGC_ALLOW_PRIVATE=1  accept LAN/loopback servers (testing only)
"""
import fcntl
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dodgc_master as dm  # noqa: E402

STATE_FILE = "state.json"
MAX_STATE_BYTES = 1 << 20


def _data_dir():
    path = os.environ.get("DODGC_DATA") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "dodgc_data")
    os.makedirs(path, mode=0o700, exist_ok=True)
    deny = os.path.join(path, ".htaccess")
    if not os.path.exists(deny):
        try:
            with open(deny, "w") as f:
                f.write("Require all denied\nDeny from all\n")
        except OSError:
            pass
    return path


def _load(master, path):
    try:
        if os.path.getsize(path) > MAX_STATE_BYTES:
            return
        with open(path, "r") as f:
            doc = json.load(f)
        for ip, port, rec in doc.get("servers", []):
            master.servers[(str(ip), int(port))] = rec
        for ip, tokens, stamp in doc.get("buckets", []):
            master.buckets[str(ip)] = (float(tokens), float(stamp))
    except (OSError, ValueError, TypeError, KeyError):
        pass  # missing or corrupt state starts empty


def _save(master, path, now):
    doc = {"servers": [[ip, port, rec] for (ip, port), rec in master.servers.items()],
           "buckets": [[ip, t, s] for ip, (t, s) in master.buckets.items() if now - s < 120]}
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(doc, f, separators=(",", ":"))
    os.replace(tmp, path)


def handle(method, target, body, peer, data_dir, allow_private=False, now=time.time):
    """Run one request against the file-backed state. Returns (status, doc)."""
    lock = open(os.path.join(data_dir, "lock"), "a")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX)
        master = dm.Master(now=now, allow_private=allow_private)
        state = os.path.join(data_dir, STATE_FILE)
        _load(master, state)
        status, doc = master.handle(method, target, body, peer)
        _save(master, state, now())
        return status, doc
    finally:
        lock.close()  # releases the lock


def _peer(environ):
    header = os.environ.get("DODGC_PEER_HEADER", "")
    if header:
        value = environ.get(header.upper().replace("-", "_"), "")
        if value:
            return value.split(",")[-1].strip()
    return environ.get("REMOTE_ADDR", "")


def _target(environ):
    path = environ.get("PATH_INFO", "")
    if not path:  # some hosts do not set PATH_INFO: strip the script name from the URI
        uri = environ.get("REQUEST_URI", "").split("?", 1)[0]
        script = environ.get("SCRIPT_NAME", "")
        path = uri[len(script):] if script and uri.startswith(script) else uri
    query = environ.get("QUERY_STRING", "")
    return path + ("?" + query if query else "")


def application(environ, start_response):
    method = environ.get("REQUEST_METHOD", "GET")
    body = b""
    status, doc = 200, None
    try:
        if method == "POST":
            try:
                length = int(environ.get("CONTENT_LENGTH") or "")
            except ValueError:
                length = -1
            if length < 0:
                status, doc = 411, {"error": "Content-Length required"}
            elif length > dm.MAX_BODY:
                status, doc = 413, {"error": "body too large"}
            else:
                body = environ["wsgi.input"].read(length)
        if doc is None:
            status, doc = handle(method, _target(environ), body, _peer(environ), _data_dir(),
                                 os.environ.get("DODGC_ALLOW_PRIVATE") == "1")
    except Exception:
        status, doc = 500, {"error": "internal error"}
    data = json.dumps(doc, separators=(",", ":")).encode("ascii")
    start_response("%d %s" % (status, "OK" if status == 200 else "Error"),
                   [("Content-Type", "application/json"), ("Content-Length", str(len(data))),
                    ("Cache-Control", "no-store"), ("X-Content-Type-Options", "nosniff")])
    return [data]


def run_cgi():
    env = dict(os.environ)
    env["wsgi.input"] = sys.stdin.buffer
    out = {}

    def start_response(status, headers):
        out["status"], out["headers"] = status, headers

    chunks = application(env, start_response)
    sys.stdout.write("Status: %s\r\n" % out["status"])
    for k, v in out["headers"]:
        sys.stdout.write("%s: %s\r\n" % (k, v))
    sys.stdout.write("\r\n")
    sys.stdout.flush()
    for c in chunks:
        sys.stdout.buffer.write(c)


if __name__ == "__main__":
    run_cgi()
