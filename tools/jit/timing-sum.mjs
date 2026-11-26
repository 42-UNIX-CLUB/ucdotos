// SPDX-License-Identifier: GPL-2.0-or-later
// Summarise a timing file of tools/jit/timing.sh or timing-chrome.sh: per test and side,
// the run count, best, lower quartile and median wall time (ms), failed runs, and the load
// range seen. Side "off2" is the interpreter again (the control: off against off2 shows the
// noise of the session).
//   node tools/jit/timing-sum.mjs FILE
import fs from 'node:fs';
const lines = fs.readFileSync(process.argv[2], 'utf8').split('\n');
const t = {}, loads = {}, sides = new Set();
let cur = null, skip = false;
const exclude = process.env.EXCLUDE ? new RegExp(process.env.EXCLUDE) : null;   // headers of disturbed windows
for (const l of lines) {
  let m = l.match(/^== (\S+) load ([\d.]+)/);
  if (m && m[1] !== 'end') { skip = !!(exclude && exclude.test(l)); cur = m[1]; if (!skip) for (const c of cur.split(',')) (loads[c] ||= []).push(Number(m[2])); continue; }
  if (skip && !/^== end/.test(l)) continue;
  if (skip) { skip = false; continue; }
  m = l.match(/^== end load ([\d.]+)/);
  if (m && cur) { for (const c of cur.split(',')) (loads[c] ||= []).push(Number(m[1])); continue; }
  m = l.match(/^(\S+) (\S+) jtime ([\d.]+) ms exit (\d+)/);
  if (m) { sides.add(m[2]); const k = m[1] + ' ' + m[2]; (t[k] ||= []).push({ ms: Number(m[3]), exit: Number(m[4]) }); }
}
const q = (a, f) => { const s = [...a].sort((x, y) => x - y); return s[Math.floor((s.length - 1) * f)]; };
const order = ['off', 'off2', 'on', 'on500', 'onsmall', 'onsmall500', 'onsplit', 'on2'].filter((s) => sides.has(s));
console.log(`| test | ${order.map((s) => s + ' best / q1 / median (n)').join(' | ')} | load |`);
console.log(`|---|${order.map(() => '---|').join('')}---|`);
for (const test of [...new Set(Object.keys(t).map((k) => k.split(' ')[0]))]) {
  const cell = (side) => {
    const r = t[test + ' ' + side] || [];
    if (!r.length) return '';
    const ms = r.map((x) => x.ms), bad = r.filter((x) => x.exit).length;
    return `${q(ms, 0).toFixed(1)} / ${q(ms, 0.25).toFixed(1)} / ${q(ms, 0.5).toFixed(1)} (${r.length})${bad ? ` ${bad} failed` : ''}`;
  };
  const L = loads[test] || [0];
  console.log(`| ${test} | ${order.map(cell).join(' | ')} | ${Math.min(...L).toFixed(2)} to ${Math.max(...L).toFixed(2)} |`);
}
