// SPDX-License-Identifier: GPL-2.0-or-later
// 20 windows opened and closed in a loop (memory must not grow), idle check (no framebuffer
// writes), full-frame composite time, and ui_present -> framebuffer latency for 600x400.
import { check, done, until, demos, count, openDemo, openDemos, stats, boot } from './lib.mjs';

export default async (t) => {
  await boot(t);
  check('base window', await openDemo(t));
  const mem = [];
  for (let cycle = 0; cycle < 3; cycle++) {
    const ok = await openDemos(t, 20);
    check(`cycle ${cycle}: 20 more windows`, ok && demos(t).size === 21, String(demos(t).size));
    if (cycle === 0) t.shot('30-twenty-one-windows');
    for (let i = 0; i < 20; i++) {
      const n = demos(t).size;
      await t.tap('KeyQ', { alt: true, shift: true });
      await until(t, () => demos(t).size === n - 1, 3000);
    }
    await t.sleep(400);
    check(`cycle ${cycle}: back to one window`, demos(t).size === 1, String(demos(t).size));
    const s = await stats(t);
    mem.push(s);
    console.error(`cycle ${cycle}: system memory ${(s.mem_total / 1048576).toFixed(1)} MiB, kernel heap ${(s.kmem_used / 1048576).toFixed(2)} MiB, wm ${s.wm_pages} pages, ${s.procs} procs, ${s.clients} clients`);
  }
  check('no leak: system memory after cycle 3 within 2 MiB of cycle 1', Math.abs(mem[2].mem_total - mem[0].mem_total) < 2 * 1048576, `${mem[0].mem_total} -> ${mem[2].mem_total}`);
  check('no leak: kernel heap stable', Math.abs(mem[2].kmem_used - mem[0].kmem_used) < 256 * 1024, `${mem[0].kmem_used} -> ${mem[2].kmem_used}`);
  check('no leak: wm memory did not grow after the first cycle', mem[2].wm_pages === mem[1].wm_pages, `${mem[0].wm_pages} ${mem[1].wm_pages} ${mem[2].wm_pages}`);
  check('process count back', mem[2].procs === mem[0].procs, `${mem[0].procs} ${mem[2].procs}`);

  // idle: with the bar visible the only writes are its clock and stats once a second
  let a = await stats(t);
  await t.sleep(1000);
  let b = await stats(t);
  console.error(`idle with the bar visible: ${b.fbwrites - a.fbwrites} framebuffer writes, ${b.fbbytes - a.fbbytes} bytes, ${b.frames - a.frames} frames in 1 s`);
  check('idle with bar: at most 2 frames per second (clock ticks)', b.frames - a.frames <= 2, String(b.frames - a.frames));
  // fullscreen hides the bar: now nothing on screen changes, so nothing is written
  await t.tap('KeyF', { alt: true });
  await t.sleep(400);
  a = await stats(t);
  await t.sleep(1000);
  b = await stats(t);
  console.error(`idle, fullscreen window: ${b.fbwrites - a.fbwrites} framebuffer writes, ${b.frames - a.frames} frames in 1 s`);
  check('idle: zero framebuffer writes in 1 s', b.fbwrites === a.fbwrites && b.frames === a.frames, `${b.fbwrites - a.fbwrites}`);
  await t.tap('KeyF', { alt: true });
  await t.sleep(300);

  // full-frame composite benchmark (paint + framebuffer write), measured inside the manager
  await t.tap('KeyB');
  await until(t, () => /wm: bench \d+x\d+/.test(t.log), 5000);
  const bench = t.log.match(/wm: bench paint ([\d.]+) ms \+ framebuffer write ([\d.]+) ms/);
  const full = t.log.match(/wm: bench (\d+x\d+) full-screen .*: ([\d.]+) ms per frame/);
  check('bench ran', !!(bench && full));
  if (bench) console.error(`RESULT full ${full[1]} frame: paint ${bench[1]} ms + framebuffer write ${bench[2]} ms = ${full[2]} ms`);

  // latency probe: a floating 600x400 window presents five full frames with tracing on
  await t.tap('KeyP');
  await until(t, () => count(t, /wm: trace .*present 600x400/g) >= 5, 8000);
  await t.sleep(200);
  t.shot('31-latency-probe');
  const starts = [...t.log.matchAll(/uidemo: latency t=([\d.]+) present 600x400 start; the write took ([\d.]+) ms/g)].map((m) => [+m[1], +m[2]]);
  const ends = [...t.log.matchAll(/wm: trace t=([\d.]+) present 600x400 .*first byte -> framebuffer ([\d.]+) ms/g)].map((m) => [+m[1], +m[2]]);
  const lat = [];
  for (const [s, w] of starts) {
    const e = ends.find(([te]) => te >= s);
    if (e) lat.push([e[0] - s, w, e[1]]);
  }
  check('latency: five samples', lat.length >= 5, String(lat.length));
  for (const [l, w, f] of lat) console.error(`RESULT ui_present(600x400) -> framebuffer: ${l.toFixed(3)} ms (client write ${w.toFixed(3)} ms, wm first byte -> fb ${f.toFixed(3)} ms)`);
  if (lat.length) {
    const sorted = lat.map((x) => x[0]).sort((x, y) => x - y);
    console.error(`RESULT latency median ${sorted[Math.floor(sorted.length / 2)].toFixed(3)} ms, min ${sorted[0].toFixed(3)}, max ${sorted[sorted.length - 1].toFixed(3)}`);
  }
  done('perf');
};
