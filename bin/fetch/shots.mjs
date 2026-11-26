// fetch in the desktop at three widths (one, two, three terminals) and in both themes,
// plus fetch -a following a live theme switch.
//   node tools/shot.mjs --build=build-apps2 --out=build-apps2/shots/fetch --script=bin/fetch/shots.mjs --quiet
export default async (t) => {
  const run = async (cmd, ms) => { await t.type(cmd); await t.sleep(ms); };
  await t.sleep(2500);
  await run('clear\nfetch\n', 1500);
  t.shot('fetch-1-deck');
  await t.tap('Enter', { alt: true });
  await t.sleep(1500);
  await run('fetch\n', 1200);
  await t.tap('KeyK', { alt: true }); // back to the first terminal, now half width
  await t.sleep(300);
  await run('clear\nfetch\n', 1500);
  t.shot('fetch-2-deck');
  await t.tap('Enter', { alt: true });
  await t.sleep(1500);
  await run('fetch\n', 1200);
  await t.tap('KeyK', { alt: true });
  await t.sleep(300);
  await run('clear\nfetch\n', 1500);
  t.shot('fetch-3-deck');
  await t.tap('KeyT', { alt: true });
  await t.sleep(1500);
  for (let i = 0; i < 3; i++) { // rerun in every terminal under the new theme
    await run('clear\nfetch\n', 1500);
    await t.tap('KeyK', { alt: true });
    await t.sleep(300);
  }
  t.shot('fetch-3-brand');
  await t.tap('KeyQ', { alt: true, shift: true });
  await t.sleep(1500);
  for (let i = 0; i < 2; i++) { await run('clear\nfetch\n', 1500); await t.tap('KeyK', { alt: true }); await t.sleep(300); }
  t.shot('fetch-2-brand');
  await t.tap('KeyQ', { alt: true, shift: true });
  await t.sleep(1500);
  await run('clear\nfetch -a\n', 2500);
  t.shot('fetch-1-brand-animated');
  await t.tap('KeyT', { alt: true }); // fetch -a re-reads the theme once a second
  await t.sleep(2500);
  t.shot('fetch-1-deck-animated-after-switch');
  await t.type('q');
  await t.sleep(500);
};
