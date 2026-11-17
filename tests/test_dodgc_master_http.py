import pathlib, shutil, subprocess, tempfile, unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
INC = ROOT / "engine/common"

C = r'''
#include <stdio.h>
#include <string.h>
#include "dodgc_master_http.h"
#define CHECK(x) do{ if(!(x)){ printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } }while(0)

struct got { int n; uint8_t ip[8][4]; uint16_t port[8]; };
static void cb(const uint8_t ip[4], uint16_t port, void *u)
{
	struct got *g = u;
	if (g->n < 8) { memcpy(g->ip[g->n], ip, 4); g->port[g->n] = port; }
	g->n++;
}
static int parse(const char *s, struct got *g) { memset(g, 0, sizeof *g); return DodGC_MasterParseList(s, strlen(s), cb, g); }

int main(void){
	struct got g;
	CHECK(parse("{\"servers\":[{\"ip\":\"93.184.216.34\",\"port\":27015,\"gamedir\":\"dod\",\"map\":\"dod_charlie\",\"players\":3,\"max\":16,\"name\":\"My server\"},"
	            "{\"ip\":\"10.0.0.7\",\"port\":27016}]}", &g) == 2);
	CHECK(g.ip[0][0]==93 && g.ip[0][3]==34 && g.port[0]==27015 && g.ip[1][0]==10 && g.port[1]==27016);
	/* whitespace and key order */
	CHECK(parse(" { \"servers\" : [ { \"port\" : 27015 , \"ip\" : \"1.2.3.4\" } ] } ", &g) == 1);
	CHECK(parse("{\"servers\":[]}", &g) == 0);
	/* hostile entries are dropped, never delivered */
	CHECK(parse("{\"servers\":[{\"ip\":\"127.0.0.1\",\"port\":27015}]}", &g) == 0);
	CHECK(parse("{\"servers\":[{\"ip\":\"0.0.0.0\",\"port\":27015}]}", &g) == 0);
	CHECK(parse("{\"servers\":[{\"ip\":\"255.255.255.255\",\"port\":27015}]}", &g) == 0);
	CHECK(parse("{\"servers\":[{\"ip\":\"224.0.0.1\",\"port\":27015}]}", &g) == 0);
	CHECK(parse("{\"servers\":[{\"ip\":\"1.2.3.4\",\"port\":80}]}", &g) == 0);
	CHECK(parse("{\"servers\":[{\"ip\":\"1.2.3.4\",\"port\":70000}]}", &g) == 0);
	CHECK(parse("{\"servers\":[{\"ip\":\"1.2.3.04\",\"port\":27015}]}", &g) == 0);
	CHECK(parse("{\"servers\":[{\"ip\":\"1.2.3.4.5\",\"port\":27015}]}", &g) == 0);
	CHECK(parse("{\"servers\":[{\"ip\":\"1.2.3.256\",\"port\":27015}]}", &g) == 0);
	CHECK(parse("{\"servers\":[{\"ip\":\"example.com\",\"port\":27015}]}", &g) == 0);
	CHECK(parse("{\"servers\":[{\"ip\":\"1.2.3.4\"}]}", &g) == 0);
	CHECK(parse("{\"servers\":[{\"port\":27015}]}", &g) == 0);
	/* malformed: stop, keep what was already valid */
	CHECK(parse("{\"servers\":[{\"ip\":\"1.2.3.4\",\"port\":27015},{\"ip\":\"5.6.7.8\",\"port\":", &g) == 1);
	CHECK(parse("{\"servers\":[{\"ip\":\"1.2.3.4\",\"port\":27015},{\"ip\":\"5.6.7.8\",\"port\":27015,\"x\":{\"a\":1}}]}", &g) == 1);
	CHECK(parse("{\"servers\":[{\"ip\":\"1.2.\\u0034.4\",\"port\":27015}]}", &g) == 0);
	CHECK(parse("{\"servers\":[{\"ip\":\"1.2.3.4\",\"port\":27015,\"name\":\"a\\\"b\"}]}", &g) == 0);
	/* wrong envelope */
	CHECK(parse("", &g) == -1);
	CHECK(parse("[]", &g) == -1);
	CHECK(parse("{\"error\":\"slow down\"}", &g) == -1);
	CHECK(parse("{\"servers\":{}}", &g) == -1);
	CHECK(parse("<html>nope</html>", &g) == -1);
	/* entry cap */
	{
		char big[8192] = "{\"servers\":[";
		for (int i = 0; i < 100; i++) strcat(big, i ? ",{\"ip\":\"1.2.3.4\",\"port\":27015}" : "{\"ip\":\"1.2.3.4\",\"port\":27015}");
		strcat(big, "]}");
		CHECK(parse(big, &g) == DODGC_MASTER_MAX_ENTRIES);
		CHECK(DodGC_MasterParseList(big, DODGC_MASTER_MAX_REPLY + 1, cb, &g) == -1);
	}
	/* heartbeat body is always safe to embed */
	{
		char b[256];
		size_t n = DodGC_MasterBuildHeartbeat(b, sizeof b, 27015, "dod", "dod_charlie", 3, 16, "My \"evil\" \\ <b>server</b>");
		CHECK(n > 0 && strlen(b) == n);
		CHECK(!strchr(b + 1, '\\') && !strstr(b, "<") && !strstr(b, "\"evil\""));
		CHECK(strstr(b, "\"port\":27015") && strstr(b, "\"map\":\"dod_charlie\""));
		CHECK(DodGC_MasterBuildHeartbeat(b, sizeof b, 80, "dod", "x", 1, 2, "") == 0);
		CHECK(DodGC_MasterBuildHeartbeat(b, sizeof b, 27015, "dod", "x", 5, 2, "") == 0);
		CHECK(DodGC_MasterBuildHeartbeat(b, sizeof b, 27015, "dod", "", 1, 2, "") == 0);
		CHECK(DodGC_MasterBuildHeartbeat(b, 20, 27015, "dod", "x", 1, 2, "") == 0);
		n = DodGC_MasterBuildHeartbeat(b, sizeof b, 27015, "dod", "dod_charlie_with_a_very_long_map_name_here", 0, 16, "");
		CHECK(n > 0 && strstr(b, "\"max\":16"));
	}
	puts("ok");
	return 0;
}
'''

@unittest.skipUnless(shutil.which("gcc") or shutil.which("cc"), "no host C compiler")
class MasterHttpTests(unittest.TestCase):
    def test_parser_and_heartbeat(self):
        cc = shutil.which("gcc") or shutil.which("cc")
        with tempfile.TemporaryDirectory() as d:
            src = pathlib.Path(d) / "t.c"
            src.write_text(C)
            exe = pathlib.Path(d) / "t"
            subprocess.run([cc, "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", f"-I{INC}", str(src), "-o", str(exe)], check=True)
            out = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(out.returncode, 0, out.stdout + out.stderr)

if __name__ == "__main__":
    unittest.main()


CROSS = r'''
#include <stdio.h>
#include <string.h>
#include "dodgc_master_http.h"
static void cb(const uint8_t ip[4], uint16_t port, void *u){ (void)u; printf("%d.%d.%d.%d:%d\n", ip[0],ip[1],ip[2],ip[3], port); }
int main(int argc, char **argv){
	char in[9000]; size_t n = fread(in, 1, sizeof in, stdin);
	if (argc > 1) { char b[512]; size_t l = DodGC_MasterBuildHeartbeat(b, sizeof b, 27015, "dod", argv[1], 2, 16, argv[2]); fputs(l ? b : "FAIL", stdout); return 0; }
	return DodGC_MasterParseList(in, n, cb, NULL) < 0;
}
'''

@unittest.skipUnless(shutil.which("gcc") or shutil.which("cc"), "no host C compiler")
class CrossTests(unittest.TestCase):
    """The real Python master and the real C client code must agree."""
    def test_client_against_master(self):
        import json, sys
        sys.path.insert(0, str(ROOT / "scripts"))
        import dodgc_master as dm
        cc = shutil.which("gcc") or shutil.which("cc")
        with tempfile.TemporaryDirectory() as d:
            src = pathlib.Path(d) / "x.c"; src.write_text(CROSS)
            exe = pathlib.Path(d) / "x"
            subprocess.run([cc, "-Wall", f"-I{INC}", str(src), "-o", str(exe)], check=True)
            m = dm.Master(allow_private=True)
            for name in ("Plain", 'ev"il\\ <b>x</b> café ‮' * 2, "", "x" * 80):
                body = subprocess.run([str(exe), "dod_charlie", name], input="", capture_output=True, text=True).stdout
                st, doc = m.handle("POST", "/v1/servers", body.encode(), "93.184.216.34")
                self.assertEqual(st, 200, (name, body, doc))
                m.buckets.clear(); m.servers.clear()
            m.handle("POST", "/v1/servers", json.dumps({"port": 27016, "gamedir": "dod", "map": "dod_flash",
                     "players": 1, "max": 8, "name": "Hi"}).encode(), "93.184.216.35")
            listing = json.dumps(m.handle("GET", "/v1/servers?gamedir=dod", b"", "93.184.216.36")[1])
            out = subprocess.run([str(exe)], input=listing, capture_output=True, text=True)
            self.assertEqual(out.returncode, 0)
            self.assertEqual(out.stdout.split(), ["93.184.216.35:27016"])
