// SPDX-License-Identifier: GPL-2.0-or-later
// M2: Debian's busybox vi in UC.OS's terminal emulator. Image as for interactive.mjs:
//   node tools/shot.mjs --build=build-lx --init=/bin/term --args="--fb -e /bin/lx /usr/bin/busybox sh" --quiet --script=bin/lx/tests/os/vi.mjs
let pass = 0, fail = 0;
function check(name, cond, detail = '') {
  if (cond) pass++; else fail++;
  console.error(`[test] ${cond ? 'ok  ' : 'FAIL'} ${name}${!cond && detail ? '  (' + detail + ')' : ''}`);
}
async function screen(t) {
  const start = t.log.length;
  await t.tap('F12', { ctrl: true, alt: true, shift: true });
  for (let i = 0; i < 40 && !t.log.slice(start).includes('[screen-end]'); i++) await t.sleep(50);
  const s = t.log.slice(start);
  const a = s.lastIndexOf('[screen-begin]'), b = s.lastIndexOf('[screen-end]');
  if (a < 0 || b < 0) return '';
  return s.slice(a, b).split('\n').slice(1).filter((l) => l.startsWith('[screen] ')).map((l) => l.slice(9).replace(/\s+$/, '')).join('\n');
}
export default async (t) => {
  await t.sleep(2500);
  await t.type('/usr/bin/busybox vi /tmp/lx-vi.txt\n');
  await t.sleep(2500);
  await t.type('ihello from vi\nsecond line');
  await t.tap('Escape');
  await t.sleep(500);
  let s = await screen(t);
  check('vi shows the typed text', /hello from vi\nsecond line/.test(s), s.split('\n').slice(0, 3).join(' | '));
  check('vi draws its empty-line markers', /^~/m.test(s));
  await t.type('ggdd:wq\n');
  await t.sleep(2000);
  await t.type('/usr/bin/busybox cat /tmp/lx-vi.txt\n');
  await t.sleep(2000);
  s = await screen(t);
  check('file saved by vi, first line deleted', /^second line$/m.test(s) && !/^hello from vi$/m.test(s), s.split('\n').slice(-4).join(' | '));
  await t.shot('lx-vi');
  console.error(`[test] vi: ${pass + fail} checks, ${pass} passed, ${fail} failed`);
};
