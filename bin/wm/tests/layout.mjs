// SPDX-License-Identifier: GPL-2.0-or-later
// Boot, one / three / five tiled windows, focus by keyboard and by click, swap, master resize.
import { check, done, same, hex, until, demos, openDemo, openDemos, boot, DECK, G } from './lib.mjs';

export default async (t) => {
  await boot(t);
  t.shot('01-boot');
  check('desktop colour', same(t.pixel(301, 400), DECK.desktop), hex(t.pixel(301, 400)));
  check('bar is drawn', same(t.pixel(400, 14), DECK.bar), hex(t.pixel(400, 14)));
  check('current workspace block is orange', same(t.pixel(25, 30), DECK.focus), hex(t.pixel(25, 30)));

  check('first window opens', await openDemo(t));
  t.shot('02-one-window');
  check('one window: focused border at the left edge', same(t.pixel(G.gap, 380), DECK.focus), hex(t.pixel(G.gap, 380)));
  check('one window: gap left of it shows the desktop', same(t.pixel(5, 380), DECK.desktop));

  check('three windows open', await openDemos(t, 2));
  await t.sleep(200);
  t.shot('03-three-windows');
  // master on the left, two stacked on the right; the newest (bottom right) has the focus
  check('3: master border is thin grey', same(t.pixel(G.gap, 380), DECK.normal), hex(t.pixel(G.gap, 380)));
  check('3: gap between master and stack', same(t.pixel(G.gap + G.masterW + 5, 380), DECK.desktop));
  check('3: focused window is bottom right', same(t.pixel(G.stackX, 600), DECK.focus), hex(t.pixel(G.stackX, 600)));
  check('3: top right window not focused', same(t.pixel(G.stackX, 150), DECK.normal), hex(t.pixel(G.stackX, 150)));

  // keyboard focus: Mod+j wraps to the master, Mod+k comes back
  await t.tap('KeyJ', { alt: true });
  await t.sleep(120);
  check('Mod+j: master focused', same(t.pixel(G.gap, 380), DECK.focus), hex(t.pixel(G.gap, 380)));
  check('Mod+j: old one unfocused', same(t.pixel(G.stackX, 600), DECK.normal));
  await t.tap('KeyK', { alt: true });
  await t.sleep(120);
  check('Mod+k: back to the bottom right', same(t.pixel(G.stackX, 600), DECK.focus));
  await t.tap('ArrowUp', { alt: true });
  await t.sleep(120);
  check('Mod+Up: top right focused', same(t.pixel(G.stackX, 150), DECK.focus));

  // focus follows click
  await t.click(300, 400);
  await t.sleep(150);
  check('click: master focused', same(t.pixel(G.gap, 380), DECK.focus), hex(t.pixel(G.gap, 380)));
  check('click: top right unfocused', same(t.pixel(G.stackX, 150), DECK.normal));
  t.shot('04-focus-click');

  // master grow / shrink: the stack's left edge moves by 5 % of the width
  await t.tap('KeyL', { alt: true });
  await t.sleep(150);
  check('Mod+l: master grew', same(t.pixel(G.stackX, 380), DECK.desktop) || !same(t.pixel(G.stackX, 380), DECK.normal), hex(t.pixel(G.stackX, 380)));
  await t.tap('KeyH', { alt: true });
  await t.sleep(150);
  check('Mod+h: master back', same(t.pixel(G.stackX, 150), DECK.normal), hex(t.pixel(G.stackX, 150)));

  // swap: the focused master moves into the stack, so the master area now has an unfocused window
  await t.tap('KeyJ', { alt: true, shift: true });
  await t.sleep(150);
  check('Mod+Shift+j: focused window left the master', same(t.pixel(G.gap, 380), DECK.normal), hex(t.pixel(G.gap, 380)));
  check('Mod+Shift+j: it is now top right', same(t.pixel(G.stackX, 150), DECK.focus), hex(t.pixel(G.stackX, 150)));

  check('five windows open', await openDemos(t, 2));
  await t.sleep(300);
  t.shot('05-five-windows');
  check('5: five clients', demos(t).size === 5, String(demos(t).size));
  check('5: master still unfocused grey', same(t.pixel(G.gap, 380), DECK.normal));
  check('5: newest at the bottom right is focused', same(t.pixel(G.stackX, 690), DECK.focus), hex(t.pixel(G.stackX, 690)));
  check('wm still running', !/wm: exit/.test(t.log));
  done('layout');
};
