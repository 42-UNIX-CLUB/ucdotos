// SPDX-License-Identifier: GPL-2.0-or-later
// tools/browser.mjs script: how long does the page spend presenting a frame? Needs the page
// to be opened with ?stats (boot/page.mjs then counts frames, milliseconds and pixels in
// window.ucosStats). The scenario comes from the environment variable SCENARIO:
//   full    every frame changes the whole screen (open the page with cmdline=init=/bin/fbdemo;
//           each pointer move makes fbdemo redraw and write the whole framebuffer)
//   type    one terminal cell changes per key (the desktop; typing at the shell prompt)
//   fetch   the desktop with `fetch -a` running (a 30 frames per second animation)
// Prints one line: RESULT {"frames", "avgMs", "maxMs", "avgPixels"}. SHOT=name saves a screenshot.
export default async (t) => {
  const scenario = process.env.SCENARIO || 'type';
  const reset = () => t.eval('Object.assign(window.ucosStats, { frames: 0, ms: 0, maxMs: 0, pixels: 0 })');
  const read = async () => t.eval('window.ucosStats');
  const ready = async (ms) => { const t0 = Date.now(); while (!(await t.eval('!!window.ucosStats'))) { if (Date.now() - t0 > 30000) throw new Error('no ucosStats: open the page with ?stats'); await t.sleep(100); } await t.sleep(ms); };
  if (scenario === 'full') {
    await ready(1500);
    await reset();
    for (let i = 0; i < 60; i++) { await t.move(100 + i * 7, 100 + (i % 20) * 9); await t.sleep(25); }
    await t.sleep(200);
  } else if (scenario === 'type') {
    await ready(7000);
    await t.click(400, 400);
    await t.sleep(300);
    await reset();
    await t.type('echo the quick brown fox jumps over the lazy dog');
    await t.sleep(300);
  } else if (scenario === 'fetch') {
    await ready(7000);
    await t.click(400, 400);
    await t.type('fetch -a\n');
    await t.sleep(1500);
    await reset();
    await t.sleep(3000);
  }
  const s = await read();
  if (process.env.SHOT) await t.shot(process.env.SHOT);
  if (scenario === 'fetch') await t.tap('KeyQ');
  process.stdout.write('RESULT ' + JSON.stringify({ frames: s.frames, avgMs: s.ms / Math.max(1, s.frames), maxMs: s.maxMs, avgPixels: s.pixels / Math.max(1, s.frames) }) + '\n');
};
