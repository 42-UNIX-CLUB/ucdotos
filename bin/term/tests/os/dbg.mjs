// SPDX-License-Identifier: GPL-2.0-or-later
export default async (t) => {
  await t.sleep(1000);
  await t.type((process.env.CMD || 'term --demo size') + '\n');
  await t.sleep(1500);
  t.shot('dbg');
};
