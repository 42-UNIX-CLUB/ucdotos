// SPDX-License-Identifier: GPL-2.0-or-later
// tools/browser.mjs script: wait until every process has exited (the page prints
// "System halted"), then write the page's boot log to stdout. Used by tools/bench/ab.mjs
// to collect a benchmark's output from real Chromium.
export default async (t) => {
  const text = () => t.eval('document.getElementById("log").textContent');
  const t0 = Date.now();
  while (!(await text()).includes('System halted')) {
    if (Date.now() - t0 > 300000) throw new Error('timed out');
    await t.sleep(250);
  }
  process.stdout.write(await text());
};
