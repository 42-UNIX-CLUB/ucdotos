// SPDX-License-Identifier: GPL-2.0-or-later
// The theme picker ($mod SHIFT t) and every theme in /usr/share/themes:
//   - each theme previewed shows its own wallpaper colour (read from the theme file here)
//   - Escape goes back to the theme the picker started with, Enter keeps one
//   - a second lap through all themes grows no program (fonts change between themes)
//   node tools/shot.mjs --build=build-NAME --init=/bin/wm --size=1280x720 --script=bin/themes/shots.mjs
import { readFileSync, readdirSync } from 'node:fs';
import { check, done, same, hex } from '../wm/tests/lib.mjs';
import { screen, waitPrompt } from '../term/tests/os/lib.mjs';

const dir = new URL('../../rootfs/usr/share/themes/', import.meta.url);
const names = readdirSync(dir).filter((f) => f.endsWith('.uconf')).map((f) => f.slice(0, -6)).sort();
const desktopOf = (n) => {
  const m = readFileSync(new URL(n + '.uconf', dir), 'utf8').match(/^desktop\s*=\s*#([0-9a-fA-F]{6})/m);
  return m ? [0, 2, 4].map((i) => parseInt(m[1].slice(i, i + 2), 16)) : null;
};
const WALL = [5, 300]; // the gap at the left edge of the screen: always wallpaper

// memory of wm, term and bar, in KiB, through ps in the welcome terminal
async function mem(t) {
  await t.type("clear; ps | awk '$NF ~ /^(wm|term|bar)$/ { print \"MEM\", $NF, $4 }'\n");
  await t.sleep(600);
  const s = await screen(t);
  const o = {};
  for (const r of s.rows) { const m = r.match(/^MEM (\S+) (\d+)K/); if (m) o[m[1]] = +m[2]; }
  return o;
}
async function lap(t, shotPrefix) {
  const seen = [];
  for (let i = 0; i < names.length; i++) {
    await t.tap('ArrowRight');
    await t.sleep(700); // > 140 ms of quiet: the picker applies the theme
    const want = desktopOf(names[(start + 1 + i) % names.length]);
    seen.push(want && same(t.pixel(...WALL), want, 3));
    if (shotPrefix && i % 4 === 0) t.shot(`${shotPrefix}-${names[(start + 1 + i) % names.length]}`);
  }
  return seen;
}
let start = 0;

export default async (t) => {
  await waitPrompt(t, 10000);
  check(`${names.length} themes ship`, names.length >= 12, names.join(' '));
  start = names.indexOf('deck');
  const m0 = await mem(t);

  await t.tap('KeyT', { alt: true, shift: true });
  await t.sleep(1200);
  t.shot('40-picker');
  const seen = await lap(t, '41-theme');
  names.forEach((n, i) => check(`${names[(start + 1 + i) % names.length]}: its wallpaper colour`, seen[i]));
  await t.tap('Escape');
  await t.sleep(900);
  check('Escape: back to deck', same(t.pixel(...WALL), desktopOf('deck'), 3), hex(t.pixel(...WALL)));

  const m1 = await mem(t);
  await t.tap('KeyT', { alt: true, shift: true });
  await t.sleep(1200);
  await lap(t, null);
  await t.tap('Escape');
  await t.sleep(900);
  const m2 = await mem(t);
  for (const p of ['wm', 'term', 'bar'])
    check(`memory: ${p} does not grow on a second lap through every theme`, m1[p] > 0 && m2[p] === m1[p], `${m0[p]} -> ${m1[p]} -> ${m2[p]} KiB`);

  // Enter keeps the theme picked
  await t.tap('KeyT', { alt: true, shift: true });
  await t.sleep(1200);
  await t.tap('ArrowLeft'); // deck -> the theme before it in name order
  await t.sleep(400);
  await t.tap('Enter');
  await t.sleep(900);
  const picked = names[(start - 1 + names.length) % names.length];
  check(`Enter keeps ${picked}`, same(t.pixel(...WALL), desktopOf(picked), 3), hex(t.pixel(...WALL)));
  const s = await screen(t);
  await t.type('clear; cat ~/.config/ucos/theme\n');
  await t.sleep(500);
  const s2 = await screen(t);
  check('and writes it to ~/.config/ucos/theme', s2.rows.some((r) => r.trim() === picked), s2.rows.filter((r) => r.trim()).slice(-2).join(' | '));
  t.shot('42-picked');
  done('themes');
};
