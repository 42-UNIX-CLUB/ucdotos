// blueprint in the desktop: the club deck, moving around, a tall slide, a theme switch, two windows.
//   node tools/shot.mjs --build=build-blueprint --out=build-blueprint/shots/blueprint --script=bin/blueprint/shots.mjs --quiet
export default async (t) => {
  await t.sleep(2500);
  await t.type('blueprint\n');
  await t.sleep(1500);
  t.shot('01-readme-top');
  await t.type(' ');
  await t.sleep(600);
  t.shot('02-readme-scrolled');
  await t.type(' ');
  await t.sleep(600);
  t.shot('03-history');
  await t.type('13G');
  await t.sleep(600);
  t.shot('04-kernel-vs-os');
  await t.type('4G');
  await t.sleep(600);
  t.shot('05-bsd');
  await t.type('G');
  await t.sleep(600);
  t.shot('06-questions-last');
  await t.type('gg');
  await t.sleep(600);
  t.shot('07-first-again');
  await t.type('17G');
  await t.tap('KeyT', { alt: true });
  await t.sleep(2000);
  t.shot('08-anatomy-brand');
  await t.tap('Enter', { alt: true }); // a second terminal: blueprint gets half the width
  await t.sleep(2500);
  t.shot('09-two-windows');
  await t.tap('KeyT', { alt: true });
  await t.sleep(2000);
  await t.tap('KeyQ', { alt: true, shift: true });
  await t.sleep(2500);
  t.shot('10-one-window-deck');
  await t.type('q');
  await t.sleep(800);
  t.shot('11-after-quit');
};
