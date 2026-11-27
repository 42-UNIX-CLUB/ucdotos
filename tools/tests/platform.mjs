// SPDX-License-Identifier: GPL-2.0-or-later
// The router's ways out through the platform (the ucdotweb service), end to end: UC.OS under
// Node, a running platform, the real internet behind it. Not part of `make test`: it needs
// the platform and its test sign-in provider running (ucdotweb README, "Try it locally"),
// and the platform must reach deb.debian.org.
//   node tools/tests/platform.mjs --build=build-NAME [--platform=http://127.0.0.1:8090]
// It signs in as a browser would (the redirects of /auth/login, with the cookies), asks
// /api/me, then boots UC.OS with that session in the host layer (tools/run.mjs --platform)
// and makes HTTP requests with nc from inside:
//   signed in         the proxy carries a listed host; a host that is not listed goes out directly
//   signed in, member TCP through the relay to port 80; another port is refused
import { spawnSync } from 'node:child_process';

const args = process.argv.slice(2);
const opt = (n, d) => { const a = args.find((x) => x.startsWith(`--${n}=`)); return a ? a.slice(n.length + 3) : d; };
const build = opt('build', 'build'), platform = opt('platform', 'http://127.0.0.1:8090');
const root = new URL('../..', import.meta.url).pathname;
let pass = 0, fail = 0;
const check = (name, cond, info = '') => {
  if (cond) pass++; else fail++;
  console.log(`${cond ? 'PASS' : 'FAIL'} ${name}${cond || !info ? '' : ' :: ' + String(info).slice(0, 400)}`);
};

// sign in: follow the redirects by hand, keeping each origin's cookies apart
const jar = new Map();
const cookiesFor = (u) => [...(jar.get(u.origin) || new Map())].map(([k, v]) => `${k}=${v}`).join('; ');
async function visit(url) {
  let u = new URL(url);
  for (let hop = 0; hop < 12; hop++) {
    const r = await fetch(u, { redirect: 'manual', headers: { cookie: cookiesFor(u) } });
    for (const line of r.headers.getSetCookie()) {
      const [pair] = line.split(';'), i = pair.indexOf('=');
      if (!jar.has(u.origin)) jar.set(u.origin, new Map());
      jar.get(u.origin).set(pair.slice(0, i).trim(), pair.slice(i + 1).trim());
    }
    const next = r.headers.get('location');
    if (r.status < 300 || r.status >= 400 || !next) return r;
    u = new URL(next, u);
  }
  throw new Error('too many redirects while signing in');
}
let me = null;
try {
  await visit(platform + '/auth/login');
  me = await (await fetch(platform + '/api/me', { headers: { cookie: cookiesFor(new URL(platform)) } })).json();
} catch (e) {
  console.log(`FAIL the platform at ${platform} did not answer (${e && e.message}): start it and its sign-in provider first`);
  console.log('platform: 0 passed, 1 failed');
  process.exit(1);
}
const cookie = cookiesFor(new URL(platform));
check('sign-in through the provider gives a session', me && me.signed_in === true, JSON.stringify(me));
console.log(`INFO signed in as ${me.login}: member ${me.member}, administrator ${me.admin}`);

// one boot of UC.OS as the given kind of user; the shell runs the commands and stops the router
function os(flags, commands) {
  const input = `routerd & sleep 1\n${commands}\nkill %1\nexit\n`.replace(/\n/g, '\\n');
  if (input.length > 1000) throw new Error(`the commands are ${input.length} bytes: the console takes 1024 at once and drops the rest`);
  const r = spawnSync(process.execPath, ['tools/run.mjs', `--build=${build}`, '--init=/bin/sh', `--platform=${platform}`, `--platform-cookie=${cookie}`, ...flags, `--input=${input}`],
    { cwd: root, encoding: 'utf8', timeout: 90000, maxBuffer: 64 << 20 });
  return (r.stdout || '') + (r.stderr || '');
}
const get = (host, path, extra = '') => `printf 'GET ${path} HTTP/1.1\\r\\nHost: ${host}\\r\\nConnection: close\\r\\n\\r\\n' | nc -w 30 ${host} 80 ${extra}`;

// signed in, not a member: the proxy first (Node has no CORS, so the direct fetch would not fail)
let out = os(['--platform-proxy-first'], `${get('deb.debian.org', '/debian/dists/stable/InRelease', '| head -c 1200')}; echo; echo MARK1; ${get('example.com', '/', '| head -c 600')}; echo; echo MARK2`);
const [a, b] = [out.split('MARK1')[0], (out.split('MARK1')[1] || '').split('MARK2')[0]];
check('the router says the proxy is there', /through the platform's proxy for the hosts on its list/.test(out), out.slice(0, 600));
check('a listed host answers through the proxy', /HTTP\/1\.1 200/.test(a) && /x-ucos-fetched: [^\r\n]*\(through the platform's proxy\)/.test(a), a.slice(-900));
check('the file is the real one (a signed Debian InRelease)', /BEGIN PGP SIGNED MESSAGE/.test(a) && /Origin: Debian/.test(a), a.slice(-500));
check('a host that is not on the list goes out directly', /HTTP\/1\.1 200/.test(b) && /x-ucos-fetched/.test(b) && !/through the platform's proxy\)/.test(b), b.slice(0, 700));

// a member: TCP through the relay
if (me.member) {
  out = os(['--platform-member'], `${get('deb.debian.org', '/debian/dists/stable/InRelease', '| head -c 900')}; echo; echo MARK1; ${get('deb.debian.org', '/debian/dists/stable/Release', '| wc -c')}; echo MARK2; nc -w 8 deb.debian.org 22 < /dev/null; echo "port22 rc=$?"; echo MARK3`);
  const p = out.split(/MARK[123]/);
  check('the router says TCP goes through the relay', /TCP to ports 80 and 443 \(through the platform's relay\)/.test(out), out.slice(0, 600));
  check('a raw HTTP exchange through the relay', /HTTP\/1\.1 200 OK/.test(p[0]) && !/x-ucos-fetched/.test(p[0]) && /BEGIN PGP SIGNED MESSAGE/.test(p[0]), p[0].slice(-900));
  const n = Number(((p[1] || '').match(/^\s*(\d+)\s*$/m) || [])[1] || 0);
  check('a larger file arrives whole through the relay (Release, over 100,000 bytes)', n > 100000, `wc -c said ${n}: ${(p[1] || '').slice(0, 200)}`);
  console.log(`INFO Release through the relay: ${n} bytes with its headers`);
  check('a port the relay does not carry is refused', /port22 rc=[1-9]/.test(p[2] || ''), (p[2] || '').slice(0, 300));
} else console.log('INFO the signed-in user is not a member: the relay checks are skipped (start the provider with -member)');

console.log(`platform: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
