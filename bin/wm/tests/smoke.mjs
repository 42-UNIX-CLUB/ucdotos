// SPDX-License-Identifier: GPL-2.0-or-later
// Smoke test: boot, open the launcher, start a uidemo, take screenshots.
export default async (t) => {
  await t.sleep(1500);
  t.shot('smoke-boot');
  await t.tap('KeyD', { alt: true });
  await t.sleep(500);
  t.shot('smoke-launcher');
  await t.type('uidemo');
  await t.sleep(300);
  t.shot('smoke-filter');
  await t.tap('Enter');
  await t.sleep(1000);
  t.shot('smoke-window');
};
