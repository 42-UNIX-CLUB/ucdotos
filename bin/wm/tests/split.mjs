// SPDX-License-Identifier: GPL-2.0-or-later
// The split layout and open left|right|up|down. Windows are terminals, opened with the
// default $mod CTRL + h j k l keys. Every split leaves a 12 px gap of wallpaper exactly where
// the tree puts it, so each check samples the wallpaper colour in the gap a split must make.
// At 1280x720 with the default config the tile area is x 12..1268, y 52..708.
// Needs term in the image: make B=build-NAME PROGS="wm bar launcher uidemo term sh ps awk".
import { check, done, same, hex, until, boot, DECK } from './lib.mjs';
import { screen, waitPrompt } from '../../term/tests/os/lib.mjs';

const desk = (t, x, y) => same(t.pixel(x, y), DECK.desktop);
async function newTerm(t, key) {
  const n = (t.log.match(/\[screen-begin\]/g) || []).length;
  await t.tap(key, { alt: true, ctrl: true });
  await t.sleep(2200);
  await waitPrompt(t, 5000);
  return n;
}
// The window manager's memory, in KiB, from ps in the focused terminal.
async function wmKiB(t) {
  await t.type("clear; ps | awk '$NF == \"wm\" { print \"WMK\", $4 }'\n"); // short: the terminal may be narrow
  await t.sleep(500);
  const s = await screen(t);
  const m = s.rows.map((r) => r.match(/^WMK (\d+)K/)).filter(Boolean).pop();
  return m ? +m[1] : -1;
}

export default async (t) => {
  await boot(t);
  await t.tap('Enter', { alt: true }); // term A, alone
  await t.sleep(2200);
  await waitPrompt(t, 5000);
  await t.tap('KeyS', { alt: true }); // this workspace: split layout
  await t.sleep(300);

  // A | B : right of A
  await newTerm(t, 'KeyL');
  t.shot('30-split-right');
  check('open right: a vertical gap in the middle', desk(t, 640, 400) && !desk(t, 300, 400) && !desk(t, 900, 400), hex(t.pixel(640, 400)));

  // B over C : below B (B is focused)
  await newTerm(t, 'KeyJ');
  check('open down: the right half splits, with a gap at y 374..386', desk(t, 900, 380) && !desk(t, 300, 380), hex(t.pixel(900, 380)));

  // D | C : left of C (C is focused), in the bottom-right quarter
  await newTerm(t, 'KeyH');
  check('open left: the bottom-right quarter splits at x 951..963', desk(t, 957, 500) && !desk(t, 800, 500) && !desk(t, 1100, 500), hex(t.pixel(957, 500)));

  // E over D : above D (D is focused)
  await newTerm(t, 'KeyK');
  t.shot('31-split-four-ways');
  check('open up: D splits at y 541..553', desk(t, 800, 547) && !desk(t, 800, 470) && !desk(t, 800, 620), hex(t.pixel(800, 547)));

  // closing E gives its place back to D
  await t.tap('KeyQ', { alt: true, shift: true });
  await t.sleep(800);
  check('close: D takes E\'s place again', !desk(t, 800, 547), hex(t.pixel(800, 547)));

  // master and stack keeps the same windows; the split tree comes back as it was
  await t.tap('KeyS', { alt: true });
  await t.sleep(500);
  t.shot('32-back-to-master');
  check('layout toggle: master and stack (no gap at the split\'s middle row)', !desk(t, 900, 380) || !desk(t, 957, 500));
  await t.tap('KeyS', { alt: true });
  await t.sleep(500);
  check('layout toggle again: the split tree as before', desk(t, 640, 400) && desk(t, 900, 380) && desk(t, 957, 500), hex(t.pixel(640, 400)));

  // memory: open and close a terminal beside the focused one, again and again
  const before = await wmKiB(t);
  for (let i = 0; i < 8; i++) {
    await newTerm(t, i % 2 ? 'KeyL' : 'KeyJ');
    await t.tap('KeyQ', { alt: true, shift: true });
    await t.sleep(700);
  }
  await waitPrompt(t, 5000);
  const after = await wmKiB(t);
  check('memory: the window manager does not grow over 8 opens and closes', before > 0 && after === before, `${before} -> ${after} KiB`);
  done('split');
};
