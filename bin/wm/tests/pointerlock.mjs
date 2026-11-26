// SPDX-License-Identifier: GPL-2.0-or-later
// Mouse capture for fullscreen games: a window that asked (ui_pointer_lock, key l in
// uidemo) gets the mouse captured only while it is fullscreen, focused and on the current
// workspace. Captured: the page is told to lock the pointer, the cursor is not drawn, and
// movement arrives as UI_EV_MOUSE_REL. Every way out releases it.
import { check, done, until, count, openDemos, boot } from './lib.mjs';

const CURSOR = [0x10, 0x10, 0x10];                  /* the arrow's outline at its tip */
const isCursor = (p) => p[0] === CURSOR[0] && p[1] === CURSOR[1] && p[2] === CURSOR[2];

export default async (t) => {
  await boot(t);
  check('one window', await openDemos(t, 1));
  await t.move(400, 300);
  await t.sleep(200);
  check('the cursor is drawn at the pointer', isCursor(t.pixel(400, 300)));

  await t.tap('KeyL');                              /* uidemo asks for the capture */
  await until(t, () => /pointer-lock on/.test(t.log));
  await t.sleep(200);
  check('asking is not enough: not fullscreen, not captured', !t.pointerLocked());

  await t.tap('KeyF', { alt: true });               /* fullscreen */
  check('fullscreen: the mouse is captured', await until(t, () => t.pointerLocked(), 2000));
  await t.sleep(200);
  check('captured: the cursor is not drawn', !isCursor(t.pixel(400, 300)));
  const rel = count(t, /MOUSE_REL/g);
  await t.rel(15, -7);
  check('movement arrives as MOUSE_REL with its distance', await until(t, () => /MOUSE_REL x=15 y=-7/.test(t.log), 2000));
  await t.move(10, 10);                             /* a position from the page is ignored while captured */
  await t.sleep(200);
  check('a position does not show or move the cursor', !isCursor(t.pixel(10, 10)) && !isCursor(t.pixel(400, 300)));

  await t.tap('KeyF', { alt: true });               /* fullscreen off */
  check('fullscreen off: released', await until(t, () => !t.pointerLocked(), 2000));
  await t.sleep(200);
  check('released: the cursor is back where it was', isCursor(t.pixel(400, 300)));
  const before = count(t, /MOUSE_REL/g);
  await t.rel(5, 5);
  await t.sleep(300);
  check('released: no MOUSE_REL', count(t, /MOUSE_REL/g) === before && before === rel + 1);

  await t.tap('KeyF', { alt: true });
  check('fullscreen again: captured again', await until(t, () => t.pointerLocked(), 2000));
  await t.tap('Digit2', { alt: true });             /* another workspace */
  check('another workspace: released', await until(t, () => !t.pointerLocked(), 2000));
  await t.tap('Digit1', { alt: true });
  check('back: captured again', await until(t, () => t.pointerLocked(), 2000));

  await t.tap('KeyL');                              /* the window no longer asks */
  check('the window stops asking: released', await until(t, () => !t.pointerLocked(), 2000));
  await t.tap('KeyL');
  check('asks again while fullscreen: captured', await until(t, () => t.pointerLocked(), 2000));

  await t.tap('KeyQ');                              /* the window goes away while it has the mouse */
  check('the window quits: released', await until(t, () => !t.pointerLocked(), 2000));
  done('pointerlock');
};
