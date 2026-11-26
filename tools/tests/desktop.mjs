// SPDX-License-Identifier: GPL-2.0-or-later
// Whole-system smoke test: boot through init to the desktop, use it, take screenshots.
//   node tools/shot.mjs --build=build-all --script=tools/tests/desktop.mjs
export default async (t) => {
  await t.sleep(2500);
  t.shot('01-boot');
  await t.type('ls ~/unix_club\n');
  await t.sleep(400);
  await t.type('echo 2^100 | bc\n');
  await t.sleep(400);
  await t.type('seq 1 2000 | sort -rn | head -3\n');
  await t.sleep(600);
  t.shot('02-shell');
  await t.tap('Enter', { alt: true }); // second terminal
  await t.sleep(1200);
  await t.type('sysbench\n');
  await t.sleep(4000);
  t.shot('03-two-terminals');
  await t.tap('Enter', { alt: true }); // third terminal
  await t.sleep(1200);
  await t.type('htop\n');
  await t.sleep(1500);
  t.shot('04-htop');
  await t.tap('KeyD', { alt: true }); // launcher
  await t.sleep(800);
  await t.type('do');
  await t.sleep(400);
  t.shot('05-launcher');
  await t.tap('Escape');
  await t.sleep(300);
  await t.tap('KeyT', { alt: true }); // theme switch
  await t.sleep(1500);
  t.shot('06-brand-theme');
};
