// SPDX-License-Identifier: GPL-2.0-or-later
// The clipboard history window on the desktop: open it from the launcher, pick an older
// entry with Down + Enter, delete one with Delete. Checked through the clip command in the
// welcome terminal.
//   node tools/shot.mjs --build=build-NAME --init=/bin/wm --size=1280x720 --script=bin/clipboard/shots.mjs
import { check, summary, screen, waitPrompt } from '../term/tests/os/lib.mjs';

async function run(t, cmd) { await t.type(cmd + '\n'); await t.sleep(300); return waitPrompt(t); }
async function newest(t) {
  const s = await run(t, 'echo "[$(clip -o)]"');
  const l = s.rows.filter((r) => r.startsWith('['));
  return l.length ? l[l.length - 1].trim() : '(nothing)';
}
async function open(t) {
  await t.tap('KeyD', { alt: true });
  await t.sleep(500);
  await t.type('clipboard');
  await t.sleep(150);
  await t.tap('Enter');
  await t.sleep(900);
}

export default async (t) => {
  await waitPrompt(t, 10000);
  await run(t, 'clear; clip -c; echo first | clip; echo second | clip; echo third | clip');
  check('three entries, newest is third', (await newest(t)) === '[third]');

  await open(t);
  t.shot('clipboard_window');
  await t.tap('ArrowDown');
  await t.sleep(100);
  t.shot('clipboard_window_down');
  await t.tap('Enter');
  await t.sleep(500);
  check('Down + Enter makes the second newest entry the newest', (await newest(t)) === '[second]');

  await open(t);
  await t.tap('Delete');
  await t.sleep(300);
  t.shot('clipboard_window_deleted');
  await t.tap('Escape');
  await t.sleep(500);
  const s = await run(t, 'clip -l');
  const list = s.rows.filter((r) => /^\s*\d+\s+\d+ B?/.test(r) || /^\s*\d\s+\d+\s/.test(r)).map((r) => r.trim());
  check('Delete removes the chosen entry', list.length >= 2 && /third/.test(list[list.length - 2]) && /first/.test(list[list.length - 1]),
        list.join(' | '));
  check('the terminal has the keyboard again after Escape', (await newest(t)) === '[third]');
  summary('clipboard window');
};
