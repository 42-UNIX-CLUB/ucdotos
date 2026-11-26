// SPDX-License-Identifier: GPL-2.0-or-later
// tools/browser.mjs script: boot phases of the page, in milliseconds since navigation, read
// from window.ucosBoot (boot/page.mjs): script start, kernel and image fetched, kernel up
// (kmain returned), first frame on the canvas. Also the transfer size of each file fetched.
// Prints one line: RESULT {...}. Run it on a fresh browser profile each time (browser.mjs does).
export default async (t) => {
  const t0 = Date.now();
  while (!(await t.eval('!!(window.ucosBoot && window.ucosBoot.firstFrame)'))) {
    if (Date.now() - t0 > 60000) throw new Error('no first frame after 60 s');
    await t.sleep(50);
  }
  const r = await t.eval(`(() => {
    const b = window.ucosBoot, res = {};
    for (const e of performance.getEntriesByType('resource')) {
      const n = e.name.split('/').pop().split('?')[0];
      if (/kernel|initrd/.test(n)) res[n] = { transfer: e.transferSize, body: e.encodedBodySize, ms: e.responseEnd - e.startTime };
    }
    return { ...b, resources: res };
  })()`);
  process.stdout.write('RESULT ' + JSON.stringify(r) + '\n');
};
