// SPDX-License-Identifier: GPL-2.0-or-later
// The router's ways out through the platform, offline: a stand-in for the platform (its
// proxy, its relay token and its Wisp relay, as the ucdotweb service speaks them), a local
// HTTP server behind it, and UC.OS booted under Node with that platform in the host layer
// (tools/run.mjs --platform). The real service is tested by tools/tests/platform.mjs.
//   node tools/tests/platform-local.mjs [--build=build] [--show]     --show prints everything UC.OS said
// Checks the host layer (boot/hostnet.mjs): the proxy is asked when the direct request
// fails, its refusals are told apart from a site's own answers, the session never comes
// from the program, and the relay client keeps to its credit, carries data both ways,
// reports a refused port and opens a new socket after the old one dropped.
import http from 'node:http';
import net from 'node:net';
import crypto from 'node:crypto';
import dns from 'node:dns';
import { spawn } from 'node:child_process';

const root = new URL('../..', import.meta.url).pathname;
const build = (process.argv.find((a) => a.startsWith('--build=')) || '--build=build').slice(8);
const show = process.argv.includes('--show');
let pass = 0, fail = 0;
const check = (name, cond, info = '') => {
  if (cond) pass++; else fail++;
  console.log(`${cond ? 'PASS' : 'FAIL'} ${name}${cond || !info ? '' : ' :: ' + String(info).slice(0, 500)}`);
};

const COOKIE = 'ucdotweb=the-session', CREDIT = 4, BIG = 3 << 20, UP = 1000000;
const big = Buffer.alloc(BIG);
for (let i = 0; i < big.length; i++) big[i] = (i * 7 + (i >> 8) * 13) & 255;
const seen = { outside: [], proxy: [], cookies: [], tokens: 0, sockets: 0, connects: [], overCredit: 0, upBytes: 0, programCookie: false, badRelay: [] };

// the site behind the platform: what a relayed TCP stream reaches
const site = http.createServer((req, res) => {
  res.sendDate = false;                         // so that two replies to one request are the same bytes
  const chunks = [];
  req.on('data', (c) => chunks.push(c));
  req.on('end', () => {
    const body = Buffer.concat(chunks);
    if (req.url === '/hello') { res.writeHead(200, { 'content-type': 'text/plain' }); res.end('hello through the relay\n'); }
    else if (req.url === '/big') { res.writeHead(200, { 'content-type': 'application/octet-stream' }); res.end(big); }
    else if (req.url === '/up') {
      seen.upBytes = body.length;
      res.writeHead(200, { 'content-type': 'text/plain' });
      res.end(`received ${body.length} bytes, sha256 ${crypto.createHash('sha256').update(body).digest('hex')}\n`);
    } else { res.writeHead(404, { 'content-type': 'text/plain' }); res.end('no\n'); }
  });
});

// the stand-in platform
let base = '';
const platform = http.createServer((req, res) => {
  const say = (status, headers, text) => { res.writeHead(status, { 'content-type': 'text/plain', ...headers }); res.end(text); };
  const signedIn = (req.headers.cookie || '') === COOKIE;
  if (req.url === '/api/relay-token' && req.method === 'POST') {
    if (!signedIn || req.headers.origin !== base) { say(403, {}, 'no\n'); return; }
    seen.tokens++;
    res.writeHead(200, { 'content-type': 'application/json' });
    res.end(JSON.stringify({ token: 'token-' + seen.tokens, expires_in: 300 }));
    return;
  }
  const m = /^\/proxy\/([^/]+)(\/.*)$/.exec(req.url);
  if (!m) {                                     // a page of the platform itself: nothing a program asks for may arrive here
    seen.outside.push(`${req.method} ${req.url}${signedIn ? ' with the session' : ''}`);
    if (req.url === '/api/me') { res.writeHead(200, { 'content-type': 'application/json' }); res.end(JSON.stringify({ signed_in: signedIn, secret: 'of-the-session' })); }
    else say(404, {}, 'not found\n');
    return;
  }
  seen.proxy.push(`${req.method} ${m[1]}${m[2]}`);
  seen.cookies.push(req.headers.cookie || '');
  if (/program-cookie/.test(req.headers.cookie || '')) seen.programCookie = true;
  if (req.headers['x-ucos-proxy'] !== '1') { say(403, { 'x-ucos-proxy-refused': 'no-header' }, 'the request did not come from UC.OS\n'); return; }
  if (!signedIn) { say(401, { 'x-ucos-proxy-refused': 'signed-out' }, 'sign in first\n'); return; }
  const fromSite = { 'content-security-policy': 'sandbox', 'x-content-type-options': 'nosniff' };
  switch (m[1]) {
    case 'listed.test':                         // a host on the list
      if (m[2] === '/missing') say(404, fromSite, 'the site has no such page\n');
      else say(200, { ...fromSite, 'x-site': 'yes' }, `the proxy fetched ${m[2]}\n`);
      break;
    case 'quota.test': say(429, { 'x-ucos-proxy-refused': 'quota' }, 'your daily amount through the proxy is used up\n'); break;
    case 'old.test': say(404, {}, 'not on the list\n'); break;   // a platform without the refusal header
    default: say(404, { 'x-ucos-proxy-refused': 'not-listed' }, 'this host is not on the list\n');
  }
});

// the relay: WebSocket (RFC 6455) by hand, Wisp v1 inside
platform.on('upgrade', (req, sock) => {
  const offered = (req.headers['sec-websocket-protocol'] || '').split(',').map((s) => s.trim());
  const token = (offered.find((p) => p.startsWith('ucos-token.')) || '').slice(11);
  if (req.url !== '/relay' || req.headers.origin !== base || !offered.includes('wisp') || token !== 'token-' + seen.tokens) {
    seen.badRelay.push(`${req.url} origin=${req.headers.origin} protocols=${offered.join('|')}`);
    sock.end('HTTP/1.1 403 Forbidden\r\n\r\n');
    return;
  }
  seen.sockets++;
  const accept = crypto.createHash('sha1').update(req.headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
  sock.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\nSec-WebSocket-Protocol: wisp\r\n\r\n`);
  const frame = (payload) => {
    const n = payload.length;
    const head = n < 126 ? Buffer.from([0x82, n]) : n < 65536 ? Buffer.from([0x82, 126, n >> 8, n & 255]) : Buffer.concat([Buffer.from([0x82, 127]), Buffer.alloc(8)]);
    if (n >= 65536) head.writeBigUInt64BE(BigInt(n), 2);
    if (!sock.destroyed) sock.write(Buffer.concat([head, payload]));
  };
  const packet = (type, id, payload) => { const p = Buffer.alloc(5 + payload.length); p[0] = type; p.writeUInt32LE(id, 1); payload.copy(p, 5); frame(p); };
  const u32 = (n) => { const b = Buffer.alloc(4); b.writeUInt32LE(n); return b; };
  const streams = new Map();
  packet(3, 0, u32(CREDIT));
  const wisp = (p) => {
    const type = p[0], id = p.readUInt32LE(1), body = p.subarray(5);
    if (type === 1) {
      const port = body.readUInt16LE(1), host = body.subarray(3).toString();
      seen.connects.push(`${host}:${port}`);
      if (host === '203.0.113.99') { sock.destroy(); return; }          // the relay drops
      if (port !== 80 && port !== 443) { packet(4, id, Buffer.from([0x48])); return; }
      const s = { up: net.connect(site.address().port, '127.0.0.1'), allowed: CREDIT, got: 0 };
      streams.set(id, s);
      s.up.on('data', (d) => { for (let off = 0; off < d.length; off += 60000) packet(2, id, d.subarray(off, off + 60000)); });
      s.up.on('end', () => { if (streams.delete(id)) packet(4, id, Buffer.from([0x02])); });
      s.up.on('error', () => { if (streams.delete(id)) packet(4, id, Buffer.from([0x03])); });
    } else if (type === 2) {
      const s = streams.get(id);
      if (!s) return;
      if (--s.allowed < 0) seen.overCredit++;
      s.up.write(body);
      if (++s.got % CREDIT === 0) setTimeout(() => { s.allowed = CREDIT; packet(3, id, u32(CREDIT)); }, 2); // late, so a client that does not wait is caught
    } else if (type === 4) { const s = streams.get(id); if (s) { streams.delete(id); s.up.destroy(); } }
  };
  let buf = Buffer.alloc(0);
  sock.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    for (;;) {
      if (buf.length < 2) return;
      let n = buf[1] & 127, at = 2;
      if (n === 126) { if (buf.length < 4) return; n = buf.readUInt16BE(2); at = 4; }
      else if (n === 127) { if (buf.length < 10) return; n = Number(buf.readBigUInt64BE(2)); at = 10; }
      const masked = buf[1] & 128;
      if (buf.length < at + (masked ? 4 : 0) + n) return;
      const mask = masked ? buf.subarray(at, at + 4) : null;
      at += masked ? 4 : 0;
      const payload = Buffer.from(buf.subarray(at, at + n));
      if (mask) for (let i = 0; i < n; i++) payload[i] ^= mask[i & 3];
      const op = buf[0] & 15;
      buf = buf.subarray(at + n);
      if (op === 8) { sock.end(); return; }
      if (op === 2 && n >= 5) wisp(payload);
    }
  });
  sock.on('close', () => { for (const s of streams.values()) s.up.destroy(); streams.clear(); });
  sock.on('error', () => {});
});

// one boot of UC.OS; the shell runs the commands with a router as init starts it
function os(flags, commands) {
  return new Promise((done) => {
    const input = `routerd &\nsleep 1\n${fn}\n${commands}\nkill %1\nexit\n`;
    if (input.length > 1000) throw new Error(`the commands are ${input.length} bytes: the console takes 1024 at once and drops the rest`);
    const child = spawn(process.execPath, [root + 'tools/run.mjs', `--build=${build}`, '--init=/bin/sh', `--platform=${base}`, `--platform-cookie=${COOKIE}`, ...flags, `--input=${input.replace(/\n/g, '\\n')}`],
      { cwd: root, stdio: ['ignore', 'pipe', 'pipe'] });
    let out = '';
    const t0 = Date.now();
    child.stdout.on('data', (d) => { out += d; });
    child.stderr.on('data', (d) => { out += d; });
    const timer = setTimeout(() => { out += '\nTIMED OUT\n'; child.kill('SIGKILL'); }, 120000);
    child.on('close', (code) => { clearTimeout(timer); if (show) console.log(`---- UC.OS said (exit ${code}, ${((Date.now() - t0) / 1000).toFixed(1)} s):\n${out}\n----`); done(out); });
  });
}
// requests go to an address, not a name: there is no DNS for .test names here. The router's
// HTTP path takes the site from the Host header; the relay is given the address.
const fn = 'g() { printf "GET $2 HTTP/1.1\\r\\nHost: $1\\r\\n$3Connection: close\\r\\n\\r\\n" | nc -w 20 203.0.113.10 80; }';
const req = (host, path, more = '', tail = '') => `g ${host} ${path}${more ? ` '${more}'` : ''} ${tail}`;

await new Promise((r) => site.listen(0, '127.0.0.1', r));
await new Promise((r) => platform.listen(0, '127.0.0.1', r));
base = `http://127.0.0.1:${platform.address().port}`;

// ---- signed in, not a member: HTTP only, the proxy behind the direct request
// The direct request has to fail, as a browser's does for a site without CORS headers. Here
// it fails because .test names do not exist. A network that answers for every name (some
// hotel and provider resolvers do) would spoil that: then the proxy is asked first instead.
const wildcard = await dns.promises.lookup('listed.test').then(() => true, () => false);
if (wildcard) console.log('INFO this network gives an address for .test names: the proxy is asked first, the order "direct, then proxy" is not tested');
let out = await os(wildcard ? ['--platform-proxy-first'] : [], [
  req('listed.test', '/one', 'Cookie: program-cookie=1\\r\\n'), 'echo; echo MARK1',
  req('listed.test', '/two'), 'echo; echo MARK2',
  req('listed.test', '/missing'), 'echo; echo MARK3',
  req('unlisted.test', '/'), 'echo "rc=$?"; echo MARK4',
  req('quota.test', '/'), 'echo; echo MARK5',
  req('old.test', '/'), 'echo "rc=$?"; echo MARK6',
  'nc -w 5 203.0.113.10 443 < /dev/null; echo "port443 rc=$?"; echo MARK7',
  `${req('..', '/api/me')}; ${req('%%2e%%2e', '/api/me')}; ${req('.', '/api/me')}; echo MARK8`,
].join('\n'));
let p = out.split(/MARK\d/);
check('the router says the proxy is there and TCP is not', /through the platform's proxy for the hosts on its list/.test(out) && !/TCP to/.test(out), out.slice(0, 700));
check('the direct request fails, the proxy carries it', /HTTP\/1\.1 200/.test(p[0]) && /the proxy fetched \/one/.test(p[0]) && /x-site: yes/.test(p[0]), p[0].slice(-600));
check('the reply says it came through the proxy', /x-ucos-fetched: http:\/\/listed\.test\/one \(through the platform's proxy\)/.test(p[0]), p[0].slice(-600));
check('the next request to that host works too', /the proxy fetched \/two/.test(p[1]), p[1]);
check('a 404 of the site itself reaches the program', /HTTP\/1\.1 404/.test(p[2]) && /the site has no such page/.test(p[2]), p[2]);
if (!wildcard) check('a host that is not on the list: the program gets the failure of the direct request', !/HTTP\/1\.1 [234]/.test(p[3]) && !/not on the list/.test(p[3]), p[3]);
check('a refusal that is not about the list reaches the program as it is', /HTTP\/1\.1 429/.test(p[4]) && /daily amount/.test(p[4]), p[4]);
if (!wildcard) check('a platform without the refusal header: its 404 counts as "not on the list"', !/HTTP\/1\.1 [234]/.test(p[5]) && !/not on the list/.test(p[5]), p[5]);
check('without the relay, a port other than 80 is refused', /port443 rc=[1-9]/.test(p[6]), p[6]);
check('the proxy saw each request once', ['GET listed.test/one', 'GET listed.test/two', 'GET listed.test/missing', 'GET unlisted.test/', 'GET quota.test/', 'GET old.test/'].every((r) => seen.proxy.filter((x) => x === r).length === 1), seen.proxy.join(', '));
check('every proxy request carried the session of the host layer, never a cookie of the program', seen.cookies.length > 0 && seen.cookies.every((c) => c === COOKIE) && !seen.programCookie, seen.cookies.join(' | '));
check('no relay token was asked for', seen.tokens === 0 && seen.sockets === 0);
check('a host name of ".." does not turn the proxy address into a page of the platform', seen.outside.length === 0 && !/of-the-session/.test(p[7]) && !seen.proxy.some((r) => /api\/me/.test(r)), `${seen.outside.join(', ')} ; ${seen.proxy.join(', ')} ; ${p[7]}`);

// ---- a member: TCP through the relay
out = await os(['--platform-member'], [
  req('site.test', '/hello'), 'echo; echo MARK1',
  req('site.test', '/big', '', '| wc -c'), 'echo MARK2',
  req('site.test', '/big', '', '| sha256sum'), 'echo MARK3',
  `{ printf 'POST /up HTTP/1.1\\r\\nHost: site.test\\r\\nContent-Length: ${UP}\\r\\nConnection: close\\r\\n\\r\\n'; head -c ${UP} /dev/zero; } | nc -w 20 203.0.113.10 80`, 'echo; echo MARK4',
  'nc -w 5 203.0.113.10 22 < /dev/null; echo "port22 rc=$?"; echo MARK5',
  `${req('site.test', '/hello', '', '> /tmp/a &')} ${req('site.test', '/hello', '', '> /tmp/b &')} ${req('site.test', '/hello', '', '> /tmp/c')}; sleep 1; cat /tmp/a /tmp/b /tmp/c | grep -c 'hello through the relay'`, 'echo MARK6',
  'nc -w 5 203.0.113.99 80 < /dev/null; echo "dropped rc=$?"; echo MARK7',
  req('site.test', '/hello'), 'echo; echo MARK8',
].join('\n'));
p = out.split(/MARK\d/);
check('the router says TCP goes through the relay', /TCP to ports 80 and 443 \(through the platform's relay\)/.test(out), out.slice(0, 700));
check('a raw HTTP exchange through the relay', /HTTP\/1\.1 200 OK/.test(p[0]) && /hello through the relay/.test(p[0]) && !/x-ucos-fetched/.test(p[0]), p[0].slice(-500));
const count = Number(((p[1] || '').match(/^\s*(\d+)\s*$/m) || [])[1] || 0);
check(`${BIG} bytes arrive whole through the relay`, count > BIG && count < BIG + 1000, `wc -c said ${count}`);
const whole = await new Promise((r) => {
  const s = net.connect(site.address().port, '127.0.0.1', () => s.end('GET /big HTTP/1.1\r\nHost: site.test\r\nConnection: close\r\n\r\n'));
  const h = crypto.createHash('sha256');
  s.on('data', (d) => h.update(d)); s.on('end', () => r(h.digest('hex')));
});
const sum = ((p[2] || '').match(/[0-9a-f]{64}/) || [])[0];
check('and they are the same bytes (sha256 of the whole reply)', !!sum && sum === whole, `${sum} against ${whole}`);
const zeros = crypto.createHash('sha256').update(Buffer.alloc(UP)).digest('hex');
check(`${UP} bytes go up through the relay, unchanged`, (p[3] || '').includes(`received ${UP} bytes, sha256 ${zeros}`), (p[3] || '').slice(-300));
check(`the client never sent more DATA packets than its credit of ${CREDIT}`, seen.overCredit === 0, `${seen.overCredit} packets over`);
check('a port the relay does not carry is refused', /port22 rc=[1-9]/.test(p[4] || ''), p[4]);
check('three streams at once on the one socket', /^\s*3\s*$/m.test(p[5] || ''), p[5]);
check('when the relay drops, the open connection ends', /dropped rc=/.test(p[6] || ''), p[6]);
check('and the next connection opens a new socket with a new token', /hello through the relay/.test(p[7] || '') && seen.tokens === 2 && seen.sockets === 2, `tokens ${seen.tokens}, sockets ${seen.sockets}: ${(p[7] || '').slice(-200)}`);
check('the relay was given addresses and ports as the program asked', seen.connects.includes('203.0.113.10:80') && seen.connects.includes('203.0.113.10:22'), seen.connects.join(', '));
check('no relay request was refused by the stand-in (origin, protocols, token)', seen.badRelay.length === 0, seen.badRelay.join(' ; '));
check('members do not use the proxy', seen.proxy.length === 6, seen.proxy.join(', '));

console.log(`platform-local: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
