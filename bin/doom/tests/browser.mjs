// SPDX-License-Identifier: GPL-2.0-or-later
// Doom in real Chromium, with the game data fetched on first use. Run `make B=... dist` first.
//   DOOM_MODE=cold|throttle|offline|freedoom|import node tools/browser.mjs --build=B --script=bin/doom/tests/browser.mjs
//   cold      first start (fetch) and second start (in memory): requests, bytes, times, kernel memory
//   throttle  the same on a slow connection (DOOM_KBPS, default 400 kB/s), screenshot of the progress bar
//   offline   the network goes away before Doom starts: the error message
//   freedoom  the Freedoom launcher entry
//   import    Ctrl+Alt+O opens the file chooser (a real key press), doom1.wad from disk goes into
//             ~/Downloads, and a dropped file goes there too; Doom then plays the imported file
const MODE = process.env.DOOM_MODE || 'cold';
const WAD = MODE === 'freedoom' ? 'freedoom1.wad' : 'doom1.wad';
const check = (name, ok) => console.error(`[test] ${ok ? 'ok  ' : 'FAIL'} ${name}`);
const data = (t) => t.requests.filter((r) => /\/files\//.test(r.url));
const until = async (t, re, ms) => { const t0 = Date.now(); while (Date.now() - t0 < ms && !re.test(t.log)) await t.sleep(25); return re.test(t.log) ? Date.now() - t0 : -1; };
const kmem = (t) => t.eval('ucos.memory.buffer.byteLength');
const launch = async (t, name) => { await t.tap('KeyD', { alt: true }); await t.sleep(700); await t.type(name); await t.sleep(300); await t.tap('Enter'); };
const MB = (n) => (n / 1048576).toFixed(1) + ' MiB';
export default async (t) => {
  const p = `browser-${MODE}-`;
  while (!(await t.eval('!!(window.ucosBoot && window.ucosBoot.firstFrame)'))) await t.sleep(100);
  await t.sleep(2500);
  await t.shot(p + '1-desktop');
  check('no game data requested before Doom starts', data(t).length === 0);
  const m0 = await kmem(t);
  if (MODE === 'throttle') await t.send('Network.emulateNetworkConditions', { offline: false, latency: 40, downloadThroughput: Number(process.env.DOOM_KBPS || 400) * 1000, uploadThroughput: 100000 });
  if (MODE === 'offline') await t.send('Network.emulateNetworkConditions', { offline: true, latency: 0, downloadThroughput: -1, uploadThroughput: -1 });
  if (MODE === 'import') {
    // the page's own key chord: a real key press, so the browser lets it open the chooser
    await t.send('Page.setInterceptFileChooserDialog', { enabled: true });
    const opened = new Promise((r) => t.on('Page.fileChooserOpened', r));
    await t.tap('KeyO', { ctrl: true, alt: true });
    const ev = await Promise.race([opened, t.sleep(3000).then(() => null)]);
    check('Ctrl+Alt+O opens the file chooser', !!ev);
    if (ev) await t.send('DOM.setFileInputFiles', { backendNodeId: ev.backendNodeId, files: [process.env.DOOM_FILE || '/home/ensui/uc-os/bin/doom/wads/doom1.wad'] });
    check('the chosen file is copied in', (await until(t, /\[page\] import doom1\.wad: 4196020 bytes, done/, 10000)) >= 0);
    await t.shot(p + '2-imported');
    // a drop: the page's handler, fed a DataTransfer made in the page (DevTools cannot drop a real file headless)
    await t.eval(`(() => { const d = new DataTransfer(); d.items.add(new File(['dropped\\n'], 'note.txt')); dispatchEvent(new DragEvent('drop', { dataTransfer: d, cancelable: true })); })()`);
    check('a dropped file is copied in', (await until(t, /\[page\] import note\.txt: 8 bytes, done/, 5000)) >= 0);
    await t.sleep(300);
  }
  // JavaScript heap while the data arrives: shows whether the host keeps a copy of the file
  const heap0 = (await t.send('Runtime.getHeapUsage')).usedSize;
  let heapMax = heap0, sampling = true;
  (async () => { while (sampling) { try { heapMax = Math.max(heapMax, (await t.send('Runtime.getHeapUsage')).usedSize); } catch {} await t.sleep(20); } })();
  await launch(t, MODE === 'freedoom' ? 'freedoom' : 'doom');
  const t0 = Date.now(); // from the Enter key in the launcher
  if (MODE === 'offline') {
    await until(t, /Could not fetch/, 15000);
    await t.sleep(1000);
    await t.shot(p + '2-message');
    check('offline: Doom says it could not fetch the data', /Could not fetch the game data/.test(t.log));
    return;
  }
  if (MODE === 'throttle') { await until(t, /doom: fetching/, 10000); await t.sleep(1500); await t.shot(p + '2-progress'); }
  const toData = MODE === 'import' ? 0 : await until(t, new RegExp(`doom: ${WAD.replace('.', '\\.')} arrived`), 300000);
  sampling = false;
  const toTitle = await until(t, /I_InitGraphics: DOOM screen size/, 300000);
  console.error(`[test] JS heap: ${MB(heap0)} before, at most ${MB(heapMax)} while the data arrived`);
  await t.sleep(1500);
  await t.shot(p + '3-title');
  const m1 = await kmem(t);
  const req = data(t);
  console.error(`[test] first start: data after ${toData} ms, title after ${Date.now() - t0 - 1500} ms (I_InitGraphics at ${toTitle} ms)`);
  console.error(`[test] requests: ${req.map((r) => `${r.url.replace(/.*\/files\//, 'files/')} ${r.status} ${r.bytes} bytes`).join(', ') || 'none'}`);
  console.error(`[test] kernel memory: ${MB(m0)} before, ${MB(m1)} after`);
  if (MODE === 'import') check('Doom plays the imported file, nothing fetched', req.length === 0 && /adding \/home\/user\/Downloads\/doom1\.wad/.test(t.log));
  else check('one request for the data', req.length === 1 && req[0].status === 200);
  if (MODE === 'freedoom') check('the Freedoom notice is printed', /You are playing using one of the Freedoom IWAD/.test(t.log));
  // play a little, close, start again
  await t.tap('Escape'); await t.sleep(400);
  await t.tap('Enter'); await t.sleep(400);
  await t.tap('Enter'); await t.sleep(400);
  await t.tap('Enter'); await t.sleep(2500);
  await t.key('ArrowUp', true); await t.sleep(800); await t.key('ArrowUp', false);
  await t.shot(p + '4-play');
  await t.tap('KeyQ', { alt: true, shift: true });
  await t.sleep(1500);
  await t.shot(p + '5-closed');
  const n = (t.log.match(/I_InitGraphics: DOOM screen size/g) || []).length;
  await launch(t, MODE === 'freedoom' ? 'freedoom' : 'doom');
  const t1 = Date.now();
  for (let i = 0; i < 600 && (t.log.match(/I_InitGraphics: DOOM screen size/g) || []).length === n; i++) await t.sleep(25);
  console.error(`[test] second start: title after ${Date.now() - t1} ms (to I_InitGraphics)`);
  check('the second start fetches nothing', data(t).length === req.length);
  await t.sleep(1500);
  await t.shot(p + '6-second-start');
  await t.tap('KeyQ', { alt: true, shift: true });
  await t.sleep(500);
};
