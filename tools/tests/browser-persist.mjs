// SPDX-License-Identifier: GPL-2.0-or-later
// Persistence in a real browser (IndexedDB): fill, reload, verify; then reload in the middle
// of a large write and check that what survived is consistent.
//   make B=build dist && node tools/browser.mjs --build=build --query=reset --script=tools/tests/browser-persist.mjs
// Programs write their results to /dev/console, which reaches the page's console (t.log).
export default async (t) => {
  let checks = 0, failed = 0;
  const check = (name, ok, extra = '') => { checks++; if (!ok) failed++; console.log(`  ${ok ? 'ok  ' : 'FAIL'}  ${name}${extra ? '  ' + extra : ''}`); };
  let from = 0; /* t.log only grows: search what came after the last mark */
  const mark = () => { from = t.log.length; };
  const waitFor = async (re, ms) => { const end = Date.now() + ms; while (Date.now() < end) { const m = re.exec(t.log.slice(from)); if (m) return m; await t.sleep(200); } return null; };
  const reload = async () => { await t.eval('location.href = location.pathname, 1'); await t.sleep(4500); };
  await t.sleep(4500);
  const small = 2000, mb = 8;
  let t0 = Date.now();
  await t.type(`persisttest fill /home/user/pt ${small} ${mb} > /dev/console 2>&1\n`);
  const f = await waitFor(/persisttest: filled .* sync took (\d+) ms/, 120000);
  check('fill and sync in the browser', !!f, f ? `whole command ${((Date.now() - t0) / 1000).toFixed(1)} s, sync ${f[1]} ms` : '');
  const usage = await t.eval('navigator.storage.estimate().then((e) => JSON.stringify(e))');
  console.log('  info  storage estimate after fill: ' + usage);
  mark();
  t0 = Date.now();
  await reload();
  const ap = await waitFor(/\[store\] (\d+) records applied in ([\d.]+) ms/, 20000);
  const marks = await t.eval('JSON.stringify(window.ucosBoot)');
  console.log(`  info  after reload: ${ap ? ap[0] : 'no store line'}; boot marks ${marks}`);
  await t.type(`persisttest verify /home/user/pt ${small} ${mb} > /dev/console 2>&1\n`);
  const v = await waitFor(/persisttest verify: (\d+) checks, (\d+) failed/, 120000);
  check('after a reload everything is back', v && v[2] === '0', v ? v[0] : 'no result');
  mark();
  await t.type('persisttest bigwrite /home/user/big 64 > /dev/console 2>&1\n');
  const w = await waitFor(/wrote (2[4-9]|[3-9]\d) MiB/, 120000);
  await reload(); // in the middle of the write
  await t.type('persisttest bigcheck /home/user/big > /dev/console 2>&1; cat /proc/ucos-store > /dev/console\n');
  const b = await waitFor(/bigcheck: .*/, 60000);
  check('a reload in the middle of a 64 MiB write leaves a consistent file', b && /PASS/.test(b[0]), `${w ? w[0] : '?'} before the reload; ${b ? b[0] : 'no result'}`);
  const st = await waitFor(/state: .*\n(.|\n)*?bytes_loaded: \d+/, 5000);
  if (st) console.log('  info  /proc/ucos-store:\n' + st[0]);
  const lock = await t.eval('navigator.locks ? navigator.locks.query().then((q) => JSON.stringify(q.held.map((l) => l.name))) : "no Web Locks"');
  check('this tab holds the store lock', /ucos-store/.test(lock), lock);
  console.log(`browser-persist: ${checks} checks, ${failed} failed`);
};
