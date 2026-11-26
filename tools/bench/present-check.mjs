// SPDX-License-Identifier: GPL-2.0-or-later
// tools/browser.mjs script: checks that what the canvas shows is exactly the kernel's
// framebuffer (red, green and blue of every pixel), after typing, while fetch -a animates,
// and after the window is resized twice. The page copies only the damaged rectangle each
// frame, so any missed damage shows up here as differing pixels.
// Prints RESULT {"step": differing pixels, ...} and saves a screenshot per step.
export default async (t) => {
  const diff = () => t.eval(`(() => {
    const c = document.getElementById('screen'), f = ucos.frame();
    if (f.width !== c.width || f.height !== c.height) return 'size ' + c.width + 'x' + c.height + ' vs ' + f.width + 'x' + f.height;
    const a = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
    const b = new Uint8Array(ucos.memory.buffer, f.addr, f.width * f.height * 4);
    let n = 0;
    for (let i = 0; i < a.length; i += 4) if (a[i] !== b[i] || a[i + 1] !== b[i + 1] || a[i + 2] !== b[i + 2]) n++;
    return n;
  })()`);
  // let the page present the last write: wait two animation frames
  const settle = async () => { await t.sleep(150); await t.eval('new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)))'); };
  const out = {};
  const t0 = Date.now();
  while (!(await t.eval('!!(window.ucosBoot && window.ucosBoot.firstFrame)'))) { if (Date.now() - t0 > 30000) throw new Error('no first frame'); await t.sleep(100); }
  await t.sleep(6000);
  await t.click(400, 400);
  await t.type('echo damage check');
  await settle(); out.typed = await diff(); await t.shot('check-typed');
  await t.type('\nfetch -a\n');
  await t.sleep(1500);
  // fetch keeps drawing: stop the screen from changing under the comparison by pausing animation frames is not possible
  // from outside, so compare right after a frame, several times, and keep the smallest count.
  let best = Infinity;
  for (let i = 0; i < 5; i++) { await t.eval('new Promise((r) => requestAnimationFrame(r))'); const d = await diff(); if (typeof d === 'number' && d < best) best = d; }
  out.animating_min = best; await t.shot('check-fetch');
  await t.tap('KeyQ');
  await settle(); out.after_fetch = await diff();
  await t.resize(1000, 620); await t.sleep(1500); await settle(); out.resized_small = await diff(); await t.shot('check-resized-small');
  await t.resize(1280, 720); await t.sleep(1500); await settle(); out.resized_back = await diff(); await t.shot('check-resized-back');
  process.stdout.write('RESULT ' + JSON.stringify(out) + '\n');
};
