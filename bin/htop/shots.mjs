// htop in the desktop at three widths (one, two, three terminals) and in both themes.
//   node tools/shot.mjs --build=build-apps2 --out=build-apps2/shots/htop --script=bin/htop/shots.mjs --quiet
export default async (t) => {
  await t.sleep(2500);
  await t.type('htop\n');
  await t.sleep(2500);
  t.shot('htop-1-deck');
  await t.tap('Enter', { alt: true });
  await t.sleep(1500);
  await t.type('htop\n');
  await t.sleep(2500);
  t.shot('htop-2-deck');
  await t.tap('Enter', { alt: true });
  await t.sleep(1500);
  await t.type('htop\n');
  await t.sleep(2500);
  t.shot('htop-3-deck');
  await t.tap('KeyT', { alt: true }); // theme switch: every htop must follow within a refresh
  await t.sleep(2500);
  t.shot('htop-3-brand');
  await t.type('q');
  await t.sleep(500);
  await t.tap('KeyQ', { alt: true, shift: true }); // close the third terminal: two remain
  await t.sleep(2500);
  t.shot('htop-2-brand');
  await t.tap('KeyQ', { alt: true, shift: true });
  await t.sleep(2500);
  t.shot('htop-1-brand');
  await t.tap('KeyT', { alt: true });
  await t.sleep(2500);
  t.shot('htop-1-deck-again');
};
