// SPDX-License-Identifier: GPL-2.0-or-later
// In-OS test with real programs: key encodings (term --demo keys), vi, htop,
// fetch --animate and the truecolor animation demo, with frame timings.
//   node tools/shot.mjs --build=build-term --init=/bin/term --size=1280x720 --script=bin/term/tests/os/apps.mjs
import { check, summary, screen, waitFor, waitPrompt } from './lib.mjs';

export default async (t) => {
  await waitPrompt(t, 8000);

  // key encodings, as the application receives them in raw mode
  await t.type('term --demo keys\n');
  await t.sleep(500);
  const cases = [
    ['ArrowUp', {}, '\\x1b[A'], ['ArrowLeft', { ctrl: true }, '\\x1b[1;5D'], ['ArrowRight', { shift: true }, '\\x1b[1;2C'],
    ['Home', {}, '\\x1b[H'], ['End', {}, '\\x1b[F'], ['Insert', {}, '\\x1b[2~'], ['Delete', {}, '\\x1b[3~'],
    ['PageUp', {}, '\\x1b[5~'], ['PageDown', { ctrl: true }, '\\x1b[6;5~'], ['F1', {}, '\\x1bOP'], ['F4', {}, '\\x1bOS'],
    ['F5', {}, '\\x1b[15~'], ['F12', {}, '\\x1b[24~'], ['F2', { shift: true }, '\\x1b[1;2Q'], ['Enter', {}, '\\x0d'],
    ['Backspace', {}, '\\x7f'], ['Tab', {}, '\\x09'], ['Tab', { shift: true }, '\\x1b[Z'], ['Escape', {}, '\\x1b'],
    ['KeyA', { ctrl: true }, '\\x01'], ['KeyZ', { ctrl: true }, '\\x1a'], ['Space', { ctrl: true }, '\\x00'],
    ['BracketLeft', { ctrl: true }, '\\x1b'], ['Backslash', { ctrl: true }, '\\x1c'], ['BracketRight', { ctrl: true }, '\\x1d'],
    ['Digit6', { ctrl: true, shift: true }, '\\x1e'], ['Minus', { ctrl: true, shift: true }, '\\x1f'],
    ['KeyX', { alt: true }, '\\x1bx'], ['KeyX', { alt: true, shift: true }, '\\x1bX'], ['ArrowUp', { alt: true }, '\\x1b[1;3A'],
    ['KeyC', { ctrl: true }, '\\x03'],
  ];
  for (const [code, mods, want] of cases) {
    const before = t.log.length;
    await t.tap(code, mods);
    await waitFor(t, () => t.log.slice(before).includes('[demo-keys]'), 1500);
    const m = t.log.slice(before).match(/\[demo-keys\] (.*)/);
    check(`key ${Object.keys(mods).join('+')}${Object.keys(mods).length ? '+' : ''}${code} -> ${want}`, m && m[1] === want, m ? m[1] : 'nothing');
  }
  let before = t.log.length;
  await t.type('h\u00e9'.slice(0, 1)); // plain text
  await waitFor(t, () => t.log.slice(before).includes('[demo-keys]'), 1500);
  check('plain text key', /\[demo-keys\] h\n/.test(t.log.slice(before)), t.log.slice(before));
  // application cursor mode: vi and htop set it; test it through printf in the shell later
  t.shot('apps_keys');
  await t.type('q');
  await waitPrompt(t);

  // DECCKM: after ESC[?1h the arrows send ESC O A
  await t.type('printf "\\033[?1h"; term --demo keys\n');
  await t.sleep(500);
  before = t.log.length;
  await t.tap('ArrowUp');
  await waitFor(t, () => t.log.slice(before).includes('[demo-keys]'), 1500);
  check('application cursor keys', /\[demo-keys\] \\x1bOA/.test(t.log.slice(before)), t.log.slice(before));
  await t.type('q');
  await waitPrompt(t);
  await t.type('printf "\\033[?1l"; clear\n');
  await waitPrompt(t);

  // vi: alternate screen, then back
  await t.type('echo before-vi\n');
  await waitPrompt(t);
  await t.type('vi /etc/motd\n');
  await t.sleep(1500);
  let s = await screen(t);
  check('vi uses the alternate screen', s.alt === 1, `alt ${s.alt}`);
  check('vi shows the file', /WebAssembly/.test(s.text), s.text.slice(0, 300));
  t.shot('apps_vi');
  await t.type('ihello from vi ');
  await t.tap('Escape');
  await t.sleep(300);
  t.shot('apps_vi_edit');
  await t.type(':q!\n');
  s = await waitPrompt(t);
  check('leaving vi restores the main screen', s.alt === 0 && s.rows.includes('before-vi'), s.text);

  // htop for a few refreshes
  await t.type('htop\n');
  await t.sleep(2500);
  s = await screen(t);
  check('htop draws', s.alt === 1 && /term/.test(s.text), s.text.slice(0, 400));
  t.shot('apps_htop');
  await t.type('q');
  await waitPrompt(t);

  // fetch, static then animated (30 fps truecolor half blocks)
  await t.type('clear; fetch\n');
  await t.sleep(2000);
  t.shot('apps_fetch');
  before = t.log.length;
  await t.type('fetch -a\n');
  await t.sleep(4500);
  t.shot('apps_fetch_anim');
  await t.type('q');
  await t.sleep(500);
  const stats = t.log.slice(before).match(/\[term\].*/g) || [];
  for (const l of stats) console.log('INFO fetch -a: ' + l);
  await waitPrompt(t);

  // built-in truecolor animation, 4 seconds at 30 fps
  before = t.log.length;
  await t.type('term --demo anim 4\n');
  await t.sleep(2000);
  t.shot('apps_anim');
  await waitFor(t, () => t.log.slice(before).includes('[demo-anim]'), 15000);
  const a = t.log.slice(before).match(/\[demo-anim\] (\d+) frames in (\d+) ms/);
  check('animation keeps up (>= 25 fps)', a && a[1] / (a[2] / 1000) >= 25, a ? a[0] : 'no report');
  for (const l of t.log.slice(before).match(/\[term\].*/g) || []) console.log('INFO anim: ' + l);
  t.shot('apps_anim_end');
  process.exitCode = summary('apps') ? 1 : 0;
};
