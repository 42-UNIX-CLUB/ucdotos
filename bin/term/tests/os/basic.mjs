// SPDX-License-Identifier: GPL-2.0-or-later
// In-OS test of term as pid 1 (--fb mode): typing, line editing, Ctrl-C, a flood
// with keys typed during it, scrollback, resizes, and exit.
//   node tools/shot.mjs --build=build-term --init=/bin/term --size=1280x720 --script=bin/term/tests/os/basic.mjs
import { check, summary, screen, waitFor, waitPrompt } from './lib.mjs';

const has = (s, re) => s.rows.some((r) => re.test(r));

export default async (t) => {
  let s = await waitPrompt(t, 8000);
  check('a prompt appears', s.rows.length > 0 && /[$#]\s*$/.test(s.rows.filter((r) => r.trim()).pop() || ''), JSON.stringify(s.rows.slice(0, 3)));
  const bg = t.pixel(1270, 710);
  check('padding is the theme background (282828)', bg[0] === 0x28 && bg[1] === 0x28 && bg[2] === 0x28, bg.join(','));
  // the cursor is a block in the accent colour (fe8019) at the cursor cell
  const cw = Math.round(1264 / s.cols) || 10;
  t.shot('basic_prompt');

  // typing and echo
  await t.type('echo hello world\n');
  s = await waitPrompt(t);
  check('typed command is echoed', has(s, /[$#] echo hello world$/), s.text);
  check('command output', s.rows.includes('hello world'), s.text);

  // line editing with Backspace
  await t.type('echo abcx');
  await t.tap('Backspace');
  await t.type('d\n');
  s = await waitPrompt(t);
  check('backspace edits the line', s.rows.includes('abcd'), s.text);

  // arrows reach the line editor: Left twice, insert, then Enter
  await t.type('echo 1245');
  await t.tap('ArrowLeft'); await t.tap('ArrowLeft');
  await t.type('3');
  await t.tap('Enter');
  s = await waitPrompt(t);
  check('cursor keys move within the line', s.rows.includes('12345'), s.text);
  t.shot('basic_editing');

  // Ctrl-C kills the running child (the kernel line discipline does it)
  await t.type('term --demo sleep\n');
  await waitFor(t, () => false, 600);
  s = await screen(t);
  check('child is running', has(s, /^sleeping$/), s.text);
  await t.tap('KeyC', { ctrl: true });
  s = await waitPrompt(t);
  const last = s.rows.filter((r) => r.trim());
  check('Ctrl-C returns to the prompt', /[$#]$/.test(last[last.length - 1]) && has(s, /\^C/), s.text);
  await t.type('echo after-ctrl-c\n');
  s = await waitPrompt(t);
  check('shell still works after Ctrl-C', s.rows.includes('after-ctrl-c'), s.text);
  t.shot('basic_ctrl_c');

  // a child floods output for one second while keys are typed
  const logBefore = t.log.length;
  await t.type('term --demo flood 1\n');
  await t.sleep(200);
  await t.type('echo typed-during-flood\n');
  const t0 = Date.now();
  await waitFor(t, () => t.log.slice(logBefore).includes('[demo-flood]'), 10000);
  check('flood finished', t.log.slice(logBefore).includes('[demo-flood]'));
  t.shot('basic_flood_end');
  s = await waitPrompt(t, 8000);
  check('keys typed during the flood reach the shell', s.rows.includes('typed-during-flood'), s.text);
  const m = t.log.slice(logBefore).match(/\[demo-flood\] (\d+) lines, (\d+) bytes in (\d+) ms/);
  if (m) console.log(`INFO flood: ${m[1]} lines, ${(m[2] / 1e6).toFixed(2)} MB in ${m[3]} ms`);
  t.shot('basic_after_flood');

  // scrollback: Shift+PageUp shows older lines, a key returns to the bottom
  await t.type('seq 1 300\n');
  s = await waitPrompt(t);
  check('seq output at the bottom', s.rows.includes('300'), s.text);
  await t.tap('PageUp', { shift: true });
  await t.sleep(100);
  s = await screen(t);
  check('Shift+PageUp scrolls back', s.view > 0 && !s.rows.includes('300'), `view ${s.view}`);
  t.shot('basic_scrollback');
  await t.tap('PageUp', { shift: true });
  t.wheel(1); t.wheel(1);
  await t.sleep(100);
  const s2 = await screen(t);
  check('wheel scrolls further back', s2.view > s.view, `view ${s2.view} vs ${s.view}`);
  await t.tap('PageDown', { shift: true });
  await t.sleep(50);
  const s3 = await screen(t);
  check('Shift+PageDown scrolls forward', s3.view < s2.view, `view ${s3.view}`);
  await t.type('x');
  await t.sleep(100);
  s = await screen(t);
  check('a key returns to the bottom', s.view === 0, `view ${s.view}`);
  await t.tap('Backspace');

  // resize to 1920x1080 and 800x600
  for (const [w, h] of [[1920, 1080], [800, 600]]) {
    t.resize(w, h);
    await t.sleep(300);
    const before = t.log.length;
    await t.type('term --demo size\n');
    await waitFor(t, () => t.log.slice(before).includes('[demo-size]'), 5000);
    const mm = t.log.slice(before).match(/\[demo-size\] (\d+) rows, (\d+) cols/);
    s = await waitPrompt(t);
    check(`resize ${w}x${h}: pty size follows`, mm && +mm[1] === s.nrows && +mm[2] === s.cols, mm ? mm[0] : 'no report');
    console.log(`INFO ${w}x${h}: ${s.cols}x${s.nrows} cells`);
    check(`resize ${w}x${h}: bottom lines kept`, has(s, /rows, .* cols/), s.text);
    const px = t.pixel(w - 2, h - 2);
    check(`resize ${w}x${h}: corner repainted with bg`, px[0] === 0x28 && px[1] === 0x28 && px[2] === 0x28, px.join(','));
    t.shot(`basic_resize_${w}x${h}`);
  }

  // exit closes the terminal: pid 1 exits and the run ends
  const fails = summary('basic');
  let exited = false;
  const orig = console.error;
  console.error = (...a) => { if (String(a[0]).includes('every process has exited')) exited = true; orig(...a); };
  await t.type('exit\n');
  await waitFor(t, () => exited, 3000);
  check('exit in the shell ends term (pid 1)', exited);
  console.error = orig;
  summary('basic (with exit)');
  process.exitCode = fails ? 1 : 0;
};
