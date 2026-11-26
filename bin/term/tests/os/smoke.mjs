// SPDX-License-Identifier: GPL-2.0-or-later
// Smoke test: boot term as pid 1, type a command, take a screenshot.
export default async (t) => {
  await t.sleep(1500);
  t.shot('smoke_boot');
  await t.type('term --demo colors\n');
  await t.sleep(800);
  t.shot('smoke_colors');
};
