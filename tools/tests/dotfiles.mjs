// SPDX-License-Identifier: GPL-2.0-or-later
// The .uconf dotfiles on the desktop: change each one from the welcome terminal and check
// that it takes effect. The bar and the launcher are checked by eye on the screenshots.
//   node tools/shot.mjs --build=build-NAME --init=/bin/wm --size=1280x720 --script=tools/tests/dotfiles.mjs
import { check, done, same, hex, until, DECK } from '../../bin/wm/tests/lib.mjs';
import { screen, waitPrompt } from '../../bin/term/tests/os/lib.mjs';

const H = '/home/user/.config';
async function run(t, cmd) { await t.type(cmd + '\n'); await t.sleep(250); return waitPrompt(t); }
// append lines to a file: printf turns \n into line breaks
const append = (t, file, text) => run(t, `printf '${text}' >> ${file}`);
async function reload(t) {
  const n = (t.log.match(/configuration reloaded/g) || []).length;
  await t.tap('KeyR', { alt: true, shift: true });
  await until(t, () => (t.log.match(/configuration reloaded/g) || []).length > n, 3000);
  await t.sleep(400);
}
const GREEN = [0, 255, 0], MAGENTA = [255, 0, 255];

export default async (t) => {
  await waitPrompt(t, 10000);
  if (await until(t, () => /keys: \d+ bindings/.test(t.log), 4000)) { await t.tap('Slash', { alt: true }); await t.sleep(400); } // hide the cheat sheet
  await run(t, 'clear');
  check('starts with the welcome terminal at the 12 px gap', same(t.pixel(13, 300), DECK.focus), hex(t.pixel(13, 300)));

  // wm.uconf: gaps, a border colour, a binding, an unbinding; a later setting wins
  await append(t, `${H}/wm/wm.uconf`, '\\n[general]\\ngaps = 40\\nborder { colors { focus = #00ff00 } }\\n[binds]\\nbind = $mod, y, workspace, 2\\nunbind = $mod, f\\n');
  await reload(t);
  t.shot('dot1-wm-gaps-colour');
  check('wm.uconf: gaps = 40 moves the window in', same(t.pixel(20, 300), DECK.desktop), hex(t.pixel(20, 300)));
  check('wm.uconf: the border colour wins over the theme', same(t.pixel(40, 300), GREEN) || same(t.pixel(41, 300), GREEN), hex(t.pixel(40, 300)) + ' ' + hex(t.pixel(41, 300)));
  await t.tap('KeyY', { alt: true });
  await t.sleep(500);
  check('wm.uconf: bind = $mod, y, workspace, 2 works', same(t.pixel(640, 400), DECK.desktop), hex(t.pixel(640, 400)));
  await t.tap('Digit1', { alt: true });
  await t.sleep(500);
  await t.tap('KeyF', { alt: true });
  await t.sleep(400);
  // with gaps = 40 the bar sits at y = 40: fullscreen would cover it
  check('wm.uconf: unbind = $mod, f removes the built-in fullscreen', same(t.pixel(400, 54), DECK.bar), hex(t.pixel(400, 54)));
  await t.tap('Enter');
  await waitPrompt(t);

  // a mistake is reported with the file and line, and the rest still applies
  await append(t, `${H}/wm/wm.uconf`, '[general]\\ngapz = 3\\n');
  await reload(t);
  check('wm.uconf: a typo is reported', /wm\.uconf:\d+: unknown setting 'general\.gapz'/.test(t.log));
  check('wm.uconf: and the rest still applies', same(t.pixel(20, 300), DECK.desktop), hex(t.pixel(20, 300)));

  // term.uconf: padding and font size, in a new terminal
  await append(t, `${H}/term/term.uconf`, '[window]\\npadding = 30\\n[font]\\nsize = 20\\n');
  await t.tap('Enter', { alt: true });
  await t.sleep(2500);
  const before = t.log.length;
  await screen(t);
  const m = t.log.slice(before).match(/cell (\d+)x(\d+) pad (\d+)/);
  check('term.uconf: padding = 30', m && +m[3] === 30, m ? m[0] : 'no dump');
  check('term.uconf: size = 20 makes bigger cells than 15', m && +m[2] > 19 && +m[1] > 9, m ? m[0] : 'no dump');
  t.shot('dot2-term-padding-size');
  await t.tap('KeyQ', { alt: true, shift: true });
  await t.sleep(800);

  // fetch.uconf: only two lines, one renamed, no palette
  await run(t, `printf '[info]\\nline = OS\\nline = Uptime, Up\\npalette = false\\n' > ${H}/fetch/fetch.uconf`);
  let s = await run(t, 'clear; fetch > /tmp/f; cat /tmp/f');
  const text = s.rows.join('\n');
  check('fetch.uconf: the OS line shows', /^OS\s+UC\.OS/m.test(text), text.slice(0, 300));
  check('fetch.uconf: Uptime shows as Up', /^Up\s+\d/m.test(text) && !/^Uptime/m.test(text));
  check('fetch.uconf: the other lines are gone', !/^Kernel/m.test(text) && !/^Memory/m.test(text));

  // bar.uconf: a clock format and fewer stats; the bar re-reads it on a theme change
  await run(t, `printf '[clock]\\nformat = CLOCK %%Y\\n[stats]\\nshow = procs\\n' > ${H}/bar/bar.uconf`);
  // a theme of our own: the wallpaper colour, through a theme change
  await run(t, `printf '[colors]\\ndesktop = #ff00ff\\n' > /usr/share/themes/mine.uconf`);
  for (let i = 0; i < 64 && !/wm: theme mine/.test(t.log); i++) { // $mod t cycles every theme in name order
    const n = (t.log.match(/wm: theme \S+/g) || []).length;
    await t.tap('KeyT', { alt: true });
    await until(t, () => (t.log.match(/wm: theme \S+/g) || []).length > n, 2000);
  }
  await t.sleep(1200);
  t.shot('dot3-theme-mine-bar');
  check('a new theme file: its desktop colour shows', same(t.pixel(20, 300), MAGENTA), hex(t.pixel(20, 300)));

  // launcher.uconf: 3 rows, 400 px wide
  await run(t, `printf '[launcher]\\nwidth = 400\\nrows = 3\\n' > ${H}/launcher/launcher.uconf`);
  await t.tap('KeyD', { alt: true });
  await t.sleep(800);
  t.shot('dot4-launcher-small');
  await t.tap('Escape');
  await t.sleep(300);
  done('dotfiles');
};
