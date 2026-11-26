// SPDX-License-Identifier: GPL-2.0-or-later
// Worker-process prototype: the real desktop in headless Chromium (boot/worker-proto/desktop.html,
// the ordinary page.mjs with the worker extension). In a terminal, a program computes for
// 20 s without system calls, in a Worker; meanwhile a second terminal opens and works, the
// page keeps drawing, and Ctrl-C stops the program. Then the same with the ordinary build
// on the main thread, for comparison (it freezes the page for the length of the loop).
//   node tools/worker-proto.mjs --browser --build=build-NAME --test=desktop
import * as K from '../../boot/keys.mjs';

export default async (t) => {
  const { send, ev, sleep, shot } = t;
  const key = async (code, down, mods = {}, text = '') => {
    const bits = (mods.alt ? 1 : 0) | (mods.ctrl ? 2 : 0) | (mods.meta ? 4 : 0) | (mods.shift ? 8 : 0);
    const k = text || (code.startsWith('Key') ? code.slice(3).toLowerCase() : code.startsWith('Digit') ? code.slice(5) : code);
    const typed = text || (mods.ctrl || mods.alt || mods.meta ? '' : { Enter: '\r', Space: ' ' }[code] || '');
    await send('Input.dispatchKeyEvent', { type: down ? (typed ? 'keyDown' : 'rawKeyDown') : 'keyUp', code, key: k, modifiers: bits, ...(down && typed ? { text: typed } : {}) });
  };
  const tap = async (code, mods = {}, text = '') => {
    const held = [mods.ctrl && 'ControlLeft', mods.alt && 'AltLeft', mods.shift && 'ShiftLeft'].filter(Boolean);
    for (const m of held) await key(m, true, mods);
    await key(code, true, mods, mods.ctrl || mods.alt ? '' : text);
    await sleep(8);
    await key(code, false, mods);
    for (const m of held.reverse()) await key(m, false);
    await sleep(8);
  };
  const type = async (s) => { for (const ch of s) { const k = K.keyForChar(ch); await tap(k.code, { shift: k.shift }, ch === '\n' ? '\r' : ch); } };
  // the page's own frame gaps, measured inside the page
  const gaps = async (reset) => ev(`(() => { const g = window.__gap || (window.__gap = { max: 0, last: 0, n: 0 });
    if (!window.__gapOn) { window.__gapOn = 1; const f = () => { const t = performance.now(); if (g.last && t - g.last > g.max) g.max = t - g.last; g.last = t; g.n++; requestAnimationFrame(f); }; requestAnimationFrame(f); }
    const r = { max: g.max, n: g.n }; if (${reset}) { g.max = 0; g.n = 0; } return r; })()`);
  // The page keeps F12 for the browser, so the terminal's text hook does not work here:
  // what the terminals show is checked on the screenshots.

  await sleep(4500);
  await gaps(true);
  await shot('wp-d01-boot');
  for (const [prog, label] of [['wproto-a', 'A'], ['wproto-b', 'B']]) {
    await type(`${prog} spin 20\n`);
    await sleep(1500);
    await tap('Enter', { alt: true });           // a second terminal must open while it spins
    await sleep(2500);
    await type(`echo second terminal works; ps\n`);
    await sleep(1200);
    await shot(`wp-d02-${label}-spinning`);        // expect: ps in the second terminal shows the program as R
    await tap('KeyQ', { alt: true, shift: true }); // close the second terminal (wm binding), focus returns
    await sleep(800);
    const killsBefore = (t.log.match(/worker terminated by the kernel/g) || []).length;
    await tap('KeyC', { ctrl: true });            // Ctrl-C: the kernel kills the foreground job
    await sleep(800);
    await type(`echo back at the prompt\n`);
    await sleep(800);
    const killed = (t.log.match(/worker terminated by the kernel/g) || []).length > killsBefore;
    const g = await gaps(true);
    console.log(`[test] desktop ${label}: Ctrl-C ${killed ? 'terminated the Worker' : 'DID NOT terminate the Worker'}; ` +
      `page longest frame gap ${g.max.toFixed(1)} ms over ${g.n} frames (see the wp-d02/wp-d03 screenshots)`);
    await shot(`wp-d03-${label}-after-ctrl-c`);
    await type('clear\n');
    await sleep(400);
  }
  // the ordinary build: the same loop on the main thread freezes the page for its length
  await gaps(true);
  await type('wproto spin 3\n');
  await sleep(5000);
  const g = await gaps(true);
  console.log(`[test] desktop main thread (ordinary build, 3 s loop): page longest frame gap ${g.max.toFixed(1)} ms`);
  console.log(`[test] killed-by-kernel events: ${(t.log.match(/worker terminated by the kernel/g) || []).length}`);
  console.log('[test] done');
};
