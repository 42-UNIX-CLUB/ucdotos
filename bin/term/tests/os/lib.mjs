// SPDX-License-Identifier: GPL-2.0-or-later
// Helpers for the in-OS terminal tests (run by tools/shot.mjs with --init=/bin/term).
let pass = 0, fail = 0;
export function check(name, cond, info = '') {
  if (cond) pass++; else fail++;
  console.log(`${cond ? 'PASS' : 'FAIL'} ${name}${cond || !info ? '' : ' :: ' + info}`);
}
export function summary(name) {
  console.log(`${name}: ${pass} passed, ${fail} failed`);
  return fail;
}
// Ask term (stats mode) to print its visible text; returns { rows, cursor, view, alt }.
export async function screen(t) {
  const before = t.log.length;
  await t.tap('F12', { ctrl: true, alt: true, shift: true });
  for (let i = 0; i < 50; i++) {
    await t.sleep(20);
    const s = t.log.slice(before);
    const e = s.indexOf('[screen-end]');
    if (e >= 0) {
      const b = s.indexOf('[screen-begin]');
      const head = s.slice(b, s.indexOf('\n', b));
      const m = head.match(/(\d+)x(\d+) cursor (\d+),(\d+) view (\d+) alt (\d+)/);
      const rows = s.slice(b, e).split('\n').filter((l) => l.startsWith('[screen] ') || l === '[screen]').map((l) => l.slice(9));
      return { rows, cols: +m[1], nrows: +m[2], cx: +m[3], cy: +m[4], view: +m[5], alt: +m[6], text: rows.join('\n') };
    }
  }
  return { rows: [], text: '', cols: 0, nrows: 0 };
}
export async function waitFor(t, pred, ms = 5000) {
  const t0 = Date.now();
  while (Date.now() - t0 < ms) { if (pred()) return true; await t.sleep(25); }
  return false;
}
// the prompt of /bin/sh or the demo shell is on the last non-empty row
export async function waitPrompt(t, ms = 5000) {
  let s;
  const ok = await waitFor(t, () => true, 1);
  const t0 = Date.now();
  while (Date.now() - t0 < ms) {
    s = await screen(t);
    const last = s.rows.filter((r) => r.trim()).pop() || '';
    if (/\$$/.test(last.trim())) return s;
    await t.sleep(100);
  }
  return s;
}
