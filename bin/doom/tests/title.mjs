// SPDX-License-Identifier: GPL-2.0-or-later
// Boot to the title screen and take a screenshot.
export default async (t) => {
  await t.sleep(4000);
  t.shot('title');
  console.error('[test] center pixel', t.pixel(640, 360), 'corner', t.pixel(5, 5));
};
