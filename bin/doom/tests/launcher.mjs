// SPDX-License-Identifier: GPL-2.0-or-later
// Doom from the launcher on the desktop, with the game data fetched on first use.
//   make B=build-doom WADS=... ; node tools/shot.mjs --build=build-doom --remote-rate=600000 \
//     --script=bin/doom/tests/launcher.mjs
// --remote-rate slows the transfer so the progress bar can be seen. Checks: nothing is
// requested before Doom starts; one request for the data on the first start; none on the
// second start; the window closes cleanly and the desktop comes back.
const NAME = process.env.DOOM_NAME || 'doom';              // what to type in the launcher
const WAD = process.env.DOOM_WAD || 'doom1.wad';
const requests = (t) => (t.log.match(new RegExp(`request \\d+ for files/${WAD.replace('.', '\\.')}`, 'g')) || []).length;
const until = async (t, re, ms) => { for (let i = 0; i < ms / 50 && !re.test(t.log); i++) await t.sleep(50); return re.test(t.log); };
const check = (name, ok) => console.error(`[test] ${ok ? 'ok  ' : 'FAIL'} ${name}`);
export default async (t) => {
  const p = process.env.SHOT_PREFIX || 'launcher-';
  await t.sleep(3000);
  t.shot(p + '1-desktop');
  check('no game data requested before Doom starts', requests(t) === 0 && !/\[remote\] fetching/.test(t.log));
  await t.tap('KeyD', { alt: true });
  await t.sleep(800);
  await t.type(NAME);
  await t.sleep(400);
  t.shot(p + '2-launcher');
  const t0 = Date.now(), m0 = t.sys.memory.buffer.byteLength;
  await t.tap('Enter');
  check('the first start fetches the data', await until(t, new RegExp(`doom: fetching ${WAD}`), 10000));
  await t.sleep(700);
  t.shot(p + '3-progress');
  const arrived = await until(t, new RegExp(`doom: ${WAD} arrived`), 120000);
  check('the data arrived', arrived);
  console.error(`[test] kernel memory: ${(m0 / 1048576).toFixed(1)} MiB before, ${(t.sys.memory.buffer.byteLength / 1048576).toFixed(1)} MiB after the data arrived`);
  console.error(`[test] first start: data in ${((Date.now() - t0) / 1000).toFixed(1)} s (wall clock, throttled)`);
  await t.sleep(3500);
  t.shot(p + '4-title');
  await t.tap('Escape'); await t.sleep(400);
  await t.tap('Enter'); await t.sleep(400);
  await t.tap('Enter'); await t.sleep(400);
  await t.tap('Enter'); await t.sleep(2500);
  t.key('ArrowUp', true); await t.sleep(900); t.key('ArrowUp', false);
  t.key('ControlLeft', true); await t.sleep(250);
  t.shot(p + '5-play');
  t.key('ControlLeft', false); await t.sleep(200);
  check('one request for the data', requests(t) === 1);
  await t.tap('KeyQ', { alt: true, shift: true }); // the window manager's kill binding
  await t.sleep(1500);
  t.shot(p + '6-closed');
  // second start: the file is in kernel memory now
  await t.tap('KeyD', { alt: true });
  await t.sleep(800);
  await t.type(NAME);
  await t.sleep(300);
  await t.tap('Enter');
  await t.sleep(3500);
  t.shot(p + '7-second-start');
  check('the second start fetches nothing', requests(t) === 1 && (t.log.match(new RegExp(`doom: fetching ${WAD}`, 'g')) || []).length === 1);
  await t.tap('KeyQ', { alt: true, shift: true });
  await t.sleep(1000);
};
