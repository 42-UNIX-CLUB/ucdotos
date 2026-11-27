// SPDX-License-Identifier: GPL-2.0-or-later
// One tab at a time, in Chromium: the first tab boots; a second one waits and says why; when
// the first closes, the second boots by itself. Then the reverse: a waiting tab that is closed
// leaves the running one alone. A ?nostore tab runs alongside.
//   node tools/browser.mjs --build=B --query='scale=1&reset' --script=tools/tests/browser-tabs.mjs
let checks = 0, failed = 0;
const check = (name, ok, extra = '') => { checks++; if (!ok) failed++; console.log(`  ${ok ? 'ok  ' : 'FAIL'}  ${name}${extra ? '  ' + extra : ''}`); };
const until = async (fn, ms = 20000) => { const t0 = Date.now(); while (Date.now() - t0 < ms) { try { if (await fn()) return true; } catch {} await new Promise((r) => setTimeout(r, 200)); } return false; };
const booted = (p) => p.eval('!!window.ucos && !window.ucosWaiting');
const waiting = (p) => p.eval('window.ucosWaiting === true && !window.ucos && !!document.getElementById("busy")');

export default async (t) => {
  check('tab A boots', await until(() => booted(t)));
  const b = await t.openTab('scale=1');
  check('tab B waits and shows the message', await until(() => waiting(b), 8000));
  const text = await b.eval('document.getElementById("busy").innerText');
  check('the message says UC.OS is open in another tab', /already open in another tab/.test(text), JSON.stringify(text.split('\n')[0]));
  await b.shot('tabs-01-waiting');
  await new Promise((r) => setTimeout(r, 1500));
  check('tab B is still waiting 1.5 s later (no boot behind the message)', await waiting(b));
  check('tab A is undisturbed (still running, no message)', await t.eval('!!window.ucos && !document.getElementById("busy")'));
  await t.closeFirst();
  const t0 = Date.now();
  check('A closed: B boots by itself', await until(() => booted(b)), `${Date.now() - t0} ms after the close`);
  check('B took over the store', await until(() => b.eval('window.ucos && /store: \\d+ records/.test(document.getElementById("log").textContent)'), 5000));
  // the reverse: a waiting tab that goes away changes nothing for the running one
  const c = await t.openTab('scale=1');
  check('tab C waits', await until(() => waiting(c), 8000));
  await c.close();
  await new Promise((r) => setTimeout(r, 1000));
  check('C closed while waiting: B still runs', await booted(b));
  const d = await t.openTab('scale=1');
  check('tab D waits', await until(() => waiting(d), 8000));
  const e = await t.openTab('scale=1&nostore');
  check('a ?nostore tab boots alongside', await until(() => booted(e)));
  check('...and keeps nothing (no store line)', await e.eval('/\\?nostore: this session keeps nothing/.test(document.getElementById("log").textContent)'));
  await e.close();
  check('D still waits while B runs', await waiting(d));
  await b.close();
  check('B closed: D boots', await until(() => booted(d)));
  await d.shot('tabs-02-took-over');
  await d.close();
  console.log(`browser-tabs: ${checks} checks, ${failed} failed`);
  if (failed) throw new Error(`${failed} checks failed`);
};
