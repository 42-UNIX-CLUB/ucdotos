// sysbench in the desktop at three widths and both themes (the program is bin/sysbench.c;
// its shot script lives here because a single-file program has no directory of its own).
// After each layout change sysbench is rerun in every terminal, so no old full-width output
// is left behind in a window that got narrower.
//   node tools/shot.mjs --build=build-apps2 --out=build-apps2/shots/sysbench --script=bin/fetch/sysbench-shots.mjs --quiet
export default async (t) => {
  const everywhere = async (n, cmd) => {
    for (let i = 0; i < n; i++) { await t.type(cmd); await t.sleep(200); if (n > 1) { await t.tap('KeyK', { alt: true }); await t.sleep(300); } }
  };
  await t.sleep(2500);
  await everywhere(1, 'clear\nsysbench\n');
  await t.sleep(11000);
  t.shot('sysbench-1-deck');
  await t.tap('Enter', { alt: true });
  await t.sleep(1500);
  await everywhere(2, 'clear\nsysbench\n');
  await t.sleep(20000);
  t.shot('sysbench-2-deck');
  await t.tap('Enter', { alt: true });
  await t.sleep(1500);
  await everywhere(3, 'clear\nsysbench\n');
  await t.sleep(30000);
  t.shot('sysbench-3-deck');
  await t.tap('KeyT', { alt: true });
  await t.sleep(1000);
  await everywhere(3, 'clear\nsysbench\n');
  await t.sleep(30000);
  t.shot('sysbench-3-brand');
  await t.type('clear\nsysbench --clock-res 1000\n');
  await t.sleep(16000);
  t.shot('sysbench-3-brand-res1000');
  await t.tap('KeyQ', { alt: true, shift: true });
  await t.sleep(1500);
  await everywhere(2, 'clear\nsysbench --clock-res 100\n');
  await t.sleep(20000);
  t.shot('sysbench-2-brand-res100');
  await t.tap('KeyQ', { alt: true, shift: true });
  await t.sleep(1500);
  await everywhere(1, 'clear\nsysbench\n');
  await t.sleep(11000);
  t.shot('sysbench-1-brand');
};
