// SPDX-License-Identifier: GPL-2.0-or-later
// The desktop in a real browser: boot, type, keep the system responsive under a
// process that never blocks, Ctrl-C, mouse focus.
//   make B=build-all dist && node tools/browser.mjs --build=build-all --script=tools/tests/browser-desktop.mjs
export default async (t) => {
  await t.sleep(4000);
  await t.shot('b01-boot');
  const res = await t.eval(`(() => { let min = 1e9, last = performance.now(); const end = last + 30;
    for (;;) { const n = performance.now(); if (n > last) { min = Math.min(min, n - last); last = n; } if (n > end) break; } return min; })()`);
  console.error(`[test] performance.now() resolution in this browser: ${(res * 1000).toFixed(1)} us`);
  await t.type('echo hello from chromium\n');
  await t.sleep(500);
  await t.type('yes > /dev/null\n'); // never blocks: only time slicing keeps the system alive
  await t.sleep(1500);
  await t.tap('Enter', { alt: true }); // a second terminal must still open and work
  await t.sleep(2500);
  await t.type('ps\n');
  await t.sleep(800);
  await t.shot('b02-responsive-under-load');
  await t.tap('KeyK', { alt: true }); // back to the first terminal
  await t.sleep(300);
  await t.tap('KeyC', { ctrl: true }); // Ctrl-C kills yes
  await t.sleep(600);
  await t.type('echo back at the prompt\n');
  await t.sleep(600);
  await t.shot('b03-after-ctrl-c');
  await t.click(960, 400); // focus the right-hand window with the mouse
  await t.sleep(300);
  await t.type('sysbench\n');
  await t.sleep(9000);
  await t.shot('b04-sysbench-in-browser');
};
