// SPDX-License-Identifier: GPL-2.0-or-later
// Zoom: Ctrl+= and Ctrl+- are taken by the manager and sent to the focused window as UI_EV_ZOOM.
// The welcome terminal then draws bigger or smaller text, so its grid has fewer or more cells.
// Needs term in the image: make B=build-wm PROGS="wm bar launcher uidemo term sh".
// Nothing here waits a fixed time for a zoom to take effect: the zoom event and the key that
// asks for the screen dump reach the terminal in order, so the dump always shows the result.
import { check, done, until, count, same, demos, openDemo, DECK } from './lib.mjs';

// The focused terminal prints "[screen-begin] COLSxROWS ..." on Ctrl+Alt+Shift+F12.
async function grid(t) {
  const n = count(t, /\[screen-begin\]/g);
  await t.tap('F12', { ctrl: true, alt: true, shift: true });
  await until(t, () => count(t, /\[screen-begin\]/g) > n, 10000);
  const all = [...t.log.matchAll(/\[screen-begin\] (\d+)x(\d+)/g)];
  if (all.length <= n) return null;
  const m = all[all.length - 1];
  return { cols: Number(m[1]), rows: Number(m[2]) };
}
// Ask again until the focused terminal's grid satisfies ok (a new window takes a moment to appear).
async function gridWhen(t, ok, ms = 30000) {
  const end = Date.now() + ms;
  let g = await grid(t);
  while (!(g && ok(g)) && Date.now() < end) { await t.sleep(200); g = await grid(t); }
  return g;
}
const fmt = (g) => (g ? `${g.cols}x${g.rows}` : 'none');
const eq = (a, b) => !!a && !!b && a.cols === b.cols && a.rows === b.rows;

export default async (t) => {
  check('manager up', await until(t, () => /wm: \d+x\d+/.test(t.log), 10000));
  check('welcome terminal up', await until(t, () => !same(t.pixel(301, 400), DECK.desktop), 30000));
  const g0 = await gridWhen(t, () => true);
  check('terminal answers the screen dump', !!g0, fmt(g0));
  if (!g0) { done('zoom'); return; }
  t.shot('50-zoom-normal');

  await t.tap('Equal', { ctrl: true });
  await t.tap('Equal', { ctrl: true });
  const g1 = await grid(t);
  t.shot('51-zoom-in');
  check('Ctrl+= twice: fewer columns and rows', g1 && g1.cols < g0.cols && g1.rows < g0.rows, fmt(g0) + ' -> ' + fmt(g1));

  await t.tap('Equal', { ctrl: true, shift: true });
  const g2 = await grid(t);
  check('Ctrl++ (Shift held) zooms in too', g2 && g2.cols < g1.cols, fmt(g1) + ' -> ' + fmt(g2));

  await t.tap('Digit0', { ctrl: true });
  const g3 = await grid(t);
  check('Ctrl+0: back to the normal size', eq(g3, g0), fmt(g3));

  await t.tap('Minus', { ctrl: true });
  await t.tap('Minus', { ctrl: true });
  const g4 = await grid(t);
  t.shot('52-zoom-out');
  check('Ctrl+- twice: more columns and rows', g4 && g4.cols > g0.cols && g4.rows > g0.rows, fmt(g0) + ' -> ' + fmt(g4));

  for (let i = 0; i < 20; i++) await t.tap('Minus', { ctrl: true });
  const g5 = await grid(t);
  await t.tap('Equal', { ctrl: true });
  const g6 = await grid(t);
  check('at the smallest size one Ctrl+= zooms in at once', g5 && g6 && g6.cols < g5.cols, fmt(g5) + ' -> ' + fmt(g6));
  await t.tap('Digit0', { ctrl: true });

  // only the focused window zooms: a second terminal keeps its size while the first one changes
  await t.tap('Enter', { alt: true });
  const b0 = await gridWhen(t, (g) => g.cols < g0.cols); // the new one is focused and tiled, so narrower
  check('a second terminal opens next to the first', b0 && b0.cols < g0.cols, fmt(b0));
  await t.tap('KeyK', { alt: true }); // focus the first one
  const a0 = await grid(t);
  await t.tap('Equal', { ctrl: true });
  await t.tap('Equal', { ctrl: true });
  const a1 = await grid(t);
  check('the focused terminal zooms', a0 && a1 && a1.cols < a0.cols, fmt(a0) + ' -> ' + fmt(a1));
  await t.tap('KeyJ', { alt: true }); // back to the second one
  const b1 = await grid(t);
  check('the other terminal keeps its size', eq(b1, b0), fmt(b0) + ' -> ' + fmt(b1));
  t.shot('53-zoom-one-of-two');

  // a program with no use for the event is told and carries on; the key itself does not reach it
  check('a uidemo window opens', await openDemo(t));
  const keys = count(t, /uidemo \d+: KEY_DOWN key=(12|13|11) /g);
  await t.tap('Equal', { ctrl: true });
  check('a non-terminal window gets the zoom event', await until(t, () => /uidemo \d+: ZOOM 1/.test(t.log), 5000));
  await t.tap('Minus', { ctrl: true });
  await t.tap('Digit0', { ctrl: true });
  check('and the reset', await until(t, () => /uidemo \d+: ZOOM 0/.test(t.log), 5000));
  check('the zoom keys themselves do not reach the program', count(t, /uidemo \d+: KEY_DOWN key=(12|13|11) /g) === keys);
  check('the program is still running', demos(t).size === 1);

  done('zoom');
};
