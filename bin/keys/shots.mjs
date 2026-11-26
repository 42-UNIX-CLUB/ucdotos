// SPDX-License-Identifier: GPL-2.0-or-later
// The keybinding cheat sheet (top right) and its hint (bottom), started by the default wm.uconf.
//   node tools/shot.mjs --build=build-NAME --init=/bin/wm --size=1280x720 --script=bin/keys/shots.mjs
import { check, done, same, hex, until, DECK } from '../wm/tests/lib.mjs';
import { screen, waitPrompt } from '../term/tests/os/lib.mjs';

const SHEET = [1150, 570], HINT = [640, 700];  // empty spots inside the sheet (below its second column) and the hint
const DRACULA_PANEL = [0x44, 0x47, 0x5a];
const count = (t) => { const m = [...t.log.matchAll(/keys: (\d+) bindings/g)]; return m.length ? +m[m.length - 1][1] : -1; };
const shown = (t, at, panel = DECK.bar) => same(t.pixel(...at), panel); // deck: panel = 3c3836, the bar colour
async function toggle(t) { await t.tap('Slash', { alt: true }); await t.sleep(500); }
async function reload(t) {
  const n = (t.log.match(/configuration reloaded/g) || []).length;
  await t.tap('KeyR', { alt: true, shift: true });
  await until(t, () => (t.log.match(/configuration reloaded/g) || []).length > n, 3000);
  await t.sleep(700);
}
async function memKeys(t) {
  await t.type("clear; ps | awk '$NF == \"keys\" { print \"KEYSK\", $4 }'\n");
  await t.sleep(600);
  const s = await screen(t);
  const m = s.rows.map((r) => r.match(/^KEYSK (\d+)K/)).filter(Boolean).pop();
  return m ? +m[1] : -1;
}

export default async (t) => {
  await waitPrompt(t, 10000);
  await until(t, () => count(t) > 0, 4000);
  t.shot('50-keys-at-launch');
  check('at launch: the sheet shows, top right', shown(t, SHEET), hex(t.pixel(...SHEET)));
  check('at launch: the hint shows, at the bottom', shown(t, HINT), hex(t.pixel(...HINT)));
  check('the sheet lists every bind line of wm.uconf', count(t) >= 40, String(count(t)));

  // typing still goes to the terminal: the sheet never takes the focus
  await t.type('clear; echo typed-here\n');
  await t.sleep(400);
  let s = await screen(t);
  check('the terminal keeps the keyboard', s.rows.some((r) => r.trim() === 'typed-here'));

  await toggle(t);
  t.shot('51-keys-hidden');
  check('$mod /: the sheet hides', !shown(t, SHEET), hex(t.pixel(...SHEET)));
  check('$mod /: and the hint with it', !shown(t, HINT), hex(t.pixel(...HINT)));
  await toggle(t);
  check('$mod / again: both come back', shown(t, SHEET) && shown(t, HINT), hex(t.pixel(...SHEET)) + ' ' + hex(t.pixel(...HINT)));

  // sticky: on every workspace; the slide does not carry it away
  await t.tap('Digit2', { alt: true });
  await t.sleep(600);
  check('on workspace 2 too', shown(t, SHEET) && shown(t, HINT), hex(t.pixel(...SHEET)));
  await t.tap('Digit1', { alt: true });
  await t.sleep(600);

  // a bind line added, then removed again: the sheet follows the file on reload
  const n0 = count(t);
  await t.type("printf '[binds]\\nbind = $mod, x, exec, fetch\\n' >> ~/.config/wm/wm.uconf\n");
  await t.sleep(300);
  await reload(t);
  check('a new bind line shows after a reload', count(t) === n0 + 1, `${n0} -> ${count(t)}`);
  await t.type("printf 'unbind = $mod, x\\nunbind = $mod, slash\\nbind = $mod, F1, toggle, keys\\n' >> ~/.config/wm/wm.uconf\n");
  await t.sleep(300);
  await reload(t);
  t.shot('52-keys-toggle-on-f1');
  check('unbind removes it; the toggle moved to $mod F1', count(t) === n0, `${count(t)}`);
  await t.tap('F1', { alt: true });
  await t.sleep(500);
  check('the new key toggles the sheet', !shown(t, SHEET), hex(t.pixel(...SHEET)));
  await t.tap('F1', { alt: true });
  await t.sleep(500);

  // theme: the sheet takes the new colours
  for (let i = 0; i < 64 && !/wm: theme dracula/.test(t.log); i++) {
    const k = (t.log.match(/wm: theme \S+/g) || []).length;
    await t.tap('KeyT', { alt: true });
    await until(t, () => (t.log.match(/wm: theme \S+/g) || []).length > k, 2000);
  }
  await t.sleep(800);
  t.shot('53-keys-dracula');
  check('theme change: the sheet uses the new theme', shown(t, SHEET, DRACULA_PANEL), hex(t.pixel(...SHEET)));

  // a click on the hint hides both
  await t.click(...HINT);
  await t.sleep(500);
  check('a click on the hint hides both', !shown(t, SHEET, DRACULA_PANEL) && !shown(t, HINT, DRACULA_PANEL));
  await t.tap('F1', { alt: true });
  await t.sleep(500);

  // a screen too small for the sheet: the window manager shrinks it, and keys must not keep
  // reopening it (it once did, forever)
  t.resize(800, 600);
  await t.sleep(1500);
  t.resize(1280, 720);
  await t.sleep(1500);
  await t.tap('F1', { alt: true });
  await t.sleep(500);
  check('after a too-small screen, the toggle still answers', !shown(t, SHEET, DRACULA_PANEL), hex(t.pixel(...SHEET)));
  await t.tap('F1', { alt: true });
  await t.sleep(500);

  // memory: toggling and reloading again and again does not grow the program
  const m1 = await memKeys(t);
  for (let i = 0; i < 6; i++) { await t.tap('F1', { alt: true }); await t.sleep(250); await t.tap('F1', { alt: true }); await t.sleep(250); await reload(t); }
  const m2 = await memKeys(t);
  check('memory: keys does not grow over 12 toggles and 6 reloads', m1 > 0 && m2 === m1, `${m1} -> ${m2} KiB`);
  done('keys');
};
