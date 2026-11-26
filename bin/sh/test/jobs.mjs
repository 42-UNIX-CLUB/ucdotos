// SPDX-License-Identifier: GPL-2.0-or-later
// Job control in a terminal of the desktop: Ctrl-Z, jobs, bg, fg, Ctrl-C,
// and the "Done" notice. Run:
//   node tools/shot.mjs --build=build-NAME --quiet --script=bin/sh/test/jobs.mjs
// With a kernel that can stop processes (set -m is on) the real thing is
// checked; without one, the script checks that the shell says job control is
// unavailable and that Ctrl-Z does not hang anything.
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
  return s.slice(s.lastIndexOf('[screen-begin]'), s.lastIndexOf('[screen-end]')).split('\n')
    .filter((l) => l.startsWith('[screen] ')).map((l) => l.slice(9).replace(/\s+$/, ''));
}
const has = (l, re) => l.some((x) => re.test(x));
async function run(t, cmd, wait = 600) { await t.type(cmd + '\n'); await t.sleep(wait); }

export default async (t) => {
  await t.sleep(3000);
  await t.tap('Enter', { alt: true }); // a fresh terminal running sh
  await t.sleep(1500);
  await run(t, 'clear', 400);
  await run(t, 'set -o | grep monitor');
  let l = await screen(t);
  const jc = has(l, /^monitor\s+on/);
  console.error(`[test] job control ${jc ? 'available' : 'not available'} on this kernel`);
  if (!jc) {
    await run(t, 'fg');
    l = await screen(t);
    check('fg says there is no job control', has(l, /fg: no job control/), l.join('|').slice(-200));
    await run(t, 'sleep 100', 800);
    await t.tap('KeyZ', { ctrl: true });
    await t.sleep(600);
    await t.tap('KeyC', { ctrl: true });
    await t.sleep(800);
    await run(t, 'echo after=$?');
    l = await screen(t);
    check('Ctrl-Z hangs nothing, Ctrl-C still ends the program', has(l, /^after=130$/), l.join('|').slice(-300));
    t.shot('j01-no-job-control');
    console.error(`[test] jobs: ${pass + fail} checks, ${pass} passed, ${fail} failed`);
    return;
  }
  await run(t, 'clear', 400);
  await run(t, 'sleep 100', 800);
  await t.tap('KeyZ', { ctrl: true });
  await t.sleep(800);
  await run(t, 'echo z=$?');
  l = await screen(t);
  check('Ctrl-Z stops the job and prints it', has(l, /^\[1\]\+\s+Stopped\s+sleep 100$/), l.join('|').slice(-300));
  check('status of a stopped job is 148', has(l, /^z=148$/), l.join('|').slice(-300));
  await run(t, 'jobs');
  l = await screen(t);
  check('jobs lists it as Stopped', l.filter((x) => /^\[1\]\+\s+Stopped\s+sleep 100$/.test(x)).length >= 2, l.join('|').slice(-300));
  t.shot('j01-stopped');
  await run(t, 'bg');
  await run(t, 'jobs');
  l = await screen(t);
  check('bg resumes it in the background', has(l, /^\[1\] sleep 100 &$/) && has(l, /^\[1\]\+\s+Running\s+sleep 100$/), l.join('|').slice(-300));
  await run(t, 'fg', 800);
  l = await screen(t);
  check('fg prints the job', has(l, /^sleep 100$/), l.join('|').slice(-300));
  await t.tap('KeyC', { ctrl: true });
  await t.sleep(800);
  await run(t, 'echo c=$?; jobs; echo jobs-end');
  l = await screen(t);
  check('Ctrl-C ends the job brought back with fg', has(l, /^c=130$/), l.join('|').slice(-300));
  const e1 = l.lastIndexOf('jobs-end');
  check('no job left', e1 > 0 && l[e1 - 1] === 'c=130', l.join('|').slice(-300));
  t.shot('j02-fg-ctrl-c');
  await run(t, 'clear', 400);
  await run(t, 'sleep 1 &', 400);
  await t.sleep(1800);
  await run(t, '', 600);
  l = await screen(t);
  check('a finished background job is reported as Done', has(l, /^\[1\]\+\s+Done\s+sleep 1$/), l.join('|').slice(-300));
  await run(t, 'sleep 100 &', 400);
  await run(t, 'kill -STOP %1', 600);
  await run(t, '', 600);
  l = await screen(t);
  check('a background job stopped by a signal is reported', has(l, /^\[1\]\+\s+Stopped\s+sleep 100$/), l.join('|').slice(-300));
  await run(t, 'kill %1', 800);
  await run(t, '', 600);
  await run(t, 'jobs; echo jobs-end2');
  l = await screen(t);
  const e2 = l.lastIndexOf('jobs-end2');
  check('kill %1 ends a stopped job', e2 > 0 && l[e2 - 1].endsWith('jobs; echo jobs-end2'), l.join('|').slice(-300));
  t.shot('j03-notices');
  console.error(`[test] jobs: ${pass + fail} checks, ${pass} passed, ${fail} failed`);
};
