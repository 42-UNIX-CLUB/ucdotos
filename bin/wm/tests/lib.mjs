// SPDX-License-Identifier: GPL-2.0-or-later
// Helpers shared by the window manager tests (run with tools/shot.mjs --init=/bin/wm).
// Every check prints "PASS name" or "FAIL name"; run.sh counts them.

export const DECK = { desktop: [0x1d, 0x20, 0x21], focus: [0xfe, 0x80, 0x19], normal: [0x66, 0x5c, 0x54], bar: [0x3c, 0x38, 0x36], float: [0xfa, 0xbd, 0x2f] };
export const BRAND = { desktop: [0x0a, 0x2a, 0x23], focus: [0xfe, 0x77, 0x00], bar: [0x17, 0x4a, 0x3e] };
// Geometry of the default config at 1280x720: gap 12, bar 28 px high, master 55 %.
export const G = { gap: 12, top: 52, masterW: 684, stackX: 708 };

let fails = 0;
export function check(name, ok, info = '') {
  if (!ok) fails++;
  console.error(`${ok ? 'PASS' : 'FAIL'} ${name}${info ? '  (' + info + ')' : ''}`);
  return ok;
}
export function done(name) { console.error(`DONE ${name}: ${fails} failed`); }
export const same = (p, c, tol = 2) => c.every((v, i) => Math.abs(p[i] - v) <= tol);
export const hex = (p) => p.slice(0, 3).map((v) => v.toString(16).padStart(2, '0')).join('');

// Wait until fn() is true, polling every 25 ms.
export async function until(t, fn, ms = 4000) {
  const end = Date.now() + ms;
  while (Date.now() < end) { if (fn()) return true; await t.sleep(25); }
  return fn();
}
// uidemo processes that have a window right now, from their console output.
export function demos(t) {
  const alive = new Set(); // pids are reused, so replay the events in the order they were printed
  for (const m of t.log.matchAll(/uidemo (\d+): (RESIZE|bye)|pid (\d+) \(uidemo\) killed/g)) {
    if (m[2] === 'RESIZE') alive.add(m[1]); else alive.delete(m[1] || m[3]);
  }
  return alive;
}
export function count(t, re) { return [...t.log.matchAll(re)].length; }

// The first window comes from the launcher; later ones from pressing n in the focused uidemo.
export async function openDemo(t) {
  const before = demos(t).size;
  if (before === 0) {
    await t.tap('KeyD', { alt: true });
    await t.sleep(400);
    await t.type('uidemo');
    await t.sleep(100);
    await t.tap('Enter');
  } else await t.tap('KeyN');
  const ok = await until(t, () => demos(t).size > before);
  await t.sleep(250); // let it draw
  return ok;
}
export async function openDemos(t, n) {
  for (let i = 0; i < n; i++) if (!(await openDemo(t))) return false;
  return true;
}
// Ask the manager for its counters (Alt+... is not bound to stats, so a uidemo asks).
export async function stats(t) {
  const n = count(t, /wm: stats /g);
  await t.tap('KeyS');
  await until(t, () => count(t, /wm: stats /g) > n, 2000);
  const all = [...t.log.matchAll(/wm: stats (.*)/g)];
  const o = {};
  for (const kv of all[all.length - 1][1].split(' ')) { const [k, v] = kv.split('='); o[k] = Number(v); }
  return o;
}
// The config opens a welcome terminal and the keybinding cheat sheet at boot. The tests expect an empty desktop, so close it.
// Nothing here is a fixed wait for the terminal: either its window shows up, or the manager
// says it could not start it (an image built without term), whichever comes first.
export async function boot(t) {
  await until(t, () => /wm: \d+x\d+/.test(t.log), 10000);
  await until(t, () => same(t.pixel(400, 14), DECK.bar), 10000); // the bar is up
  // the keybinding cheat sheet covers the top right: hide it first ($mod /), if keys is in the image
  if (await until(t, () => /keys: \d+ bindings|wm: cannot start 'keys'/.test(t.log), 5000) && /keys: \d+ bindings/.test(t.log)) {
    await t.sleep(300);
    await t.tap('Slash', { alt: true });
    await t.sleep(300);
  }
  const noTerm = () => /wm: cannot start 'term'/.test(t.log);
  const windowUp = () => !same(t.pixel(301, 400), DECK.desktop);
  await until(t, () => windowUp() || noTerm(), 30000);
  if (windowUp()) {
    await t.sleep(300); // let it finish drawing before it is asked to close
    await t.tap('KeyQ', { alt: true, shift: true });
    await until(t, () => same(t.pixel(301, 400), DECK.desktop), 10000);
  }
  await t.sleep(200);
}
