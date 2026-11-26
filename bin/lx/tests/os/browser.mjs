// SPDX-License-Identifier: GPL-2.0-or-later
// The engine in real Chromium: Debian bash in UC.OS's terminal runs the M3/M4 script and
// the speed kernels, with their output sent to /dev/console (which the runner reads).
// Image from mkimage-debian.sh plus /debian/lx/bench (guest/bench.c), then
//   make B=build-lx PROGS="sh ls cat echo true lx term" dist
//   node tools/browser.mjs --build=build-lx --script=bin/lx/tests/os/browser.mjs \
//     --query="scale=1&cmdline=init%3D%2Fbin%2Fterm%20--%20--fb%20-e%20%2Fbin%2Flx%20%2Fusr%2Fbin%2Fbash"
let pass = 0, fail = 0;
function check(name, cond, detail = '') {
  if (cond) pass++; else fail++;
  console.error(`[test] ${cond ? 'ok  ' : 'FAIL'} ${name}${!cond && detail ? '  (' + detail + ')' : ''}`);
}
// Each write() reaches the runner as its own console line: match on the joined text.
const joined = (t) => t.log.replace(/\n/g, '');
export default async (t) => {
  await t.sleep(12000);
  await t.shot('lx-chromium-start');
  await t.type('export PATH=/usr/bin:/bin; sh /lx/m34.sh > /dev/console 2>&1; for k in sieve sort fib sha256; do /lx/bench $k 3; done > /dev/console 2>&1\n');
  for (let i = 0; i < 300 && !/sha256 best_us \d+ first_us \d+ check [0-9a-f]{16}/.test(joined(t)); i++) await t.sleep(500);
  const m = joined(t).match(/m34: (\d+) checks, (\d+) passed, (\d+) failed/);
  check('M3/M4 script in Chromium', m && m[3] === '0', m ? m[0] : 'no result');
  for (const k of ['sieve', 'sort', 'fib', 'sha256']) {
    const line = (joined(t).match(new RegExp(`${k} best_us \\d+ first_us \\d+ check [0-9a-f]{16}`)) || [''])[0];
    check(`bench ${k} ran`, !!line);
    if (line) console.error(`[bench] ${line}`);
  }
  for (const l of t.log.split('\n')) if (/FAIL|bench|lx:|not found|trap|crash|memory|signal|killed|out of bounds|unreachable/i.test(l)) console.error('[log] ' + l);
  await t.shot('lx-chromium');
  console.error(`[test] chromium: ${pass + fail} checks, ${pass} passed, ${fail} failed`);
};
