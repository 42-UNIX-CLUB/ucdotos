// SPDX-License-Identifier: GPL-2.0-or-later
// M4: Debian's bash and dash typed at interactively, in UC.OS's terminal emulator.
// Needs an image from bin/lx/tests/mkimage-debian.sh and the programs lx and term:
//   make B=build-lx PROGS="sh ls cat echo true lx term"
//   LX_SCRATCH=dir sh bin/lx/tests/mkimage-debian.sh build-lx
//   node tools/shot.mjs --build=build-lx --init=/bin/term --args="--fb -e /bin/lx /usr/bin/bash" --quiet --script=bin/lx/tests/os/interactive.mjs
// Prints "[test] ok NAME" / "[test] FAIL NAME" and a count.
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
async function run(t, cmd, wait = 1500) { await t.type(cmd + '\n'); await t.sleep(wait); }

export default async (t) => {
  await t.sleep(4000); // engine start, ld.so, bash start-up
  let s = await screen(t);
  check('bash prompt appears', /[$#] *$/m.test(s), s.split('\n').slice(-3).join(' | '));
  await run(t, 'export PATH=/usr/bin:/bin', 800);
  await run(t, 'echo $((6*7)) $BASH_VERSION');
  s = await screen(t);
  check('bash evaluates', /42 5\.2/.test(s), s.split('\n').slice(-3).join(' | '));
  await run(t, 'ls -d /usr/bin', 2500);
  s = await screen(t);
  check('runs coreutils ls (fork emulation + engine child)', /^\/usr\/bin$/m.test(s), s.split('\n').slice(-3).join(' | '));
  await run(t, 'for i in 1 2 3; do echo n$i; done | cat', 2500);
  s = await screen(t);
  check('loop into a pipe', /n1\nn2\nn3/.test(s), s.split('\n').slice(-4).join(' | '));
  await run(t, 'tput cols 2>/dev/null || stty size', 2000);
  await run(t, 'dash', 2500);
  await run(t, 'echo dash-$((1+1))', 1500);
  s = await screen(t);
  check('dash inside bash', /dash-2/.test(s), s.split('\n').slice(-3).join(' | '));
  await run(t, 'exit', 1500);
  await run(t, 'echo back-in-bash', 1500);
  s = await screen(t);
  check('back in bash after dash exits', /back-in-bash/.test(s), s.split('\n').slice(-3).join(' | '));
  await t.shot('lx-bash');
  console.error(`[test] interactive: ${pass + fail} checks, ${pass} passed, ${fail} failed`);
};
