// SPDX-License-Identifier: GPL-2.0-or-later
// The slide-26 composition: bar, a master and two stacked windows, the launcher open.
// Writes showcase-deck.png and showcase-brand.png.
import { check, done, until, openDemos, boot } from './lib.mjs';

export default async (t) => {
  await boot(t);
  check('three windows', await openDemos(t, 3));
  await t.tap('KeyJ', { alt: true }); // focus the master, like the slide
  await t.sleep(200);
  await t.tap('KeyD', { alt: true });
  await t.sleep(500);
  await t.type('fe');
  await t.sleep(300);
  t.shot('showcase-deck');
  await t.tap('Escape');
  await t.tap('KeyT', { alt: true });
  await until(t, () => /wm: theme brand/.test(t.log));
  await t.sleep(700);
  await t.tap('KeyD', { alt: true });
  await t.sleep(500);
  await t.type('fe');
  await t.sleep(300);
  t.shot('showcase-brand');
  await t.tap('Escape');
  await t.tap('KeyT', { alt: true }); // leave the theme file as it was
  await until(t, () => /wm: theme deck/.test(t.log));
  done('showcase');
};
