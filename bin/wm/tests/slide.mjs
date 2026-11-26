// SPDX-License-Identifier: GPL-2.0-or-later
// Workspace slide. Mid-way the old and the new workspace are both on screen; when it ends,
// only the new one. Nothing here depends on catching a 160 ms animation at the right moment:
// the test slows the slide to 1.5 s (uidemo key "a") and polls for each state.
import { check, done, same, hex, openDemos, boot, stats, until, DECK } from './lib.mjs';

const W = 1280, ROW = 380;
// How many of 80 samples along one row are window pixels (not wallpaper).
const windowSamples = (t) => { let n = 0; for (let x = 8; x < W; x += 16) if (!same(t.pixel(x, ROW), DECK.desktop, 16)) n++; return n; };
const isBar = (t, x) => same(t.pixel(x, 14), DECK.bar);
const half = (t, from, to) => { let n = 0; for (let x = from + 8; x < to; x += 16) if (!same(t.pixel(x, ROW), DECK.desktop, 16)) n++; return n; };
// Mid-slide in these tests the windows are on the left part of the screen and the empty workspace on the right.
const windowsLeft = (t) => half(t, 0, W / 2) > half(t, W / 2, W) && same(t.pixel(W - 24, ROW), DECK.desktop, 16);
const empty = (t) => windowSamples(t) === 0;
const full = (t) => windowSamples(t) >= 70;

export default async (t) => {
  await boot(t);
  check('two windows', await openDemos(t, 2));
  const before = await stats(t);
  check('workspace 1 is full of windows', full(t), String(windowSamples(t)));

  await t.tap('KeyA'); // slow slides
  await t.sleep(100);

  // up: workspace 1 -> 2 (2 is empty): the windows leave to the left, the desktop comes from the right
  await t.tap('Digit2', { alt: true });
  const midUp = await until(t, () => { const n = windowSamples(t); return n > 8 && n < 62; }, 3000);
  t.shot('20-slide-up-midway');
  check('up: mid-slide both workspaces are on screen', midUp, String(windowSamples(t)));
  check('up: windows are on the left, the empty workspace on the right', windowsLeft(t), `${half(t, 0, W / 2)} left, ${half(t, W / 2, W)} right`);
  check('up: the bar stays put', isBar(t, 400) && isBar(t, 900), hex(t.pixel(400, 14)));
  check('up: ends on the empty workspace', await until(t, () => empty(t), 5000) && isBar(t, 400), String(windowSamples(t)));

  // down: 2 -> 1: the windows come in from the left
  await t.tap('Digit1', { alt: true });
  const midDown = await until(t, () => { const n = windowSamples(t); return n > 8 && n < 62; }, 3000);
  t.shot('21-slide-down-midway');
  check('down: mid-slide both workspaces are on screen', midDown, String(windowSamples(t)));
  check('down: windows are on the left, the empty workspace on the right', windowsLeft(t), `${half(t, 0, W / 2)} left, ${half(t, W / 2, W)} right`);
  check('down: ends with the windows back', await until(t, () => full(t), 5000), String(windowSamples(t)));

  // turning back mid-slide carries on from where the picture is: no jump
  await t.tap('Digit2', { alt: true });
  await until(t, () => { const n = windowSamples(t); return n > 40 && n < 62; }, 3000); // early in the slide
  const n1 = windowSamples(t);
  await t.tap('Digit1', { alt: true });
  await t.sleep(60);
  const n2 = windowSamples(t);
  check('turning back mid-slide does not jump', Math.abs(n2 - n1) <= 16, `${n1} -> ${n2} of 80 samples`);
  check('turning back: ends with the windows back', await until(t, () => full(t), 5000), String(windowSamples(t)));

  // the slide keeps no pictures: after three slides the manager's memory is what it was before
  // the first (measured here, before the fullscreen part: a fullscreen window needs a bigger buffer)
  const after = await stats(t);
  check('sliding does not grow the manager', after.wm_pages === before.wm_pages, `${before.wm_pages} -> ${after.wm_pages} pages`);

  // a fullscreen window hides the bar on its workspace: the bar travels with the workspace that shows it
  await t.tap('KeyF', { alt: true });
  check('fullscreen hides the bar', await until(t, () => !isBar(t, 400) && !isBar(t, 900), 2000), hex(t.pixel(400, 14)));
  await t.tap('Digit2', { alt: true });
  const barComing = await until(t, () => { const n = windowSamples(t); return n > 20 && n < 60 && !isBar(t, 40) && [1100, 1140, 1180, 1220, 1250].some((x) => isBar(t, x)); }, 3000);
  t.shot('22-slide-from-fullscreen');
  check('leaving a fullscreen workspace: the bar slides in with the new one', barComing, hex(t.pixel(40, 14)) + ' ' + hex(t.pixel(1220, 14)));
  check('leaving a fullscreen workspace: ends with the bar across the screen', await until(t, () => empty(t) && isBar(t, 400) && isBar(t, 900), 5000));
  await t.tap('Digit1', { alt: true });
  check('back on the fullscreen workspace the bar is hidden', await until(t, () => full(t) && !isBar(t, 400) && !isBar(t, 900), 5000), hex(t.pixel(400, 14)));
  await t.tap('KeyF', { alt: true });
  check('leaving fullscreen brings the bar back', await until(t, () => isBar(t, 400), 2000));

  // animation 0: the switch is instant
  await t.tap('Digit0'); // uidemo key "0": no slide
  await t.sleep(100);
  const s0 = await stats(t);
  await t.tap('Digit2', { alt: true });
  check('animation 0: the new workspace is there at once', await until(t, () => empty(t), 400), String(windowSamples(t)));
  await t.tap('Digit1', { alt: true });
  await until(t, () => full(t), 2000);
  const s1 = await stats(t);
  check('animation 0: two switches cost only a few frames', s1.frames - s0.frames <= 6, `${s1.frames - s0.frames} frames`);

  // back to the default speed: a slide takes about 160 ms and then the manager is idle again
  await t.tap('KeyF');
  await t.sleep(100);
  await t.tap('Digit2', { alt: true });
  check('default speed: over within a second', await until(t, () => empty(t), 1000));
  await t.tap('Digit1', { alt: true });
  await until(t, () => full(t), 2000);

  done('slide');
};
