// SPDX-License-Identifier: GPL-2.0-or-later
// A program that floods a terminal, in a real browser: the page must keep drawing frames
// and answering, Ctrl-C must stop the flood, and a second terminal must open and work
// while the first one floods. See tools/tests/desktop-flood.mjs for the cause.
//   make B=build-NAME dist && node tools/browser.mjs --build=build-NAME --script=tools/tests/browser-flood.mjs
let pass = 0, fail = 0;
const check = (name, cond, info = '') => {
  if (cond) pass++; else fail++;
  console.log(`${cond ? 'PASS' : 'FAIL'} ${name}${cond || !info ? '' : ' :: ' + info}`);
};
// the page's answer, or null when it does not come in time (a frozen page never answers)
const within = (p, ms) => Promise.race([p, new Promise((r) => setTimeout(() => r(null), ms))]);

export default async (t) => {
  const logHas = async (text, from, ms = 5000) => {
    for (const t0 = Date.now(); Date.now() - t0 < ms; await t.sleep(50)) if (t.log.slice(from).includes(text)) return true;
    return false;
  };
  await t.sleep(4000);
  await t.eval('window.floodFrames = 0; (function f() { window.floodFrames++; requestAnimationFrame(f); })(); 0');
  for (const cmd of ['yes', 'yes | cat']) {
    const tag = cmd.replace(/[^a-z]/g, '');
    const typed = await within(t.type(cmd + '\n').then(() => true), 5000); // a frozen page never takes the last key
    await t.sleep(500);
    const f0 = typed && await within(t.eval('window.floodFrames'), 3000);
    await t.sleep(1000);
    const f1 = typed && await within(t.eval('window.floodFrames'), 3000);
    check(`${cmd}: the page answers during the flood`, typed && f0 !== null && f1 !== null);
    check(`${cmd}: frames keep coming during the flood`, f1 - f0 >= 10, `${f1 - f0} frames in one second`);
    console.log(`INFO ${cmd}: ${f1 - f0} frames in one second of flood`);
    if (!typed || f1 === null) { console.log(`browser-flood: ${pass} passed, ${fail} failed`); throw new Error('the page is frozen'); }
    await t.shot(`bflood-${tag}`);
    const from = t.log.length;
    await t.tap('KeyC', { ctrl: true });
    await t.sleep(300);
    await t.type(`echo stopped-${tag} > /dev/console\n`);
    check(`${cmd}: Ctrl-C stops it and the shell answers`, await logHas(`stopped-${tag}`, from));
  }
  await t.type('yes\n');
  await t.sleep(500);
  const from = t.log.length;
  await t.tap('Enter', { alt: true });
  await t.sleep(2500);
  await t.type('echo second-terminal > /dev/console\n');
  check('a second terminal opens and works during the flood', await logHas('second-terminal', from));
  await t.shot('bflood-two-terminals');
  console.log(`browser-flood: ${pass} passed, ${fail} failed`);
  if (fail) throw new Error('browser-flood failed');
};
