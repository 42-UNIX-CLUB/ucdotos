// SPDX-License-Identifier: GPL-2.0-or-later
// Window mode: the window manager starts doom (see mkimage.sh, WM=1).
// Title in a window, start a game, resize the screen, close the window.
export default async (t) => {
  await t.sleep(4000);
  t.shot('win-title');
  await t.tap('Escape'); await t.sleep(400);
  await t.tap('Enter'); await t.sleep(400);
  await t.tap('Enter'); await t.sleep(400);
  await t.tap('Enter'); await t.sleep(2500);
  t.key('ArrowUp', true); await t.sleep(800); t.key('ArrowUp', false);
  await t.sleep(200);
  t.shot('win-ingame');
  t.resize(1024, 768); await t.sleep(1500);
  t.shot('win-resized');
  t.resize(1280, 720); await t.sleep(1000);
  await t.tap('KeyQ', { alt: true, shift: true }); await t.sleep(1500); // the wm's default "kill" binding
  t.shot('win-closed');
  console.error('[test] doom exited: ' + /doom stats for the whole run/.test(t.log));
};
