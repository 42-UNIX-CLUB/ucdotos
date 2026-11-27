// SPDX-License-Identifier: GPL-2.0-or-later
// Browser storage as a disk, in Chromium: what navigator.storage.estimate() reports and how
// it moves, the same figures inside UC.OS (/proc/ucos-store, stat -f /), persist(), and a
// full store under a quota made small with the DevTools call Storage.overrideQuotaForOrigin.
// Each phase is one boot that runs rootfs/usr/share/tests/store-browser.sh as init, so its
// output reaches the page's console; a tab closes before the next opens (one tab at a time).
//   node tools/browser.mjs --build=B --query='scale=1&reset' --script=tools/tests/browser-storage.mjs
let checks = 0, failed = 0;
const check = (name, ok, extra = '') => { checks++; if (!ok) failed++; console.log(`  ${ok ? 'ok  ' : 'FAIL'}  ${name}${extra ? '  ' + extra : ''}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const until = async (fn, ms = 20000) => { const t0 = Date.now(); while (Date.now() - t0 < ms) { try { if (await fn()) return true; } catch {} await sleep(200); } return false; };
const estJs = 'navigator.storage.estimate().then((e) => ({ usage: e.usage, quota: e.quota, idb: e.usageDetails && e.usageDetails.indexedDB }))';
const mb = (n) => (n / 1e6).toFixed(2) + ' MB';
async function phase(t, name, timeout = 40000) {
  const p = await t.openTab('scale=1&cmdline=' + encodeURIComponent(`init=/bin/sh -- /usr/share/tests/store-browser.sh ${name}`));
  const done = await until(() => new RegExp(`store-browser: ${name} done`).test(p.log), timeout);
  await sleep(1500); // let the last batch drain
  const text = p.log;
  const est = await p.eval(estJs).catch(() => null);
  const persisted = await p.eval('navigator.storage.persisted()').catch(() => null);
  await p.close();
  await sleep(500);
  for (const l of text.split('\n')) if (/^(state|quota|used|level|persist|blocks|WROTE|statfs|full after|after deleting|  FAIL|persisttest|\[store\])/.test(l.trim())) console.log('    | ' + l.trim());
  return { done, text, est, persisted };
}

export default async (t) => {
  check('the first tab boots with a store (?reset)', await until(() => t.eval('!!window.ucos')));
  await sleep(2000);
  const e0 = await t.eval(estJs);
  console.log(`  info  estimate at boot: usage ${e0.usage} B, quota ${mb(e0.quota)}; persisted(): ${await t.eval('navigator.storage.persisted()')}`);
  await t.closeFirst();
  await sleep(500);
  let r = await phase(t, 'info');
  const q = /quota_bytes: (\d+)/.exec(r.text), b = /blocks (\d+) free (\d+) bsize (\d+)/.exec(r.text);
  check('/proc/ucos-store has the browser quota', q && r.est && Math.abs(Number(q[1]) - r.est.quota) <= 1e6, q && q[1]);
  check('stat -f / reports that quota as the size of /', b && q && Math.abs(Number(b[1]) * Number(b[3]) - Number(q[1])) < 8192, b && `${b[1]} blocks of ${b[3]}`);
  r = await phase(t, 'write');
  check('8 MiB written and synced', /WROTE/.test(r.text));
  check('/proc/ucos-store says whether the browser keeps the data', /persistent: (yes|no)/.test(r.text), (/persistent: [a-z]+/.exec(r.text) || [''])[0]);
  console.log(`  info  estimate after the 8 MiB boot: usage ${r.est && mb(r.est.usage)}, IndexedDB ${r.est && mb(r.est.idb || 0)}; persisted(): ${r.persisted}`);
  const series = [];
  const probe = await t.openTab('scale=1&nostore'); // a tab that touches no storage, to watch the estimate
  for (let k = 0; k < 4; k++) { const e = await probe.eval(estJs); series.push(mb(e.usage)); await sleep(2000); }
  console.log(`  info  estimate over the next 8 s, every 2 s: ${series.join(', ')}`);
  // a small quota: the store fills
  const small = Math.round((r.est ? r.est.usage : 0) + 6e6);
  let ok = true;
  try { await probe.cdp('Storage.overrideQuotaForOrigin', { origin: t.origin, quotaSize: small }); } catch (err) { ok = false; console.log('  info  Storage.overrideQuotaForOrigin failed: ' + err.message); }
  const e2 = await probe.eval(estJs);
  console.log(`  info  estimate with the override: quota ${mb(e2.quota)}`);
  await probe.close();
  const took = ok && Math.abs(e2.quota - small) < 1e6;
  if (!took) console.log(`  info  NOT TESTED in Chromium: the quota could not be made small (override ${ok ? 'accepted, but estimate() still reports ' + mb(e2.quota) : 'refused'}); the full store is tested under Node (tools/tests/store-full.mjs)`);
  if (took) {
    r = await phase(t, 'full', 60000);
    console.log(`  info  estimate after the full boot: usage ${r.est && mb(r.est.usage)}, quota ${r.est && mb(r.est.quota)}`);
    check('a full store in Chromium: ENOSPC, the warning, recovery after deleting', /persisttest: full \d+ checks, 0 failed/.test(r.text));
  }
  console.log(`browser-storage: ${checks} checks, ${failed} failed`);
  if (failed) throw new Error(`${failed} checks failed`);
};
