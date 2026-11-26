// SPDX-License-Identifier: GPL-2.0-or-later
// Selection, Ctrl+Shift+C and Ctrl+Shift+V in the terminal, through the clipboard (clipd).
//   node tools/shot.mjs --build=build-term --init=/bin/term --size=1280x720 --script=bin/term/tests/os/clipboard.mjs
import * as K from '../../../../boot/keys.mjs';
import { check, summary, screen, waitFor, waitPrompt } from './lib.mjs';

// Cell size and padding, from the screen dump's header.
async function geometry(t) {
  const before = t.log.length;
  await screen(t);
  const m = t.log.slice(before).match(/cell (\d+)x(\d+) pad (\d+)/);
  return m ? { cw: +m[1], ch: +m[2], pad: +m[3] } : null;
}
const btn = (t, down) => { t.sys.input(K.EV_KEY, K.BTN_LEFT, down ? 1 : 0); t.sys.input(K.EV_SYN, 0, 0); };
// The middle of cell (col, row) on screen; the terminal fills the screen from (0, 0).
const at = (g, col, row) => [g.pad + col * g.cw + (g.cw >> 1), g.pad + row * g.ch + (g.ch >> 1)];
const rowOf = (s, text) => s.rows.findIndex((r) => r.includes(text));

async function click(t, g, col, row, times = 1) {
  const [x, y] = at(g, col, row);
  for (let i = 0; i < times; i++) { t.move(x, y); btn(t, true); await t.sleep(15); btn(t, false); await t.sleep(15); }
}
async function drag(t, g, c0, r0, c1, r1, shift = false) {
  if (shift) t.key('ShiftLeft', true);
  t.move(...at(g, c0, r0)); btn(t, true); await t.sleep(20);
  for (let i = 1; i <= 5; i++) { // a few steps, like a real hand
    const [x0, y0] = at(g, c0, r0), [x1, y1] = at(g, c1, r1);
    t.move(Math.round(x0 + (x1 - x0) * i / 5), Math.round(y0 + (y1 - y0) * i / 5)); await t.sleep(15);
  }
  btn(t, false); await t.sleep(20);
  if (shift) t.key('ShiftLeft', false);
}
const copy = (t) => t.tap('KeyC', { ctrl: true, shift: true });
const paste = (t) => t.tap('KeyV', { ctrl: true, shift: true });
// Run a command and give back the screen once the prompt is back.
async function run(t, cmd) { await t.type(cmd + '\n'); await t.sleep(300); return waitPrompt(t); }
// What `clip -o` prints, between brackets on its own line.
async function clipboard(t) {
  const s = await run(t, 'echo "[$(clip -o)]"');
  const lines = s.rows.filter((r) => r.startsWith('['));
  return lines.length ? lines[lines.length - 1].trim() : '(nothing)';
}

export default async (t) => {
  let s = await waitPrompt(t, 8000);
  const g = await geometry(t);
  check('screen dump gives the cell size', !!g, JSON.stringify(g));
  if (!g) { summary('clipboard'); return; }
  s = await run(t, 'clear; echo alpha beta gamma');
  let r = rowOf(s, 'alpha beta gamma');
  check('the sample line is on screen', r >= 0, s.text.slice(0, 200));
  const col = s.rows[r].indexOf('alpha');

  // double click: a word
  await click(t, g, col + 7, r, 2); // inside "beta"
  t.shot('clip_word_selected');
  await copy(t);
  check('double click + Ctrl+Shift+C copies the word', (await clipboard(t)) === '[beta]');

  // drag: from the a of alpha to the a at the end of beta
  s = await run(t, 'clear; echo alpha beta gamma');
  r = rowOf(s, 'alpha beta gamma');
  await drag(t, g, col, r, col + 9, r);
  t.shot('clip_drag_selected');
  await copy(t);
  check('drag + Ctrl+Shift+C copies the dragged text', (await clipboard(t)) === '[alpha beta]');

  // triple click: the line, blanks at the end left out
  s = await run(t, 'clear; echo alpha beta gamma');
  r = rowOf(s, 'alpha beta gamma');
  await click(t, g, col + 2, r, 3);
  await copy(t);
  check('triple click copies the whole line', (await clipboard(t)) === '[alpha beta gamma]');

  // a selection over two lines keeps the line break
  s = await run(t, 'clear; echo one-two; echo three-four');
  const r1 = rowOf(s, 'one-two'), r2 = rowOf(s, 'three-four');
  await drag(t, g, 0, r1, 4, r2);
  await copy(t);
  s = await run(t, 'clip -o | wc -l');
  check('a two-line selection copies a line break', s.rows.some((l) => l.trim() === '1'), s.rows.filter((l) => l.trim()).slice(-3).join(' | '));

  // a single click (no drag) leaves nothing selected: Ctrl+Shift+C changes nothing
  await run(t, 'echo kept | clip');
  await click(t, g, 3, 0);
  await copy(t);
  check('a plain click selects nothing', (await clipboard(t)) === '[kept]');

  // Ctrl+Shift+V types the newest entry
  await run(t, 'clear; echo pasted-text | clip');
  await t.type('echo X');
  await paste(t);
  s = await run(t, '');
  check('Ctrl+Shift+V pastes at the cursor', s.rows.some((l) => l.trim() === 'Xpasted-text'), s.rows.filter((l) => l.trim()).slice(-3).join(' | '));

  // several lines paste as several Enter presses
  await run(t, "clear; printf 'echo L1-ok\\necho L2-ok\\n' | clip");
  await paste(t);
  await t.sleep(400);
  s = await waitPrompt(t);
  check('a two-line paste runs two commands', rowOf(s, 'L1-ok') >= 0 && s.rows.some((l) => l.trim() === 'L2-ok'), s.rows.filter((l) => l.trim()).slice(-4).join(' | '));

  // Ctrl+C alone still interrupts (Shift makes the difference)
  await t.type('sleep 30\n');
  await t.sleep(300);
  await t.tap('KeyC', { ctrl: true });
  s = await waitPrompt(t, 3000);
  check('Ctrl+C without Shift still interrupts', /\$$/.test((s.rows.filter((l) => l.trim()).pop() || '').trim()));

  // A program that asked for the mouse (mode 1000; nothing in the system does, so printf
  // turns it on and head reads the reports, after Enter since the pty is in line mode): Shift+drag selects and sends nothing, a plain
  // drag goes to the program (a press and a release report, 6 bytes each) and selects nothing.
  await run(t, "clear; clip -c; echo before | clip; echo mouse-line-one; echo mouse-line-two");
  s = await screen(t);
  const m1 = rowOf(s, 'mouse-line-one'), m2 = rowOf(s, 'mouse-line-two');
  await t.type("printf '\\033[?1000h'; head -c 12 | wc -c; printf '\\033[?1000l'\n");
  await t.sleep(500);
  await drag(t, g, 0, m1, 13, m1, true);
  await copy(t);
  await drag(t, g, 0, m2, 13, m2);
  await copy(t);
  await t.tap('Enter'); // the pty is in line mode: head gets the reports once the line ends
  s = await waitPrompt(t, 4000);
  check('with mouse reporting on, a plain drag reaches the program', s.rows.some((l) => l.trim() === '12'), s.rows.filter((l) => l.trim()).slice(-3).join(' | '));
  s = await run(t, 'clip -l | wc -l');
  const entries = +(s.rows.map((l) => l.trim()).filter((l) => /^\d+$/.test(l)).pop());
  const got = await clipboard(t);
  check('with mouse reporting on, Shift+drag copies and a plain drag does not', entries === 2 && got === '[mouse-line-one]', `${entries} entries, newest ${got}`);

  // The computer's clipboard: the page calls sys.hostPaste on Ctrl+V; clipd adds it.
  await run(t, 'clear; clip -c');
  check('hostPaste takes text', t.sys.hostPaste('from-the-host caf\u00e9 \u2713') === true);
  await t.sleep(300);
  check('a host paste becomes the newest entry', (await clipboard(t)) === '[from-the-host caf\u00e9 \u2713]');
  await t.type('echo Y');
  await paste(t);
  s = await run(t, '');
  check('Ctrl+Shift+V then pastes it', s.rows.some((l) => l.trim() === 'Yfrom-the-host caf\u00e9 \u2713'), s.rows.filter((l) => l.trim()).slice(-2).join(' | '));
  t.sys.hostPaste('line-a\nline-b\n');
  await t.sleep(300);
  s = await run(t, 'clip -o | wc -l');
  check('a host paste keeps its line breaks', s.rows.some((l) => l.trim() === '2'), s.rows.filter((l) => l.trim()).slice(-2).join(' | '));
  check('a host paste over 1 MiB is refused', t.sys.hostPaste('x'.repeat((1 << 20) + 1)) === false);
  check('a host paste of exactly 1 MiB is taken', t.sys.hostPaste('y'.repeat(1 << 20)) === true);
  await t.sleep(500);
  s = await run(t, 'clip -o | wc -c');
  check('and arrives whole', s.rows.some((l) => l.trim() === String(1 << 20)), s.rows.filter((l) => l.trim()).slice(-2).join(' | '));
  s = await run(t, 'clip -l | wc -l');
  check('every host paste is its own entry', s.rows.some((l) => l.trim() === '3'), s.rows.filter((l) => l.trim()).slice(-2).join(' | '));

  // Out to the computer's clipboard: what becomes the newest entry by a copy or a pick
  // (the page then calls navigator.clipboard.writeText; here we read what the host got).
  s = await run(t, 'clear; echo out-alpha out-beta');
  r = rowOf(s, 'out-alpha out-beta');
  await click(t, g, s.rows[r].indexOf('out-beta') + 2, r, 2);
  await copy(t);
  await t.sleep(300);
  check('Ctrl+Shift+C also goes to the computer\'s clipboard', t.sys.clipboardOut === 'out-beta', JSON.stringify(t.sys.clipboardOut));
  await run(t, 'printf "caf\\303\\251 two\\nlines" | clip');
  check('so does `clip`, UTF-8 and line breaks kept', t.sys.clipboardOut === 'caf\u00e9 two\nlines', JSON.stringify(t.sys.clipboardOut));
  await run(t, 'clip -s 1');
  check('picking an older entry sends it out', t.sys.clipboardOut === 'out-beta', JSON.stringify(t.sys.clipboardOut));
  t.sys.hostPaste('came-from-the-host');
  await t.sleep(300);
  await run(t, 'clip -o > /dev/null');
  check('a paste from the computer is not sent back', t.sys.clipboardOut === 'out-beta', JSON.stringify(t.sys.clipboardOut));
  await run(t, 'seq 1 100000 | clip');
  check('a 589 KB copy goes out whole', t.sys.clipboardOut && t.sys.clipboardOut.length === 588895 && t.sys.clipboardOut.endsWith('99999\n100000\n'),
        String(t.sys.clipboardOut && t.sys.clipboardOut.length));

  // big pastes: how much arrives in `cat > file`
  for (const n of [3000, 20000, 100000]) {
    await run(t, `clear; seq 1 ${n} | clip; seq 1 ${n} | wc -c`);
    s = await screen(t);
    const want = +(s.rows.map((l) => l.trim()).filter((l) => /^\d+$/.test(l)).pop());
    await t.type('cat > /tmp/p\n');
    await t.sleep(300);
    await paste(t);
    await waitFor(t, () => false, Math.max(500, want / 1024 * 6 + 500)); // the paste goes out 1 KiB per 4 ms
    await t.tap('KeyD', { ctrl: true });
    s = await run(t, 'wc -c < /tmp/p');
    const got = +(s.rows.map((l) => l.trim()).filter((l) => /^\d+$/.test(l)).pop());
    check(`paste of ${want} bytes arrives whole`, got === want, `${got} of ${want}`);
  }
  summary('clipboard');
};
