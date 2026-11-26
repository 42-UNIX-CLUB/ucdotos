// SPDX-License-Identifier: GPL-2.0-or-later
// Workspace slide: mid-way the old and the new workspace are both on screen; when it ends, only the new one.
import { check, done, same, hex, openDemos, boot, DECK, G } from './lib.mjs';

export default async (t) => {
  await boot(t);
  check('two windows', await openDemos(t, 2));
  const bar = () => same(t.pixel(400, 14), DECK.bar);

  // up: workspace 1 -> 2 (2 is empty): the windows leave to the left, the desktop comes from the right
  await t.tap('Digit2', { alt: true });
  await t.sleep(50);
  t.shot('20-slide-up-midway');
  check('up: windows moving out to the left', !same(t.pixel(100, 200), DECK.desktop) && same(t.pixel(900, 200), DECK.desktop), hex(t.pixel(100, 200)) + ' ' + hex(t.pixel(900, 200)));
  check('up: mid-slide the bar stays put', bar(), hex(t.pixel(400, 14)));
  await t.sleep(400);
  check('up: ended on the empty workspace', same(t.pixel(301, 400), DECK.desktop) && bar(), hex(t.pixel(301, 400)));

  // down: 2 -> 1: the windows come in from the left
  await t.tap('Digit1', { alt: true });
  await t.sleep(50);
  t.shot('21-slide-down-midway');
  check('down: windows coming in from the left', !same(t.pixel(100, 200), DECK.desktop) && same(t.pixel(1100, 200), DECK.desktop), hex(t.pixel(100, 200)) + ' ' + hex(t.pixel(1100, 200)));
  check('down: mid-slide the bar stays put', bar(), hex(t.pixel(400, 14)));
  await t.sleep(400);
  check('down: ended with the windows back', !same(t.pixel(G.gap + 20, 380), DECK.desktop), hex(t.pixel(G.gap + 20, 380)));
  done('slide');
};
