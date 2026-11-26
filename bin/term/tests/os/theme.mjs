// SPDX-License-Identifier: GPL-2.0-or-later
// Theme switch: write ~/.config/ucos/theme, then a resize event (which, like the window
// manager's theme notification, makes term reload the theme) repaints in the new theme.
import { check, summary, screen, waitPrompt } from './lib.mjs';
const near = (p, rgb) => Math.abs(p[0] - (rgb >> 16)) < 3 && Math.abs(p[1] - ((rgb >> 8) & 255)) < 3 && Math.abs(p[2] - (rgb & 255)) < 3;
export default async (t) => {
  await waitPrompt(t, 8000);
  check('deck background', near(t.pixel(1275, 715), 0x282828), t.pixel(1275, 715).join(','));
  await t.type('term --demo colors\n');
  await waitPrompt(t);
  t.shot('theme_deck');
  await t.type('mkdir -p /home/user/.config/ucos; echo brand > /home/user/.config/ucos/theme\n');
  await waitPrompt(t);
  t.resize(1280, 719); await t.sleep(300); t.resize(1280, 720); await t.sleep(500);
  const s = await screen(t);
  check('brand background after reload', near(t.pixel(1275, 715), 0x0e362e), t.pixel(1275, 715).join(','));
  check('text survives the theme change', s.text.includes('16 colours'), s.text.slice(0, 200));
  await t.type('echo $THEME; term --demo colors\n');
  await waitPrompt(t);
  t.shot('theme_brand');
  process.exitCode = summary('theme') ? 1 : 0;
};
