// SPDX-License-Identifier: GPL-2.0-or-later
// term in a window: boot the window manager, open terminals with Alt+Return, type,
// tile another one (resize), switch the theme (Alt+t: a UI_EV_RESIZE with the same size).
// The desktop already starts one terminal by itself, so the windows opened here are the
// second and third: the new one may be narrower or shorter than the one before.
//   node tools/shot.mjs --build=build-term --init=/bin/wm --size=1280x720 --script=bin/term/tests/os/window.mjs
import { check, summary, screen, waitFor } from './lib.mjs';
export default async (t) => {
  await t.sleep(2500);
  t.shot('window_desktop');
  await t.tap('Enter', { alt: true });
  await t.sleep(2500);
  t.shot('window_term_open');
  await t.type('echo hello from a window\n');
  await t.sleep(800);
  let s = await screen(t);
  check('term window runs a shell', s.rows.includes('hello from a window'), s.text.slice(0, 300));
  const first = { cols: s.cols, rows: s.nrows };
  console.log(`INFO first window: ${s.cols}x${s.nrows}`);
  await t.type('fetch\n');
  await t.sleep(2000);
  t.shot('window_fetch');
  await t.tap('Enter', { alt: true });
  await t.sleep(2500);
  await t.type('term --demo size\n');
  await t.sleep(800);
  s = await screen(t);
  console.log(`INFO next window: ${s.cols}x${s.nrows}`);
  check('the next window is tiled smaller', s.cols < first.cols || s.nrows < first.rows, `${s.cols}x${s.nrows} vs ${first.cols}x${first.rows}`);
  check('pty size matches the grid', s.text.includes(`${s.nrows} rows, ${s.cols} cols`), s.text.slice(0, 300));
  t.shot('window_two');
  await t.tap('KeyT', { alt: true });
  await t.sleep(1500);
  t.shot('window_theme_next');
  s = await screen(t);
  check('text survives a theme switch', s.text.includes('rows,'), s.text.slice(0, 200));
  process.exitCode = summary('window') ? 1 : 0;
};
