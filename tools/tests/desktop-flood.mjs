// SPDX-License-Identifier: GPL-2.0-or-later
// A program that floods a terminal must not freeze the machine. `yes` and the terminal only
// wake each other, and neither runs long enough for the kernel's time slice, so the host
// has to give the event loop a turn by itself (boot/host.mjs, turnAt). Without that no
// timer fires, no key is read and nothing is drawn.
// Checks: timers keep firing during the flood, Ctrl-C still stops it, and a second
// terminal opens and works while the first one floods.
//   node tools/shot.mjs --build=build-NAME --script=tools/tests/desktop-flood.mjs
import { Worker } from 'node:worker_threads';

let pass = 0, fail = 0;
const check = (name, cond, info = '') => {
  if (cond) pass++; else fail++;
  console.log(`${cond ? 'PASS' : 'FAIL'} ${name}${cond || !info ? '' : ' :: ' + info}`);
};

// A frozen event loop cannot report itself: a second thread watches a counter that a timer
// on this one keeps moving, and ends the run when it stands still.
const beat = new Int32Array(new SharedArrayBuffer(4));
setInterval(() => Atomics.add(beat, 0, 1), 200).unref();
new Worker(`
  const { workerData: beat } = require('node:worker_threads');
  let last = -1;
  setInterval(() => {
    const now = Atomics.load(beat, 0);
    if (now === last) {
      require('node:fs').writeSync(1, 'FAIL the event loop had no turn for 10 s: the flood froze the machine\\ndesktop-flood: 1 failed\\n');
      process.kill(process.pid, 'SIGKILL');
    }
    last = now;
  }, 10000);`, { eval: true, workerData: beat }).unref();

export default async (t) => {
  const logHas = async (text, from, ms = 5000) => {
    for (const t0 = Date.now(); Date.now() - t0 < ms; await t.sleep(50)) if (t.log.slice(from).includes(text)) return true;
    return false;
  };
  await t.sleep(2500); // the desktop and its first terminal
  for (const cmd of ['yes', 'yes | cat']) {
    const tag = cmd.replace(/[^a-z]/g, '');
    await t.type(cmd + '\n');
    const t0 = Date.now();
    for (let i = 0; i < 10; i++) await t.sleep(100);
    const ms = Date.now() - t0;
    check(`${cmd}: timers keep firing during the flood`, ms < 3000, `10 sleeps of 100 ms took ${ms} ms`);
    console.log(`INFO ${cmd}: 10 sleeps of 100 ms took ${ms} ms`);
    t.shot(`flood-${tag}`);
    const from = t.log.length;
    await t.tap('KeyC', { ctrl: true });
    await t.sleep(300);
    await t.type(`echo stopped-${tag} > /dev/console\n`);
    check(`${cmd}: Ctrl-C stops it and the shell answers`, await logHas(`stopped-${tag}`, from));
  }
  // a second terminal while the first one floods
  await t.type('yes\n');
  await t.sleep(500);
  const from = t.log.length;
  await t.tap('Enter', { alt: true });
  await t.sleep(1500);
  await t.type('echo second-terminal > /dev/console\n');
  check('a second terminal opens and works during the flood', await logHas('second-terminal', from));
  t.shot('flood-two-terminals');
  console.log(`desktop-flood: ${pass} passed, ${fail} failed`);
  if (fail) process.exit(1);
};
