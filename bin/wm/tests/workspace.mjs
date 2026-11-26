// SPDX-License-Identifier: GPL-2.0-or-later
// Workspace switch and move, fullscreen, launcher (filter, cancel, launch), theme switch and back.
import { check, done, same, hex, until, demos, openDemos, boot, DECK, BRAND, G } from './lib.mjs';

export default async (t) => {
  await boot(t);
  check('two windows', await openDemos(t, 2));

  // workspace switch: 2 is empty, the bar marks it current and 1 as occupied
  await t.tap('Digit2', { alt: true });
  await t.sleep(200);
  t.shot('10-workspace-2');
  check('ws2: desktop where the master was', same(t.pixel(301, 400), DECK.desktop), hex(t.pixel(301, 400)));
  check('ws2: block on 2 in the bar', same(t.pixel(51, 30), DECK.focus) && !same(t.pixel(25, 30), DECK.focus), hex(t.pixel(51, 30)));
  await t.tap('Digit1', { alt: true });
  await t.sleep(200);
  check('ws1 again: windows back', same(t.pixel(G.stackX, 380), DECK.focus), hex(t.pixel(G.stackX, 380)));

  // move the focused (right) window to workspace 3: the master takes the whole width
  await t.tap('Digit3', { alt: true, shift: true });
  await t.sleep(200);
  t.shot('11-moved-to-3');
  check('move: master alone and focused', same(t.pixel(G.gap, 380), DECK.focus) && !same(t.pixel(G.stackX, 380), DECK.desktop), hex(t.pixel(G.gap, 380)));
  await t.tap('Digit3', { alt: true });
  await t.sleep(200);
  check('ws3 holds the moved window', same(t.pixel(G.gap, 380), DECK.focus), hex(t.pixel(G.gap, 380)));
  // clicking a number in the bar switches workspace (bar -> ui_wm_command)
  await t.click(25, 26);
  await t.sleep(300);
  check('bar click: back on 1', same(t.pixel(23, 18), DECK.focus), hex(t.pixel(23, 18)));

  // fullscreen covers the bar and the gaps
  await t.tap('KeyF', { alt: true });
  await t.sleep(300);
  t.shot('12-fullscreen');
  check('fullscreen: no bar', !same(t.pixel(400, 14), DECK.bar), hex(t.pixel(400, 14)));
  check('fullscreen: no gap at the corner', !same(t.pixel(2, 700), DECK.desktop), hex(t.pixel(2, 700)));
  await t.tap('KeyF', { alt: true });
  await t.sleep(300);
  check('fullscreen off: bar back', same(t.pixel(400, 14), DECK.bar));

  // launcher: open, filter, cancel with Escape
  await t.tap('KeyD', { alt: true });
  await t.sleep(500);
  check('launcher: yellow border', same(t.pixel(339, 300), DECK.float) || same(t.pixel(340, 300), DECK.float), hex(t.pixel(339, 300)) + ' ' + hex(t.pixel(340, 300)));
  await t.type('spe');
  await t.sleep(200);
  t.shot('13-launcher-filter');
  await t.tap('Escape');
  await t.sleep(300);
  check('launcher: Escape closes it', !same(t.pixel(339, 300), DECK.float) && !same(t.pixel(340, 300), DECK.float));
  check('launcher: focus returns to the window', same(t.pixel(G.gap, 380), DECK.focus), hex(t.pixel(G.gap, 380)));
  // launch through it: Down to the second match of "ui" and Enter
  const n = demos(t).size;
  await t.tap('KeyD', { alt: true });
  await t.sleep(500);
  await t.type('ui demo');
  await t.sleep(150);
  t.shot('14-launcher-select');
  await t.tap('Enter');
  check('launcher: Enter starts the entry', await until(t, () => demos(t).size === n + 1), `${demos(t).size}`);
  await t.sleep(300);

  // theme: deck -> brand -> deck, every client redraws (they get a RESIZE to the same size)
  const resizes = (t.log.match(/RESIZE/g) || []).length;
  await t.tap('KeyT', { alt: true });
  await until(t, () => /wm: theme brand/.test(t.log));
  await t.sleep(600);
  t.shot('15-theme-brand');
  check('brand: desktop is dark green', same(t.pixel(301, 712), BRAND.desktop, 3) || same(t.pixel(2, 2), BRAND.desktop, 3), hex(t.pixel(2, 2)));
  check('brand: bar is green', same(t.pixel(400, 14), BRAND.bar), hex(t.pixel(400, 14)));
  check('brand: focus border orange FE7700', same(t.pixel(G.stackX, 600), BRAND.focus), hex(t.pixel(G.stackX, 600)));
  check('brand: clients were told to redraw', (t.log.match(/RESIZE/g) || []).length >= resizes + 3);
  await t.tap('KeyT', { alt: true });
  await until(t, () => /wm: theme deck/.test(t.log));
  await t.sleep(600);
  check('deck again: desktop colour back', same(t.pixel(2, 2), DECK.desktop), hex(t.pixel(2, 2)));
  check('wm still running', !/wm: exit/.test(t.log));
  done('workspace');
};
