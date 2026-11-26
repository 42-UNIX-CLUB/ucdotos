// Ctrl-C in the first terminal, where sh is started by "sh /etc/welcome"
// (so the interactive sh is not the terminal's first process). Run:
//   node tools/shot.mjs --build=build-sh2 --quiet --script=bin/sh/test/welcome.mjs
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
export default async (t) => {
  await t.sleep(3000);
  await t.type('clear\n');
  await t.sleep(400);
  await t.type('sleep 100\n');
  await t.sleep(800);
  await t.tap('KeyC', { ctrl: true });
  await t.sleep(600);
  await t.type('echo s=$?\n');
  await t.sleep(400);
  await t.type('while true; do :; done\n');
  await t.sleep(1200);
  await t.tap('KeyC', { ctrl: true });
  await t.sleep(800);
  await t.type('echo w=$?\n');
  await t.sleep(500);
  const l = await screen(t);
  check('first terminal: Ctrl-C kills sleep 100', l.includes('s=130'), l.join('|').slice(-300));
  check('first terminal: Ctrl-C stops a busy loop', l.includes('w=130'), l.join('|').slice(-300));
  t.shot('w01-first-terminal-ctrl-c');
  console.error(`[test] welcome: ${pass + fail} checks, ${pass} passed, ${fail} failed`);
};
