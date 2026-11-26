// SPDX-License-Identifier: GPL-2.0-or-later
// Forced close of a client that ignores UI_EV_CLOSE, partial presents, config reload,
// floating toggle, and Mod+Shift+e (exit) last.
import { check, done, same, hex, until, demos, count, openDemos, boot, DECK, G } from './lib.mjs';

export default async (t) => {
  await boot(t);
  check('two windows', await openDemos(t, 2));

  // a client that ignores the close request: the second Mod+Shift+q kills it
  await t.tap('KeyI');
  await t.sleep(100);
  const n = demos(t).size;
  await t.tap('KeyQ', { alt: true, shift: true });
  check('stubborn client ignores the first close', await until(t, () => /ignoring CLOSE/.test(t.log), 2000) && demos(t).size === n);
  await t.tap('KeyQ', { alt: true, shift: true });
  check('second close forces it', await until(t, () => /ignored the close request; forcing it/.test(t.log), 3000));
  await t.sleep(300);
  check('its window is gone', same(t.pixel(G.gap, 380), DECK.focus) && same(t.pixel(G.stackX + 300, 380), DECK.desktop) === false, hex(t.pixel(G.gap, 380)));

  // partial present: c paints a 40x40 square and sends only that rectangle
  const y = Math.floor((720 - G.top - G.gap - 4) / 2) + G.top + 2; // the square's top on screen
  const before = t.pixel(G.gap + 2 + 40, y + 20);
  await t.tap('KeyC');
  await t.sleep(200);
  check('partial present reaches the screen', !same(t.pixel(G.gap + 2 + 40, y + 20), before), hex(t.pixel(G.gap + 2 + 40, y + 20)));
  t.shot('40-partial-present');

  // floating toggle: the window leaves the tiling and is centred with a yellow border
  await t.tap('Space', { alt: true, shift: true });
  await t.sleep(300);
  t.shot('41-floating');
  check('float: desktop visible around it', same(t.pixel(G.gap + 2, G.top + 2), DECK.desktop), hex(t.pixel(G.gap + 2, G.top + 2)));
  await t.tap('Space', { alt: true, shift: true });
  await t.sleep(300);
  check('float off: tiled again', same(t.pixel(G.gap, 380), DECK.focus), hex(t.pixel(G.gap, 380)));

  await t.tap('KeyR', { alt: true, shift: true });
  check('Mod+Shift+r reloads the config', await until(t, () => /configuration reloaded/.test(t.log)));
  await t.sleep(300);
  check('after reload the layout is intact', same(t.pixel(G.gap, 380), DECK.focus), hex(t.pixel(G.gap, 380)));

  await t.tap('KeyE', { alt: true, shift: true });
  check('Mod+Shift+e exits the manager', await until(t, () => /wm: exit/.test(t.log)));
  done('misc');
};
