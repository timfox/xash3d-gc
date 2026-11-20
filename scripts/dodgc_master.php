<?php
/*
 * DoD GC master server, PHP edition for ordinary shared hosting (PHP 7.4+, no extensions,
 * no database). Same API and the same rules as scripts/dodgc_master.py:
 *
 *   GET    <script>/v1/servers[?gamedir=dod&map=dod_charlie]   list live servers
 *   POST   <script>/v1/servers                                  register / heartbeat
 *   DELETE <script>/v1/servers/<port>                           unregister
 *   GET    <script>/healthz
 *
 * Upload this one file, e.g. as public_html/dodgc/master.php, and set the console cvar to
 *   dodgc_master http://example.com/dodgc/master.php
 * State is a JSON file (default: dodgc_data/ next to this file; it is created with a deny
 * rule, but a directory outside the web root is better: set DODGC_DATA below).
 */

const DODGC_DATA = '';          // state directory; '' = "<this dir>/dodgc_data"
const DODGC_PEER_HEADER = '';   // behind a CDN/proxy: e.g. 'HTTP_CF_CONNECTING_IP' or 'HTTP_X_FORWARDED_FOR' (last hop)
const DODGC_ALLOW_PRIVATE = false; // true = accept LAN/loopback servers (testing only)

const DG_TTL = 660;
const DG_MAX_SERVERS = 1000;
const DG_MAX_PER_IP = 4;
const DG_MAX_BODY = 512;
const DG_MAX_REPLY = 100;
const DG_RATE_BURST = 20;
const DG_RATE_PER_SEC = 0.5;
const DG_MAX_RATE_ENTRIES = 10000;

class DgError extends Exception
{
    public $status;
    public function __construct($status, $message)
    {
        parent::__construct($message);
        $this->status = $status;
    }
}

function dg_int($v, $lo, $hi, $field)
{
    if (!is_int($v) || $v < $lo || $v > $hi) {
        throw new DgError(400, "$field must be an integer $lo..$hi");
    }
    return $v;
}

function dg_text($v, $regex, $field, $required = true)
{
    if (!is_string($v) || !preg_match($regex, $v) || ($required && $v === '')) {
        throw new DgError(400, "$field is missing or has invalid characters");
    }
    return $v;
}

const DG_RE_GAMEDIR = '/\A[a-z0-9_]{1,16}\z/';
const DG_RE_MAP = '/\A[A-Za-z0-9_\-]{1,32}\z/';
const DG_RE_NAME = '/\A[A-Za-z0-9 _.\-\[\]()+!?#:\'@]{0,32}\z/';

function dg_validate_server($doc)
{
    if (!($doc instanceof stdClass)) {
        throw new DgError(400, 'body must be a JSON object');
    }
    $d = get_object_vars($doc);
    foreach (array_keys($d) as $k) {
        if (!in_array((string)$k, ['port', 'gamedir', 'map', 'players', 'max', 'name'], true)) {
            throw new DgError(400, 'unknown field');
        }
    }
    foreach (['port', 'gamedir', 'map', 'players', 'max'] as $k) {
        if (!array_key_exists($k, $d)) {
            throw new DgError(400, "$k is required");
        }
    }
    $rec = [
        'port' => dg_int($d['port'], 1024, 65535, 'port'),
        'gamedir' => dg_text($d['gamedir'], DG_RE_GAMEDIR, 'gamedir'),
        'map' => dg_text($d['map'], DG_RE_MAP, 'map'),
        'players' => dg_int($d['players'], 0, 32, 'players'),
        'max' => dg_int($d['max'], 1, 32, 'max'),
        'name' => dg_text(array_key_exists('name', $d) ? $d['name'] : '', DG_RE_NAME, 'name', false),
    ];
    if ($rec['players'] > $rec['max']) {
        throw new DgError(400, 'players exceeds max');
    }
    return $rec;
}

function dg_public_ip($text, $allow_private)
{
    if (strncasecmp($text, '::ffff:', 7) === 0) {
        $text = substr($text, 7);
    }
    if (filter_var($text, FILTER_VALIDATE_IP, FILTER_FLAG_IPV4) === false) {
        throw new DgError(400, filter_var($text, FILTER_VALIDATE_IP) === false ? 'bad peer address' : 'IPv4 servers only');
    }
    $n = ip2long($text);
    if ($n === false) {
        throw new DgError(400, 'bad peer address');
    }
    if (!$allow_private) {
        $ok = filter_var($text, FILTER_VALIDATE_IP, FILTER_FLAG_IPV4 | FILTER_FLAG_NO_PRIV_RANGE | FILTER_FLAG_NO_RES_RANGE) !== false
            && (($n >> 22) & 0x3FF) !== (ip2long('100.64.0.0') >> 22 & 0x3FF)   // 100.64.0.0/10 carrier NAT
            && ($n >> 24 & 255) < 224;
        if (!$ok) {
            throw new DgError(400, 'address is not publicly routable');
        }
    }
    return long2ip($n);
}

/* $state = ['servers' => [[ip, port, rec+last]], 'buckets' => [[ip, tokens, stamp]]]; returns [status, doc, state] */
function dg_handle($method, $target, $body, $peer, $state, $now, $allow_private)
{
    $servers = [];
    foreach ($state['servers'] as $s) {
        $servers[$s[0] . ':' . $s[1]] = $s;
    }
    $buckets = [];
    foreach ($state['buckets'] as $b) {
        $buckets[$b[0]] = [(float)$b[1], (float)$b[2]];
    }
    foreach ($servers as $k => $s) {
        if ($now - $s[2]['last'] > DG_TTL) {
            unset($servers[$k]);
        }
    }
    if (count($buckets) > DG_MAX_RATE_ENTRIES) {
        $buckets = [];
    }
    try {
        $ip = dg_public_ip($peer, $allow_private);
        list($tokens, $stamp) = isset($buckets[$ip]) ? $buckets[$ip] : [DG_RATE_BURST, $now];
        $tokens = min(DG_RATE_BURST, $tokens + ($now - $stamp) * DG_RATE_PER_SEC);
        if ($tokens < 1) {
            $buckets[$ip] = [$tokens, $now];
            $status = 429;
            $doc = ['error' => 'slow down'];
        } else {
            $buckets[$ip] = [$tokens - 1, $now];
            list($status, $doc) = dg_route($method, $target, $body, $ip, $servers, $now);
        }
    } catch (DgError $e) {
        $status = $e->status;
        $doc = ['error' => $e->getMessage()];
    }
    $out = ['servers' => array_values($servers), 'buckets' => []];
    foreach ($buckets as $ip2 => $b) {
        if ($now - $b[1] < 120) {
            $out['buckets'][] = [$ip2, $b[0], $b[1]];
        }
    }
    return [$status, $doc, $out];
}

function dg_route($method, $target, $body, $ip, &$servers, $now)
{
    if (strlen($target) > 256) {
        throw new DgError(414, 'request target too long');
    }
    $parts = explode('?', $target, 2);
    $path = $parts[0];
    $query = isset($parts[1]) ? $parts[1] : '';
    if ($path === '/healthz' && $method === 'GET') {
        return [200, ['ok' => true]];
    }
    if ($path === '/v1/servers') {
        if ($method === 'GET') {
            return dg_list($query, $servers);
        }
        if ($method === 'POST') {
            return dg_register($body, $ip, $servers, $now);
        }
        throw new DgError(405, 'method not allowed');
    }
    if (preg_match('/\A\/v1\/servers\/([0-9]{1,5})\z/', $path, $m)) {
        if ($method === 'DELETE') {
            unset($servers[$ip . ':' . (int)$m[1]]);
            return [200, ['ok' => true]];
        }
        throw new DgError(405, 'method not allowed');
    }
    throw new DgError(404, 'not found');
}

function dg_register($body, $ip, &$servers, $now)
{
    if (strlen($body) > DG_MAX_BODY) {
        throw new DgError(413, 'body too large');
    }
    $doc = json_decode($body, false, 8);
    if (json_last_error() !== JSON_ERROR_NONE) {
        throw new DgError(400, 'body is not valid JSON');
    }
    $rec = dg_validate_server($doc);
    $key = $ip . ':' . $rec['port'];
    if (!isset($servers[$key])) {
        if (count($servers) >= DG_MAX_SERVERS) {
            throw new DgError(503, 'server list is full');
        }
        $same = 0;
        foreach ($servers as $s) {
            if ($s[0] === $ip) {
                $same++;
            }
        }
        if ($same >= DG_MAX_PER_IP) {
            throw new DgError(429, 'too many servers from this address');
        }
    }
    $rec['last'] = $now;
    $servers[$key] = [$ip, $rec['port'], $rec];
    return [200, ['ok' => true, 'ttl' => DG_TTL]];
}

function dg_list($query, $servers)
{
    $q = [];
    if ($query !== '') {
        if (substr_count($query, '&') + 1 > 8) {
            throw new DgError(400, 'too many query fields');
        }
        parse_str($query, $q);
    }
    $gamedir = isset($q['gamedir']) && is_string($q['gamedir']) ? $q['gamedir'] : '';
    $map = isset($q['map']) && is_string($q['map']) ? $q['map'] : '';
    if ($gamedir !== '') {
        dg_text($gamedir, DG_RE_GAMEDIR, 'gamedir');
    }
    if ($map !== '') {
        dg_text($map, DG_RE_MAP, 'map');
    }
    $out = [];
    foreach ($servers as $s) {
        $r = $s[2];
        if (($gamedir !== '' && $r['gamedir'] !== $gamedir) || ($map !== '' && $r['map'] !== $map)) {
            continue;
        }
        $out[] = ['ip' => $s[0], 'port' => $s[1], 'gamedir' => $r['gamedir'], 'map' => $r['map'],
                  'players' => $r['players'], 'max' => $r['max'], 'name' => $r['name']];
        if (count($out) >= DG_MAX_REPLY) {
            break;
        }
    }
    return [200, ['servers' => $out]];
}

function dg_data_dir()
{
    $dir = DODGC_DATA !== '' ? DODGC_DATA : __DIR__ . '/dodgc_data';
    if (!is_dir($dir)) {
        mkdir($dir, 0700, true);
    }
    if (!file_exists($dir . '/.htaccess')) {
        @file_put_contents($dir . '/.htaccess', "Require all denied\nDeny from all\n");
    }
    return $dir;
}

function dg_run($method, $target, $body, $peer, $dir, $now, $allow_private)
{
    $lock = fopen($dir . '/lock', 'c');
    flock($lock, LOCK_EX);
    $file = $dir . '/state.json';
    $state = ['servers' => [], 'buckets' => []];
    if (is_file($file) && filesize($file) <= (1 << 20)) {
        $doc = json_decode((string)file_get_contents($file), true);
        if (is_array($doc)) {
            foreach (['servers', 'buckets'] as $k) {
                if (isset($doc[$k]) && is_array($doc[$k])) {
                    $state[$k] = array_values(array_filter($doc[$k], function ($e) use ($k) {
                        return is_array($e) && count($e) === 3 && is_string($e[0]) && ($k === 'buckets' || (is_int($e[1]) && is_array($e[2]) && isset($e[2]['last'])));
                    }));
                }
            }
        }
    }
    list($status, $out, $new) = dg_handle($method, $target, $body, $peer, $state, $now, $allow_private);
    file_put_contents($file . '.tmp', json_encode($new));
    rename($file . '.tmp', $file);
    flock($lock, LOCK_UN);
    fclose($lock);
    return [$status, $out];
}

function dg_main()
{
    $method = isset($_SERVER['REQUEST_METHOD']) ? $_SERVER['REQUEST_METHOD'] : 'GET';
    $peer = isset($_SERVER['REMOTE_ADDR']) ? $_SERVER['REMOTE_ADDR'] : '';
    if (DODGC_PEER_HEADER !== '' && !empty($_SERVER[DODGC_PEER_HEADER])) {
        $hops = explode(',', $_SERVER[DODGC_PEER_HEADER]);
        $peer = trim(end($hops));
    }
    $path = isset($_SERVER['PATH_INFO']) ? $_SERVER['PATH_INFO'] : '';
    if ($path === '') {
        $uri = explode('?', isset($_SERVER['REQUEST_URI']) ? $_SERVER['REQUEST_URI'] : '', 2)[0];
        $script = isset($_SERVER['SCRIPT_NAME']) ? $_SERVER['SCRIPT_NAME'] : '';
        $path = ($script !== '' && strpos($uri, $script) === 0) ? substr($uri, strlen($script)) : $uri;
    }
    $query = isset($_SERVER['QUERY_STRING']) ? $_SERVER['QUERY_STRING'] : '';
    $body = '';
    $status = 200;
    $out = null;
    if ($method === 'POST') {
        $len = isset($_SERVER['CONTENT_LENGTH']) ? $_SERVER['CONTENT_LENGTH'] : '';
        if (!ctype_digit((string)$len)) {
            $status = 411;
            $out = ['error' => 'Content-Length required'];
        } elseif ((int)$len > DG_MAX_BODY) {
            $status = 413;
            $out = ['error' => 'body too large'];
        } else {
            $body = (string)file_get_contents('php://input', false, null, 0, DG_MAX_BODY + 1);
        }
    }
    if ($out === null) {
        try {
            list($status, $out) = dg_run($method, $path . ($query !== '' ? '?' . $query : ''), $body, $peer,
                dg_data_dir(), microtime(true), DODGC_ALLOW_PRIVATE);
        } catch (Throwable $e) {
            $status = 500;
            $out = ['error' => 'internal error'];
        }
    }
    http_response_code($status);
    header('Content-Type: application/json');
    header('Cache-Control: no-store');
    header('X-Content-Type-Options: nosniff');
    echo json_encode($out);
}

if (PHP_SAPI !== 'cli') {
    dg_main();
}
