// SPDX-License-Identifier: GPL-2.0-or-later
// Clients that crash mid-message, send garbage or absurd headers, a graceful close,
// a forced close, then screen resizes to 1920x1080 and 800x600.
import { check, done, same, hex, until, demos, count, openDemos, boot, DECK, G } from './lib.mjs';

export default async (t) => {
  await boot(t);
  check('three windows', await openDemos(t, 3));

  // x: the focused uidemo sends half a PRESENT and traps; the kernel kills it
  let n = demos(t).size;
  await t.tap('KeyX');
  check('crash: client killed', await until(t, () => /pid \d+ \(uidemo\) killed by signal 11/.test(t.log)), '');
  await until(t, () => demos(t).size === n - 1);
  await t.sleep(300);
  t.shot('20-after-crash');
  check('crash: its window is gone, two left', demos(t).size === 2 && same(t.pixel(G.stackX, 380), DECK.focus), hex(t.pixel(G.stackX, 380)));

  // g: garbage bytes -> the manager drops the client, which then exits
  n = demos(t).size;
  await t.tap('KeyG');
  check('garbage: manager reports and closes the client', await until(t, () => /sent a bad message/.test(t.log)));
  check('garbage: client gone', await until(t, () => demos(t).size === n - 1), String(demos(t).size));

  // o: a PRESENT header claiming a 100000x100000 buffer -> dropped without allocating it
  check('open another', await openDemos(t, 1));
  n = demos(t).size;
  await t.tap('KeyO');
  check('absurd PRESENT: rejected', await until(t, () => count(t, /wm: client \d+ sent a bad/g) >= 2));
  check('absurd PRESENT: client gone', await until(t, () => demos(t).size === n - 1), String(demos(t).size));

  // Mod+Shift+q: the client gets UI_EV_CLOSE and leaves by itself
  check('two more', await openDemos(t, 2));
  n = demos(t).size;
  await t.tap('KeyQ', { alt: true, shift: true });
  check('close: graceful exit', await until(t, () => demos(t).size === n - 1), String(demos(t).size));
  // the manager is still healthy: open, focus, draw
  check('manager still serves new clients', await openDemos(t, 1));
  t.shot('21-after-abuse');
  check('manager never exited', !/wm: exit/.test(t.log) && !/\(wm\) killed/.test(t.log));

  // resize the screen
  t.resize(1920, 1080);
  await t.sleep(800);
  t.shot('22-1920x1080');
  check('1920x1080: bar spans the width', same(t.pixel(1800, 14), DECK.bar), hex(t.pixel(1800, 14)));
  check('1920x1080: windows fill the height', !same(t.pixel(300, 1000), DECK.desktop), hex(t.pixel(300, 1000)));
  check('1920x1080: clients resized', /RESIZE 1\d\d\dx/.test(t.log) || /RESIZE \d+x9\d\d/.test(t.log));
  t.resize(800, 600);
  await t.sleep(800);
  t.shot('23-800x600');
  check('800x600: bar at the right edge', same(t.pixel(780, 14), DECK.bar), hex(t.pixel(780, 14)));
  check('800x600: gap at the bottom right', same(t.pixel(795, 595), DECK.desktop), hex(t.pixel(795, 595)));
  t.resize(1280, 720);
  await t.sleep(500);
  check('back to 1280x720', same(t.pixel(G.stackX - 5, 380), DECK.desktop), hex(t.pixel(G.stackX - 5, 380)));
  done('robust');
};
