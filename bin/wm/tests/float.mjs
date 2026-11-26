// SPDX-License-Identifier: GPL-2.0-or-later
// Floating with Mod+y, then moving the floating window with Mod + left-button drag.
// The window that stays tiled must grow to fill the space the floating one left.
import * as K from '../../../boot/keys.mjs';
import { check, done, same, hex, until, openDemos, boot, demos, DECK, G } from './lib.mjs';

// The last "uidemo PID: RESIZE WxH" line for one pid.
function lastSize(t, pid) {
  const all = [...t.log.matchAll(new RegExp(`uidemo ${pid}: RESIZE (\\d+)x(\\d+)`, 'g'))];
  return all.length ? { w: Number(all[all.length - 1][1]), h: Number(all[all.length - 1][2]) } : null;
}
const fmt = (s) => (s ? `${s.w}x${s.h}` : 'none');
const btn = (t, down) => { t.sys.input(K.EV_KEY, K.BTN_LEFT, down ? 1 : 0); t.sys.input(K.EV_SYN, 0, 0); };
// The floating border is 2 px wide; accept either of two neighbouring columns.
const floatEdge = (t, x, y) => same(t.pixel(x, y), DECK.float) || same(t.pixel(x + 1, y), DECK.float);

export default async (t) => {
  await boot(t);
  check('two windows', await openDemos(t, 2));
  const [first, second] = [...demos(t)];
  await t.sleep(300);
  const tiled = lastSize(t, first);

  // Mod+y floats the focused (second) window; the first one takes the whole area
  await t.tap('KeyY', { alt: true });
  await until(t, () => { const s = lastSize(t, first); return s && s.w > tiled.w; }, 2000);
  await t.sleep(300);
  t.shot('50-float-alt-y');
  const full = lastSize(t, first), fl = lastSize(t, second);
  check('Mod+y: the tiled window grows to the full width', full && full.w > tiled.w && full.w >= 1280 - 2 * G.gap - 8, fmt(tiled) + ' -> ' + fmt(full));
  check('Mod+y: the floating window is 60 % of the screen', fl && fl.w === 768 && fl.h === 432, fmt(fl));
  // 60 % centred in the tile area: outer 772 x 436 at (254, 125)
  check('Mod+y: floating border where expected', floatEdge(t, 254, 300), hex(t.pixel(254, 300)));

  // Mod + drag: grab inside the window, move by (+200, +100), release
  const downs = (re) => [...t.log.matchAll(re)].length;
  const clicks = downs(new RegExp(`uidemo ${second}: MOUSE_DOWN`, 'g'));
  t.move(500, 300);
  t.key('AltLeft', true);
  btn(t, true);
  await t.sleep(30);
  for (let i = 1; i <= 10; i++) { t.move(500 + 20 * i, 300 + 10 * i); await t.sleep(16); }
  t.shot('51-float-dragging');
  btn(t, false);
  t.key('AltLeft', false);
  await t.sleep(300);
  t.shot('52-float-dragged');
  check('drag: the left border moved 200 px right', floatEdge(t, 454, 400) && !floatEdge(t, 254, 400), hex(t.pixel(254, 400)) + ' ' + hex(t.pixel(454, 400)));
  check('drag: the top border moved 100 px down', floatEdge(t, 600, 225) || same(t.pixel(600, 226), DECK.float), hex(t.pixel(600, 225)));
  check('drag: the window got no click', downs(new RegExp(`uidemo ${second}: MOUSE_DOWN`, 'g')) === clicks);
  check('drag: the size did not change', fmt(lastSize(t, second)) === fmt(fl), fmt(lastSize(t, second)));

  // past the screen edge it stops at the edge
  t.move(700, 400);
  t.key('AltLeft', true);
  btn(t, true);
  await t.sleep(30);
  t.move(1279, 719);
  await t.sleep(50);
  btn(t, false);
  t.key('AltLeft', false);
  await t.sleep(300);
  check('drag: stops at the bottom-right corner', floatEdge(t, 1278, 600) || same(t.pixel(1279, 600), DECK.float), hex(t.pixel(1279, 600)));

  // without Mod a click goes to the window as usual
  await t.click(1000, 600);
  await t.sleep(200);
  check('plain click still reaches the window', downs(new RegExp(`uidemo ${second}: MOUSE_DOWN`, 'g')) === clicks + 1);

  // Mod+y again: back in the tiling, both windows share the area
  await t.tap('KeyY', { alt: true });
  await t.sleep(400);
  t.shot('53-float-off');
  check('Mod+y again: tiled once more', fmt(lastSize(t, first)) === fmt(tiled), fmt(lastSize(t, first)) + ' vs ' + fmt(tiled));
  done('float');
};
