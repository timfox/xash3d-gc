"""The shared-hosting variants (CGI/WSGI Python and PHP) must behave exactly like dodgc_master.py."""
import io
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import dodgc_master as dm  # noqa: E402
import dodgc_master_cgi as cgi  # noqa: E402

A, B = "93.184.216.34", "93.184.216.35"


def good(**o):
    d = {"port": 27015, "gamedir": "dod", "map": "dod_charlie", "players": 3, "max": 16, "name": "My server"}
    d.update(o)
    return json.dumps(d)


def scenario():
    """(method, target, body, peer, time) steps covering accept and reject paths."""
    s, t = [], 1000.0

    def add(m, tg, b="", p=A, dt=5):
        nonlocal t
        t += dt
        s.append((m, tg, b, p, t))

    add("POST", "/v1/servers", good())
    add("POST", "/v1/servers", good(port=27016, map="dod_flash"), B)
    add("GET", "/v1/servers?gamedir=dod")
    add("GET", "/v1/servers?map=dod_flash")
    add("GET", "/v1/servers?map=%3Cscript%3E")
    add("GET", "/v1/servers?gamedir=dod%0A")
    add("GET", "/v1/servers", p="10.0.0.1")
    add("GET", "/healthz")
    add("GET", "/nope")
    add("PATCH", "/v1/servers")
    add("DELETE", "/v1/servers/27016", p=B)
    add("DELETE", "/v1/servers/27015", p=B)
    for bad in (good(port=80), good(port="27015"), good(port=27015.0), good(gamedir="DOD"), good(map="a b"),
                good(players=17), good(max=0), good(name="<b>"), good(name="café"), good(map="x\n"),
                '{"port":27015,"gamedir":"dod","map":"m","players":1,"max":2,"ip":"8.8.8.8"}',
                "[]", "null", "{", "", "1", '{"port":1e999}', "[" * 400, " " * 600):
        add("POST", "/v1/servers", bad)
    for peer in ("127.0.0.1", "192.168.1.5", "169.254.1.1", "::1", "fe80::1", "garbage", "::ffff:" + B):
        add("POST", "/v1/servers", good(port=27100), peer)
    for i in range(5):
        add("POST", "/v1/servers", good(port=27200 + i), "93.184.216.40")
    add("GET", "/healthz", dt=1000)  # everything expired by now
    add("GET", "/v1/servers")
    for _ in range(25):
        add("GET", "/healthz", p="93.184.216.50", dt=0.1)
    return s


def run_python(steps):
    m = dm.Master()
    out = []
    for method, target, body, peer, now in steps:
        m.now = lambda now=now: now
        st, doc = m.handle(method, target, body.encode(), peer)
        out.append([st, doc])
    return out


@unittest.skipUnless(shutil.which("php"), "php not installed")
class PhpParity(unittest.TestCase):
    def test_php_matches_python(self):
        steps = scenario()
        driver = (
            "require $argv[1]; $steps = json_decode(file_get_contents('php://stdin'), true); $d = $argv[2]; $res = [];"
            "foreach ($steps as $s) { list($st, $doc) = dg_run($s[0], $s[1], $s[2], $s[3], $d, (float)$s[4], false);"
            "$res[] = [$st, $doc]; } echo json_encode($res);")
        with tempfile.TemporaryDirectory() as d:
            r = subprocess.run(["php", "-r", driver, str(ROOT / "scripts/dodgc_master.php"), d],
                               input=json.dumps(steps), capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr + r.stdout)
            php = json.loads(r.stdout)
        py = run_python(steps)
        self.assertEqual(len(php), len(py))
        for i, (a, b) in enumerate(zip(php, py)):
            self.assertEqual(a[0], b[0], (i, steps[i][:4], a, b))
            norm = lambda x: json.loads(json.dumps(x, sort_keys=True))
            if "servers" in b[1]:
                key = lambda e: (e["ip"], e["port"])
                self.assertEqual(sorted(a[1]["servers"], key=key), sorted(b[1]["servers"], key=key), i)
            else:
                self.assertEqual(norm(a[1].get("ok")), norm(b[1].get("ok")), i)


class CgiTests(unittest.TestCase):
    def test_cgi_matches_python_and_persists(self):
        steps = scenario()
        with tempfile.TemporaryDirectory() as d:
            out = []
            for method, target, body, peer, now in steps:
                st, doc = cgi.handle(method, target, body.encode(), peer, d, now=lambda now=now: now)
                out.append([st, doc])
        self.assertEqual(out, run_python(steps))

    def test_real_cgi_process(self):
        with tempfile.TemporaryDirectory() as d:
            env = {"PATH": os.environ["PATH"], "DODGC_DATA": d, "DODGC_ALLOW_PRIVATE": "1",
                   "REQUEST_METHOD": "POST", "REMOTE_ADDR": "127.0.0.1", "PATH_INFO": "/v1/servers",
                   "CONTENT_LENGTH": str(len(good()))}
            script = str(ROOT / "scripts/dodgc_master_cgi.py")
            r = subprocess.run([sys.executable, script], input=good().encode(), env=env, capture_output=True)
            head, _, body = r.stdout.partition(b"\r\n\r\n")
            self.assertIn(b"Status: 200", head)
            self.assertEqual(json.loads(body)["ok"], True)
            env.update(REQUEST_METHOD="GET", QUERY_STRING="gamedir=dod", CONTENT_LENGTH="")
            r = subprocess.run([sys.executable, script], env=env, capture_output=True)
            self.assertEqual(json.loads(r.stdout.partition(b"\r\n\r\n")[2])["servers"][0]["port"], 27015)
            self.assertTrue(os.path.exists(os.path.join(d, ".htaccess")))

    def test_peer_header_and_missing_path_info(self):
        env = {"REQUEST_URI": "/dodgc/master.cgi/v1/servers?x=1", "SCRIPT_NAME": "/dodgc/master.cgi",
               "QUERY_STRING": "x=1", "REMOTE_ADDR": "10.0.0.1", "HTTP_CF_CONNECTING_IP": "1.2.3.4, 5.6.7.8"}
        self.assertEqual(cgi._target(env), "/v1/servers?x=1")
        os.environ["DODGC_PEER_HEADER"] = "HTTP_CF_CONNECTING_IP"
        try:
            self.assertEqual(cgi._peer(env), "5.6.7.8")
        finally:
            del os.environ["DODGC_PEER_HEADER"]
        self.assertEqual(cgi._peer(env), "10.0.0.1")

    def test_wsgi_limits(self):
        with tempfile.TemporaryDirectory() as d:
            os.environ["DODGC_DATA"] = d
            try:
                def call(method, body=b"", length=None, path="/v1/servers"):
                    env = {"REQUEST_METHOD": method, "PATH_INFO": path, "REMOTE_ADDR": A,
                           "wsgi.input": io.BytesIO(body), "CONTENT_LENGTH": str(len(body)) if length is None else length}
                    got = {}
                    data = cgi.application(env, lambda s, h: got.update(status=s, headers=dict(h)))
                    return int(got["status"].split()[0]), json.loads(b"".join(data)), got["headers"]
                self.assertEqual(call("POST", b"x" * 2000)[0], 413)
                self.assertEqual(call("POST", b"", length="abc")[0], 411)
                st, doc, h = call("POST", good().encode())
                self.assertEqual(st, 200)
                self.assertEqual(h["X-Content-Type-Options"], "nosniff")
            finally:
                del os.environ["DODGC_DATA"]


if __name__ == "__main__":
    unittest.main()
