# Hosting the DoD GC master on your own domain

Three ways to run the same API (`GET/POST /v1/servers`, `DELETE /v1/servers/<port>`,
`GET /healthz`; see `docs/GAMECUBE_NETWORKING.md`). All of them apply the same validation,
rate limits and caps, and `tests/test_dodgc_master_hosting.py` checks that they answer
identically.

| Host type | Files to upload | Console URL (`dodgc_master`) |
| --- | --- | --- |
| Shared hosting with PHP (easiest) | `scripts/dodgc_master.php` | `http://example.com/dodgc/master.php` |
| Shared hosting with CGI / cPanel Python | `scripts/dodgc_master_cgi.py` **and** `scripts/dodgc_master.py` | `http://example.com/cgi-bin/dodgc_master_cgi.py` |
| A VPS or any box you can run a process on | `scripts/dodgc_master.py` (or `scripts/Dockerfile.master`) | `http://master.example.com:8080` |

The console appends `/v1/servers` to whatever you set, so a script URL works as is: the
server sees `master.php/v1/servers` as a path-info request. No rewrite rules needed.

## PHP on shared hosting (about two minutes)

1. Upload `dodgc_master.php` to the web root, for example `public_html/dodgc/master.php`.
2. Optional: edit the three constants at the top. Set `DODGC_DATA` to a folder **outside**
   the web root if your host allows it; otherwise leave it empty and the script creates
   `dodgc_data/` next to itself with a deny-all `.htaccess` (check with
   `curl -I http://example.com/dodgc/dodgc_data/state.json`, it must not be 200). If the site is behind
   Cloudflare, set `DODGC_PEER_HEADER` to `HTTP_CF_CONNECTING_IP`.
3. Check it: `curl http://example.com/dodgc/master.php/healthz` returns `{"ok":true}`, and
   `curl http://example.com/dodgc/master.php/v1/servers` returns `{"servers":[]}`.
4. On every Cube (or in its config), set `dodgc_master http://example.com/dodgc/master.php`
   (or start with `-dodgcmaster <url>`).

## CGI or WSGI (Python hosting)

Upload both `dodgc_master_cgi.py` and `dodgc_master.py` to the same folder (for example
`cgi-bin/`), `chmod 755 dodgc_master_cgi.py`, and make sure the first line `#!/usr/bin/env python3`
works on the host. For Passenger / cPanel "Setup Python App" use the file as the WSGI entry,
callable `application`. Settings are environment variables: `DODGC_DATA`, `DODGC_PEER_HEADER`,
`DODGC_ALLOW_PRIVATE` (see the header of the file).

## Things that commonly go wrong

- **Forced HTTPS.** The Cube has no TLS. If the host or a plugin redirects http to https, the
  list never loads. Exempt the master path from the redirect, for example in `.htaccess`
  above your existing HTTPS rule:

  ```apache
  RewriteCond %{REQUEST_URI} !^/dodgc/
  ```

  Or give the master its own subdomain such as `master.timfox.xyz` with plain HTTP enabled.
- **Path info disabled.** If `…/master.php/healthz` returns a 404 from the web server, add
  `AcceptPathInfo On` in `.htaccess`, or rewrite instead:
  `RewriteRule ^v1/(.*)$ master.php/v1/$1 [L]` and `RewriteRule ^healthz$ master.php/healthz [L]`,
  then set `dodgc_master` to the folder URL.
- **Wrong peer address behind a CDN or proxy.** Servers are registered under the address the
  script sees. Set `DODGC_PEER_HEADER` (see above) only when the proxy sets that header itself.
- **Port forwarding.** A hosting Cube still needs UDP 27015 forwarded on the home router.
- Games using `ip` that is private or loopback are refused unless you enable the testing switch.

## What the host has to provide

A domain or subdomain with plain-HTTP access to one folder, PHP 7.4+ (or Python 3.8+ with CGI),
and write permission for one data folder. Nothing else: no database, cron, or extensions.
