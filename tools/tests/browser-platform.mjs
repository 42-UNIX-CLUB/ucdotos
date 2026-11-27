// SPDX-License-Identifier: GPL-2.0-or-later
// UC.OS and the platform on one origin, in a real browser: the page asks who is signed in,
// signing in is a visit to /auth/login, and after it the router reaches the internet
// through the platform (the relay for a member, the proxy for anyone else signed in).
// Needs the platform running with this build's dist folder as its static folder and its
// test sign-in provider (ucdotweb README, "Try it locally"), and the internet behind it.
//   node tools/browser.mjs --at=http://127.0.0.1:8090 --build=build-NAME --script=tools/tests/browser-platform.mjs
let pass = 0, fail = 0;
const check = (name, cond, info = '') => {
  if (cond) pass++; else fail++;
  console.log(`${cond ? 'PASS' : 'FAIL'} ${name}${cond || !info ? '' : ' :: ' + String(info).slice(0, 500)}`);
};

export default async (t) => {
  const waitLog = async (re, from, ms = 15000) => {
    for (const t0 = Date.now(); Date.now() - t0 < ms; await t.sleep(100)) { const m = re.exec(t.log.slice(from)); if (m) return m[0]; }
    return null;
  };
  const get = (path) => `printf 'GET ${path} HTTP/1.1\\r\\nHost: deb.debian.org\\r\\nConnection: close\\r\\n\\r\\n' | nc -w 30 deb.debian.org 80`;

  // what the page wrote under the screen about the platform (boot/page.mjs says it there, not in the console)
  const said = async () => (/platform: [^\n]*/.exec(await t.eval("document.getElementById('log').textContent")) || [null])[0];

  // nobody is signed in: a new browser profile has no session
  let line = await waitLog(/routerd: this host can carry: [^\n]*/, 0);
  check('signed out, the router has only fetch()', line && /HTTP on port 80/.test(line) && !/platform/.test(line), line);
  let told = await said();
  check('the page says nobody is signed in', told && /not signed in/.test(told) && (await t.eval('window.ucosPlatform.signed_in')) === false, told);
  await t.sleep(1500);
  await t.type('sudo whoami > /dev/console 2>&1; echo "sudo-out rc=$?" > /dev/console\n');
  check('signed out, this session cannot become root', await waitLog(/sudo-out rc=[1-9]/, 0, 5000), t.log.slice(-300));
  await t.shot('platform-signed-out');

  // sign in: the provider answers at once in the test setup, and the platform sends the browser back to /
  let from = t.log.length;
  await t.cdp('Page.navigate', { url: t.origin + '/auth/login' });
  line = await waitLog(/routerd: this host can carry: [^\n]*/, from, 20000);
  const me = await t.eval('window.ucosPlatform') || {};
  told = await said();
  check('after /auth/login the page says who is signed in', me.signed_in === true && told && /signed in as \S+/.test(told), told);
  console.log(`INFO ${told}`);
  check('the address is the OS again, not a page of the platform', /^\/(\?.*)?$/.test(await t.eval('location.pathname + location.search')), await t.eval('location.href'));
  check(me.member ? 'a member: the router has TCP through the relay' : 'signed in: the router has the proxy',
    line && (me.member ? /through the platform's relay/ : /through the platform's proxy/).test(line), line);
  await t.sleep(2500);

  from = t.log.length;
  await t.type('sudo whoami > /dev/console 2>&1; echo "sudo-in rc=$?" > /dev/console\n');
  line = await waitLog(/sudo-in rc=\d+/, from, 5000);
  if (me.admin) check('an administrator of the platform may become root in the OS', line === 'sudo-in rc=0' && /\broot\b/.test(t.log.slice(from)), t.log.slice(from).slice(-300));
  else check('someone who is not an administrator cannot become root', line && line !== 'sudo-in rc=0', line);

  // out to the internet from a program
  from = t.log.length;
  await t.type(`${get('/debian/dists/stable/InRelease')} | head -c 2000 > /dev/console; echo; echo "got-one" > /dev/console\n`);
  await waitLog(/got-one/, from, 40000);
  let got = t.log.slice(from);
  if (me.member) {
    check('a raw HTTP exchange through the relay', /HTTP\/1\.1 200 OK/.test(got) && !/x-ucos-fetched/.test(got), got.slice(0, 500));
    check('the page asked for a relay token and got one', t.requests.some((r) => /\/api\/relay-token$/.test(r.url) && r.status === 200), JSON.stringify(t.requests.filter((r) => /relay/.test(r.url))));
    from = t.log.length;
    await t.type(`${get('/debian/dists/stable/Release')} | wc -c > /dev/console; echo "got-two" > /dev/console\n`);
    await waitLog(/got-two/, from, 60000);
    const n = Number((/^\s*(\d+)\s*$/m.exec(t.log.slice(from)) || [])[1] || 0);
    check('a larger file arrives whole through the relay (Release, over 100,000 bytes)', n > 100000, `wc -c said ${n}`);
    console.log(`INFO Release through the relay: ${n} bytes with its headers`);
    from = t.log.length;
    await t.type('nc -w 8 deb.debian.org 22 < /dev/null; echo "port22 rc=$?" > /dev/console\n');
    check('a port the relay does not carry is refused', await waitLog(/port22 rc=[1-9]/, from, 15000), t.log.slice(from).slice(-300));
  } else {
    check('the site refuses the browser (no CORS), the proxy carries the request', /HTTP\/1\.1 200/.test(got) && /x-ucos-fetched: [^\r\n]*\(through the platform's proxy\)/.test(got), got.slice(0, 600));
    check('the page sent it to /proxy on its own origin', t.requests.some((r) => r.url.startsWith(t.origin + '/proxy/deb.debian.org/') && r.status === 200), JSON.stringify(t.requests.filter((r) => /proxy/.test(r.url))));
  }
  // the console log arrives in pieces, each ending a line: join them before looking for the text
  check('the file is the real one (a signed Debian InRelease)', /BEGIN PGP SIGNED MESSAGE/.test(got.replace(/\r?\n/g, '')), got.slice(0, 500));
  await t.shot('platform-signed-in');

  console.log(`browser-platform: ${pass} passed, ${fail} failed`);
  if (fail) throw new Error('browser-platform failed');
};
