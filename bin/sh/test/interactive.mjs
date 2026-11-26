// Interactive shell test on the real desktop: types into terminals, reads the
// terminal's text through its test hook (Ctrl+Alt+Shift+F12) and saves
// screenshots. Run:
//   make B=build-sh2 && node tools/shot.mjs --build=build-sh2 --quiet --script=bin/sh/test/interactive.mjs
// Prints "[test] ok NAME" or "[test] FAIL NAME" per check and a final count.
let pass = 0, fail = 0;
const results = [];
function check(name, cond, detail = '') {
  if (cond) pass++; else fail++;
  const line = `[test] ${cond ? 'ok  ' : 'FAIL'} ${name}${!cond && detail ? '  (' + detail + ')' : ''}`;
  results.push(line);
  console.error(line);
}

// the focused terminal's visible text, as an array of lines (trailing blanks cut)
async function screen(t) {
  const start = t.log.length;
  await t.tap('F12', { ctrl: true, alt: true, shift: true });
  for (let i = 0; i < 40 && !t.log.slice(start).includes('[screen-end]'); i++) await t.sleep(50);
  const s = t.log.slice(start);
  const a = s.lastIndexOf('[screen-begin]'), b = s.lastIndexOf('[screen-end]');
  if (a < 0 || b < 0) return { lines: [], cols: 0, rows: 0 };
  const head = s.slice(a, s.indexOf('\n', a));
  const m = head.match(/(\d+)x(\d+)/);
  const lines = s.slice(a, b).split('\n').slice(1).filter((l) => l.startsWith('[screen] ')).map((l) => l.slice(9).replace(/\s+$/, ''));
  return { lines, cols: m ? +m[1] : 0, rows: m ? +m[2] : 0 };
}
const text = (sc) => sc.lines.join('\n');
const nonEmpty = (sc) => sc.lines.filter((l) => l.length);
const lastLine = (sc) => { const n = nonEmpty(sc); return n.length ? n[n.length - 1] : ''; };
const PROMPT = 'user@42lisboa:';

async function run(t, cmd, wait = 600) { await t.type(cmd + '\n'); await t.sleep(wait); }
async function keys(t, list) { for (const k of list) { if (typeof k === 'string') await t.tap(k); else await t.tap(k[0], k[1]); await t.sleep(30); } }

export default async (t) => {
  await t.sleep(3000);
  // ---- a fresh terminal running sh directly ----
  await t.tap('Enter', { alt: true });
  await t.sleep(1500);
  let sc = await screen(t);
  check('prompt is drawn', lastLine(sc).startsWith(PROMPT + '~$'), lastLine(sc));
  t.shot('i01-prompt');

  await run(t, 'clear', 400);
  await t.type('echo wrld');
  await keys(t, ['ArrowLeft', 'ArrowLeft', 'ArrowLeft']);
  await t.type('o\n');
  await t.sleep(400);
  sc = await screen(t);
  check('Left arrow and insert', sc.lines.includes('world'), text(sc).slice(-200));

  await t.type('xyz echo abc');
  await keys(t, ['Home', 'Delete', 'Delete', 'Delete', 'Delete', 'End']);
  await t.type('d\n');
  await t.sleep(400);
  sc = await screen(t);
  check('Home, Delete, End', sc.lines.includes('abcd'));

  await t.type('echo abx');
  await keys(t, ['Backspace']);
  await t.type('c\n');
  await t.sleep(400);
  sc = await screen(t);
  check('Backspace', sc.lines.includes('abc'));

  await t.type('echo foo bar');
  await keys(t, [['KeyW', { ctrl: true }]]);
  await t.type('baz\n');
  await t.sleep(400);
  sc = await screen(t);
  check('Ctrl-W kills a word', sc.lines.includes('foo baz'));

  await t.type('garbage here');
  await keys(t, [['KeyU', { ctrl: true }]]);
  await t.type('echo cleared\n');
  await t.sleep(400);
  sc = await screen(t);
  check('Ctrl-U kills the line', sc.lines.includes('cleared') && !text(sc).includes('garbage hereecho'));

  await t.type('echo kept tail');
  await keys(t, [['KeyA', { ctrl: true }]]);
  for (let i = 0; i < 9; i++) await keys(t, ['ArrowRight']);
  await keys(t, [['KeyK', { ctrl: true }], ['KeyE', { ctrl: true }]]);
  await t.type('!\n');
  await t.sleep(400);
  sc = await screen(t);
  check('Ctrl-A, Ctrl-K, Ctrl-E', sc.lines.includes('kept!'), text(sc).slice(-200));
  t.shot('i02-editing');

  await run(t, 'clear', 400);
  await run(t, 'echo first-cmd', 300);
  await run(t, 'echo second-cmd', 300);
  await keys(t, ['ArrowUp', 'ArrowUp']);
  await t.sleep(200);
  sc = await screen(t);
  check('Up twice recalls an older command', lastLine(sc).endsWith('$ echo first-cmd'), lastLine(sc));
  await keys(t, ['ArrowDown']);
  await t.sleep(200);
  sc = await screen(t);
  check('Down goes forward again', lastLine(sc).endsWith('$ echo second-cmd'), lastLine(sc));
  await t.type('\n');
  await t.sleep(400);
  sc = await screen(t);
  check('recalled command runs', sc.lines.filter((l) => l === 'second-cmd').length === 2);

  await t.type('unam');
  await keys(t, ['Tab']);
  await t.sleep(300);
  sc = await screen(t);
  check('Tab completes a command', lastLine(sc).endsWith('$ uname'), lastLine(sc));
  await t.type('-s\n');
  await t.sleep(400);
  sc = await screen(t);
  check('completed command runs', sc.lines.includes('UC.OS'));
  await t.type('cat ~/unix_club/07_join_us/TO');
  await keys(t, ['Tab']);
  await t.sleep(300);
  sc = await screen(t);
  check('Tab completes a file name', lastLine(sc).endsWith('07_join_us/TODO.md'), lastLine(sc));
  await t.type('\n');
  await t.sleep(500);
  sc = await screen(t);
  check('completed file name works', text(sc).includes('# TODO.md'));
  await t.type('ls ~/unix_club/0');
  await keys(t, ['Tab']);
  await t.sleep(400);
  sc = await screen(t);
  check('Tab lists ambiguous names', text(sc).includes('01_what_is_unix/') && text(sc).includes('07_join_us/'));
  t.shot('i03-completion');
  await keys(t, [['KeyU', { ctrl: true }]]);

  await keys(t, [['KeyL', { ctrl: true }]]);
  await t.sleep(300);
  sc = await screen(t);
  check('Ctrl-L clears the screen', sc.lines[0].startsWith(PROMPT) && nonEmpty(sc).length === 1, sc.lines[0]);
  t.shot('i04-ctrl-l');

  await t.type('echo nope');
  await keys(t, [['KeyC', { ctrl: true }]]);
  await t.sleep(200);
  await run(t, 'echo yes', 400);
  sc = await screen(t);
  check('Ctrl-C at the prompt drops the line', sc.lines.includes('yes') && !sc.lines.includes('nope') && text(sc).includes('echo nope^C'));

  await run(t, 'clear', 300);
  await run(t, 'sleep 100', 800);
  await keys(t, [['KeyC', { ctrl: true }]]);
  await t.sleep(600);
  await run(t, 'echo status=$?', 400);
  sc = await screen(t);
  check('Ctrl-C kills sleep 100', sc.lines.includes('status=130'), text(sc).slice(-200));

  await run(t, 'while true; do :; done', 1500);
  sc = await screen(t);
  check('busy loop runs (no new prompt yet)', lastLine(sc).endsWith('do :; done'), lastLine(sc));
  await keys(t, [['KeyC', { ctrl: true }]]);
  await t.sleep(800);
  await run(t, 'echo loop=$?', 500);
  sc = await screen(t);
  check('Ctrl-C stops while true; do :; done', sc.lines.includes('loop=130'), text(sc).slice(-300));
  await run(t, 'for i in 1 2 3; do sleep 10; echo never$i; done', 800);
  await keys(t, [['KeyC', { ctrl: true }]]);
  await t.sleep(800);
  await run(t, 'echo for=$?', 500);
  sc = await screen(t);
  check('Ctrl-C on sleep inside a loop ends the loop', sc.lines.includes('for=130') && !sc.lines.some((l) => l.startsWith('never')), text(sc).slice(-300));
  await run(t, 'f() { while :; do :; done; }; f; echo not-reached', 1200);
  await keys(t, [['KeyC', { ctrl: true }]]);
  await t.sleep(800);
  await run(t, 'echo func=$?', 500);
  sc = await screen(t);
  check('Ctrl-C stops a looping function and the rest of the line', sc.lines.includes('func=130') && !sc.lines.includes('not-reached'));
  t.shot('i05-ctrl-c');

  await run(t, 'clear', 300);
  await run(t, 'seq 1 50000 | grep 7 | wc -l', 4000);
  sc = await screen(t);
  check('seq 1 50000 | grep 7 | wc -l', sc.lines.includes('17195'), text(sc).slice(-200));

  await run(t, 'clear; ls /bin', 800);
  sc = await screen(t);
  const lsLines = sc.lines.filter((l) => l.length && !l.startsWith(PROMPT));
  check('ls /bin uses several columns', lsLines.length > 0 && lsLines.length < 20 && lsLines[0].split(/\s+/).length > 3, `${lsLines.length} lines`);
  check('ls /bin fits the width', lsLines.every((l) => l.length <= sc.cols), `cols ${sc.cols}`);
  t.shot('i06-ls-wide');
  await run(t, 'clear; ls -la ~', 800);
  sc = await screen(t);
  check('ls -la shows dotfiles', text(sc).includes('.shrc') && /^d.* \.$/m.test(text(sc)), text(sc).slice(0, 300));
  t.shot('i07-ls-la');

  await run(t, 'clear; cat /usr/share/man/man1/sh.1', 1500);
  sc = await screen(t);
  check('cat of a long file scrolls to its end', text(sc).includes('UC.OS                          November 2026') && lastLine(sc).startsWith(PROMPT));
  t.shot('i08-cat-long');

  await run(t, 'man unix-club', 1200);
  sc = await screen(t);
  check('man shows the first page and a pager prompt', text(sc).includes('UNIX-CLUB(7)') && text(sc).includes('unix-club(7) line '));
  t.shot('i09-man-page1');
  await t.type(' ');
  await t.sleep(600);
  sc = await screen(t);
  check('space shows the next page', text(sc).includes('PILLARS') && !text(sc).includes('SYNOPSIS'));
  t.shot('i10-man-page2');
  await t.type('q');
  await t.sleep(600);
  await run(t, 'echo after-man', 400);
  sc = await screen(t);
  check('q leaves man, prompt works', sc.lines.includes('after-man'));

  // ---- three terminals tile; then a narrow screen ----
  await t.tap('Enter', { alt: true });
  await t.sleep(1500);
  await t.tap('Enter', { alt: true });
  await t.sleep(1500);
  t.shot('i11-three-terminals');
  t.resize(900, 720);
  await t.sleep(1500);
  await run(t, 'clear; ls /bin', 1000);
  sc = await screen(t);
  const narrow = sc.lines.filter((l) => l.length && !l.startsWith(PROMPT));
  check('narrow window: ls fits', sc.cols > 0 && narrow.every((l) => l.length <= sc.cols), `cols ${sc.cols}`);
  check('narrow window: fewer columns, more rows', narrow.length > lsLines.length, `${narrow.length} vs ${lsLines.length} lines, cols ${sc.cols}`);
  t.shot('i12-ls-narrow');

  // ---- Ctrl-D closes the window ----
  await run(t, 'ps | grep -c " term$"', 600);
  sc = await screen(t);
  const before = +nonEmpty(sc)[nonEmpty(sc).length - 2];
  await keys(t, [['KeyD', { ctrl: true }]]);
  await t.sleep(1500);
  await run(t, 'clear; ps | grep -c " term$"', 800);
  sc = await screen(t);
  const after = +nonEmpty(sc).filter((l) => /^\d+$/.test(l))[0];
  check('Ctrl-D on an empty line closes the terminal', before - after === 1, `${before} -> ${after}`);
  t.shot('i13-after-ctrl-d');

  console.error(results.join('\n'));
  console.error(`[test] interactive: ${pass + fail} checks, ${pass} passed, ${fail} failed`);
};
