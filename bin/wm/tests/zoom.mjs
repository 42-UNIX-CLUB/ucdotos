// SPDX-License-Identifier: GPL-2.0-or-later
// Zoom: Ctrl+= and Ctrl+- are taken by the manager and sent to the focused window as UI_EV_ZOOM.
// The welcome terminal then draws bigger or smaller text, so its grid has fewer or more cells.
// Needs term in the image: make B=build-wm PROGS="wm bar launcher uidemo term sh".
import { check, done, until, count } from './lib.mjs';

// The focused terminal prints "[screen-begin] COLSxROWS ..." on Ctrl+Alt+Shift+F12.
async function grid(t) {
  const n = count(t, /\[screen-begin\]/g);
  await t.tap('F12', { ctrl: true, alt: true, shift: true });
  await until(t, () => count(t, /\[screen-begin\]/g) > n, 3000);
  const all = [...t.log.matchAll(/\[screen-begin\] (\d+)x(\d+)/g)];
  if (all.length <= n) return null;
  const m = all[all.length - 1];
  return { cols: Number(m[1]), rows: Number(m[2]) };
}
const fmt = (g) => (g ? `${g.cols}x${g.rows}` : 'none');

export default async (t) => {
  check('welcome terminal up', await until(t, () => /wm: \d+x\d+/.test(t.log), 5000));
  await t.sleep(1500);
  const g0 = await grid(t);
  check('terminal answers the screen dump', !!g0, fmt(g0));
  if (!g0) { done('zoom'); return; }
  t.shot('30-zoom-normal');

  await t.tap('Equal', { ctrl: true });
  await t.tap('Equal', { ctrl: true });
  await t.sleep(300);
  const g1 = await grid(t);
  t.shot('31-zoom-in');
  check('Ctrl+= twice: fewer columns and rows', g1 && g1.cols < g0.cols && g1.rows < g0.rows, fmt(g0) + ' -> ' + fmt(g1));

  await t.tap('Equal', { ctrl: true, shift: true });
  await t.sleep(300);
  const g2 = await grid(t);
  check('Ctrl++ (Shift held) zooms in too', g2 && g2.cols < g1.cols, fmt(g1) + ' -> ' + fmt(g2));

  await t.tap('Digit0', { ctrl: true });
  await t.sleep(300);
  const g3 = await grid(t);
  check('Ctrl+0: back to the normal size', g3 && g3.cols === g0.cols && g3.rows === g0.rows, fmt(g3));

  await t.tap('Minus', { ctrl: true });
  await t.tap('Minus', { ctrl: true });
  await t.sleep(300);
  const g4 = await grid(t);
  t.shot('32-zoom-out');
  check('Ctrl+- twice: more columns and rows', g4 && g4.cols > g0.cols && g4.rows > g0.rows, fmt(g0) + ' -> ' + fmt(g4));

  for (let i = 0; i < 20; i++) await t.tap('Minus', { ctrl: true });
  await t.sleep(300);
  const g5 = await grid(t);
  await t.tap('Equal', { ctrl: true });
  await t.sleep(300);
  const g6 = await grid(t);
  check('at the smallest size one Ctrl+= zooms in at once', g5 && g6 && g6.cols < g5.cols, fmt(g5) + ' -> ' + fmt(g6));

  done('zoom');
};
