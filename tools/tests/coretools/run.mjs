// SPDX-License-Identifier: GPL-2.0-or-later
// Differential tests for the core command-line tools (bin/*.c), run natively.
//
//   node tools/tests/coretools/run.mjs              build natively and check every case file
//   node tools/tests/coretools/run.mjs tr sed       only these case files
//   node tools/tests/coretools/run.mjs --regen tr   rewrite tr's expectations from the host's tools
//   node tools/tests/coretools/run.mjs --emit-os    write the in-OS test script (rootfs/usr/share/tests/coretools.sh)
//   options: --no-san (no sanitizers), --keep-going, --verbose, --cc=CC
//
// Each case file cases/NAME.t has a %setup section (shell lines run in a fresh
// directory before every case) and a %cases section: one shell command per
// line; a line starting with "> " continues the previous case after a newline.
// A case line starting with "@noos " is not run inside UC.OS (the reason
// should be in a comment above it). Cases run in one shared directory where
// the setup ran once; a case that writes files must start with "@fresh " and
// then gets a directory of its own (the runner checks that unmarked cases
// leave the shared directory as they found it). Expectations live in cases/NAME.expected
// (JSON lines: cmd, out, status, err) so the suite runs without the host's
// tools. cases/regex.cases and cases/fnmatch.cases are special: they feed the
// C drivers retest.c and fntest.c.
//
// The runner never deletes anything: every run works in a new directory under
// build-native/work/ and prints its path. Sharing the setup keeps a full run
// to a few megabytes (one directory per case left 868 MB in /tmp after 29 runs).
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../../..');
const args = process.argv.slice(2);
const opt = (n) => args.includes(n);
const optv = (n, d) => { const a = args.find((x) => x.startsWith(n + '=')); return a ? a.slice(n.length + 1) : d; };
const only = args.filter((a) => !a.startsWith('--'));
const regen = opt('--regen'), verbose = opt('--verbose'), san = !opt('--no-san');
const CC = optv('--cc', 'cc');
const out = path.join(root, optv('--out', 'build-native'));
const bindir = path.join(out, 'bin');
const incdir = path.join(out, 'inc');
fs.mkdirSync(bindir, { recursive: true });
/* compilers write temporary files to TMPDIR: keep them off /tmp, which is a RAM disk on the owner's machine */
fs.mkdirSync(path.join(out, 'tmp'), { recursive: true });
process.env.TMPDIR = path.join(out, 'tmp');
fs.mkdirSync(incdir, { recursive: true });

// Tools built natively, and the extra C library files each needs (our own
// implementations replace the host's at link time, so they are what is tested).
const LIBEXTRA = { regex: 'libc/src/regex.c', fnmatch: 'libc/src/fnmatch.c' };
const TOOLS = {
  tr: [], cut: [], sed: ['regex'], basename: [], dirname: [], realpath: [], mktemp: [], cmp: [],
  expr: ['regex'], xargs: [], find: ['fnmatch'], paste: [], nl: ['regex'], fold: [], comm: [], join: [],
  diff: [], od: [], id: [], whoami: [], hostname: [], tty: [], nproc: [], du: [], df: [], sync: [],
  install: [], awk: ['regex'],
};
for (const h of ['regex.h', 'fnmatch.h']) {
  const src = path.join(root, 'libc/include', h);
  if (fs.existsSync(src)) fs.copyFileSync(src, path.join(incdir, h));
}

function build(name) {
  const src = path.join(root, 'bin', name + '.c');
  if (!fs.existsSync(src)) return false;
  const extra = (TOOLS[name] || []).map((k) => path.join(root, LIBEXTRA[k])).filter((f) => fs.existsSync(f));
  const outf = path.join(bindir, name);
  const deps = [src, ...extra, ...fs.readdirSync(incdir).map((f) => path.join(incdir, f))];
  if (fs.existsSync(outf)) {
    const t = fs.statSync(outf).mtimeMs;
    if (deps.every((d) => fs.statSync(d).mtimeMs < t)) return true;
  }
  const flags = ['-O1', '-g', '-Wall', '-Wextra', '-Wno-unused-parameter', '-D_GNU_SOURCE', '-I' + incdir];
  if (san) flags.push('-fsanitize=address,undefined', '-fno-sanitize-recover=undefined');
  const r = spawnSync('nice', ['-n', '19', CC, ...flags, '-o', outf, src, ...extra, '-lm'], { encoding: 'utf8' });
  if (r.status !== 0) { process.stdout.write(`build ${name}: FAILED\n${r.stderr}`); return false; }
  if (r.stderr && verbose) process.stdout.write(r.stderr);
  return true;
}

function parseCases(file) {
  const lines = fs.readFileSync(file, 'utf8').split('\n');
  const setup = [], cases = [];
  let mode = '';
  for (const l of lines) {
    if (l === '%setup') { mode = 's'; continue; }
    if (l === '%cases') { mode = 'c'; continue; }
    if (mode === 's') { if (l.trim() && !l.startsWith('#')) setup.push(l); continue; }
    if (mode !== 'c') continue;
    if (l.startsWith('> ') && cases.length) { cases[cases.length - 1].cmd += '\n' + l.slice(2); continue; }
    if (!l.trim() || l.startsWith('#')) continue;
    let noos = false, fresh = false, cmd = l;
    for (;;) {
      if (cmd.startsWith('@noos ')) { noos = true; cmd = cmd.slice(6); }
      else if (cmd.startsWith('@fresh ')) { fresh = true; cmd = cmd.slice(7); }
      else break;
    }
    cases.push({ cmd, noos, fresh });
  }
  return { setup: setup.join('\n'), cases };
}

fs.mkdirSync(path.join(out, 'work'), { recursive: true });
const runDir = fs.mkdtempSync(path.join(out, 'work', 'run-'));
const hostPath = process.env.PATH;
const env = (mine) => ({
  PATH: mine ? `${bindir}:${hostPath}` : hostPath,
  LC_ALL: 'C', TZ: 'UTC', HOME: runDir, TMPDIR: path.join(runDir, 'tmp'),
  ASAN_OPTIONS: 'detect_leaks=0:abort_on_error=0', UBSAN_OPTIONS: 'print_stacktrace=1',
});
fs.mkdirSync(path.join(runDir, 'tmp'));
let dirNo = 0;
function newDir(setup, mine) {
  const dir = path.join(runDir, String(++dirNo));
  fs.mkdirSync(dir);
  if (setup) spawnSync('sh', ['-c', setup], { cwd: dir, env: env(mine), timeout: 10000, stdio: 'ignore' });
  return dir;
}
/* names, sizes and modification times under dir: to notice a case that wrote there */
function fingerprint(dir) {
  const out = [];
  const walk = (d) => {
    for (const e of fs.readdirSync(d, { withFileTypes: true }).sort((a, b) => (a.name < b.name ? -1 : 1))) {
      const p = path.join(d, e.name), st = fs.lstatSync(p);
      out.push(`${p}:${st.size}:${st.mtimeMs}:${st.mode}`);
      if (e.isDirectory()) walk(p);
    }
  };
  walk(dir);
  return out.join('\n');
}
const shared = {};
function runCase(setup, cmd, mine, fresh) {
  const key = (mine ? 'm' : 'h') + setup;
  let dir;
  if (fresh) dir = newDir(setup, mine);
  else {
    if (!shared[key]) { const d = newDir(setup, mine); shared[key] = { dir: d, fp: fingerprint(d) }; }
    dir = shared[key].dir;
  }
  const r = spawnSync('sh', ['-c', cmd], { cwd: dir, env: env(mine), timeout: 10000, maxBuffer: 64 << 20, stdio: ['ignore', 'pipe', 'pipe'] });
  let wrote = false;
  if (!fresh && fingerprint(dir) !== shared[key].fp) { wrote = true; delete shared[key]; }
  return {
    out: r.stdout ? r.stdout.toString('latin1') : '',
    status: r.error ? (r.error.code === 'ETIMEDOUT' ? 'timeout' : 'error') : r.signal ? 'signal ' + r.signal : r.status,
    err: r.stderr && r.stderr.length > 0,
    stderr: r.stderr ? r.stderr.toString('latin1') : '',
    wrote,
  };
}

const casesDir = path.join(here, 'cases');
let total = 0, failed = 0;
const failures = [];

// Library drivers: NAME.cases lines go through a small C program built twice,
// against our implementation (tested) and the host's C library (reference).
const DRIVERS = { regex: ['retest.c', LIBEXTRA.regex], fnmatch: ['fntest.c', LIBEXTRA.fnmatch] };
function checkDriver(name) {
  const [driver, impl] = DRIVERS[name];
  const cf = path.join(casesDir, name + '.cases'), ef = path.join(casesDir, name + '.expected');
  if (!fs.existsSync(cf)) return;
  const flags = ['-O1', '-g', '-Wall', '-Wextra', '-D_GNU_SOURCE', '-I' + incdir];
  if (san) flags.push('-fsanitize=address,undefined');
  const exe = path.join(bindir, name + '-test');
  let r = spawnSync('nice', ['-n', '19', CC, ...flags, '-o', exe, path.join(here, driver), path.join(root, impl)], { encoding: 'utf8' });
  if (r.status !== 0) { process.stdout.write(`build ${driver}: FAILED\n` + r.stderr); failed++; return; }
  if (regen) {
    r = spawnSync('nice', ['-n', '19', CC, '-O1', '-D_GNU_SOURCE', '-o', exe + '-host', path.join(here, driver)], { encoding: 'utf8' });
    if (r.status !== 0) { process.stdout.write(`build ${driver} for the host: FAILED\n` + r.stderr); return; }
    r = spawnSync(exe + '-host', [cf], { maxBuffer: 64 << 20 });
    fs.writeFileSync(ef, r.stdout);
  }
  r = spawnSync(exe, [cf], { maxBuffer: 64 << 20, timeout: 60000, env: env(true) });
  const got = r.stdout.toString().split('\n'), exp = fs.readFileSync(ef, 'utf8').split('\n');
  const inputs = fs.readFileSync(cf, 'utf8').split('\n').filter((l) => l && !l.startsWith('#'));
  let bad = 0;
  for (let i = 0; i < inputs.length; i++) {
    total++;
    if (got[i] !== exp[i]) { bad++; failed++; if (bad <= 20) failures.push(`${name}: ${JSON.stringify(inputs[i])}\n    expected ${exp[i]}\n    got      ${got[i]}`); }
  }
  if (r.stderr.length) { failed++; failures.push(`${name}: stderr from the driver:\n` + r.stderr.toString().slice(0, 2000)); }
  process.stdout.write(`${name.padEnd(10)} ${inputs.length - bad}/${inputs.length}\n`);
}

function checkFile(name) {
  const cf = path.join(casesDir, name + '.t'), ef = path.join(casesDir, name + '.expected');
  const { setup, cases } = parseCases(cf);
  let exp = [];
  if (regen) {
    exp = cases.map(({ cmd, fresh }) => { const r = runCase(setup, cmd, false, fresh); return { cmd, out: r.out, status: r.status, err: r.err }; });
    fs.writeFileSync(ef, exp.map((e) => JSON.stringify(e)).join('\n') + '\n');
  } else if (fs.existsSync(ef)) exp = fs.readFileSync(ef, 'utf8').split('\n').filter(Boolean).map((l) => JSON.parse(l));
  let pass = 0;
  cases.forEach(({ cmd, fresh }, i) => {
    total++;
    const e = exp[i];
    const r = runCase(setup, cmd, true, fresh);
    let why = '';
    if (r.wrote) why = 'the case changed the shared directory: mark it @fresh';
    if (why) ;
    else if (!e || e.cmd !== cmd) why = 'no expectation (run with --regen)';
    else if (r.out !== e.out) why = `stdout differs\n    expected ${JSON.stringify(e.out).slice(0, 400)}\n    got      ${JSON.stringify(r.out).slice(0, 400)}`;
    else if (r.status !== e.status) why = `exit status ${r.status}, expected ${e.status}`;
    else if (r.err !== e.err) why = r.err ? 'unexpected stderr' : 'expected an error message';
    if (/AddressSanitizer|runtime error:|LeakSanitizer/.test(r.stderr)) why = 'sanitizer report\n' + r.stderr.slice(0, 1500);
    if (why) { failed++; failures.push(`${name}: ${JSON.stringify(cmd)}: ${why}${r.stderr && !why.startsWith('sanitizer') ? '\n    stderr: ' + JSON.stringify(r.stderr.slice(0, 300)) : ''}`); }
    else pass++;
  });
  process.stdout.write(`${name.padEnd(10)} ${pass}/${cases.length}\n`);
}

// The in-OS script: same cases, expected stdout concatenated into one file per tool.
function emitOS() {
  const testsDir = path.join(root, 'rootfs/usr/share/tests');
  const dataDir = path.join(testsDir, 'coretools');
  fs.mkdirSync(dataDir, { recursive: true });
  const q = (s) => "'" + s.replace(/'/g, "'\\''") + "'";
  let sh = `# SPDX-License-Identifier: GPL-2.0-or-later
# Core tools, inside UC.OS: a subset of tools/tests/coretools (generated by
# "node tools/tests/coretools/run.mjs --emit-os"; do not edit by hand).
# Run:  sh /usr/share/tests/coretools.sh [-v] [TOOL...]   (-v shows outputs of failures)
# Each case runs in a fresh directory; stdout, exit status and whether
# anything was written to stderr are compared with the host's tools.
D=/usr/share/tests/coretools
T=/tmp/coretools.$$
mkdir -p $T
pass=0
fail=0
n=0
want() { [ -z "$ONLY" ] && return 0; for w in $ONLY; do [ "$w" = "$1" ] && return 0; done; return 1; }
V=
[ "$1" = "-v" ] && { V=1; shift; }
ONLY="$*"
chk() { # chk TOOL N OFFSET LENGTH STATUS ERR
  if [ "$4" -gt 0 ]; then tail -c +$(($3 + 1)) $D/$1.out | head -c $4 > $T/exp; else : > $T/exp; fi
  e=0; [ -s $T/err ] && e=1
  if cmp -s $T/exp $T/out && [ "$st" = "$5" ] && [ "$e" = "$6" ]; then pass=$((pass + 1))
  else fail=$((fail + 1)); echo "FAIL  $1 case $2 (status $st, expected $5; stderr $e, expected $6)"
    if [ -n "$V" ]; then echo "  expected:"; head -c 300 $T/exp; echo "  got:"; head -c 300 $T/out; echo "  stderr:"; head -c 200 $T/err; fi; fi
}
`;
  const names = fs.readdirSync(casesDir).filter((f) => f.endsWith('.t')).map((f) => f.slice(0, -2)).sort();
  let count = 0;
  for (const name of names) {
    if (only.length && !only.includes(name)) continue;
    const { setup, cases } = parseCases(path.join(casesDir, name + '.t'));
    const efile = path.join(casesDir, name + '.expected');
    if (!fs.existsSync(efile)) continue;
    const exp = fs.readFileSync(efile, 'utf8').split('\n').filter(Boolean).map((l) => JSON.parse(l));
    const chunks = [];
    let off = 0, body = '';
    cases.forEach(({ cmd, noos }, i) => {
      const e = exp[i];
      if (noos || !e || e.cmd !== cmd || typeof e.status !== 'number') return;
      const buf = Buffer.from(e.out, 'latin1');
      chunks.push(buf);
      /* the status is written from inside the subshell: the subshell's own exit status
         came back as 0 for failing commands in this script under UC's sh (reported to
         the shell stream); the setup runs first, outside */
      body += `n=$((n + 1)); mkdir $T/$n; cd $T/$n\nsetup_${name} > /dev/null 2>&1\n( ${cmd}\necho $? > $T/st\n) > $T/out 2> $T/err\nread st < $T/st\nchk ${name} ${i + 1} ${off} ${buf.length} ${e.status} ${e.err ? 1 : 0}\n`;
      off += buf.length;
      count++;
    });
    fs.writeFileSync(path.join(dataDir, name + '.out'), Buffer.concat(chunks));
    sh += `setup_${name}() {\n:\n${setup}\n}\nif want ${name}; then\necho "--- ${name}"\n${body}fi\n`;
  }
  sh += 'cd /\necho "coretools: $((pass + fail)) checks, $pass passed, $fail failed"\n';
  fs.writeFileSync(path.join(testsDir, 'coretools.sh'), sh);
  process.stdout.write(`wrote ${count} cases to rootfs/usr/share/tests/coretools.sh\n`);
}

if (opt('--emit-os')) { emitOS(); process.exit(0); }
const allNames = fs.readdirSync(casesDir).filter((f) => f.endsWith('.t')).map((f) => f.slice(0, -2)).sort();
const names = only.length ? only : [...Object.keys(DRIVERS), ...allNames];
for (const n of Object.keys(TOOLS)) if (!only.length || only.includes(n)) build(n);
for (const n of names) {
  if (DRIVERS[n]) { checkDriver(n); continue; }
  if (!allNames.includes(n)) { process.stdout.write(`${n}: no case file\n`); continue; }
  checkFile(n);
}
for (const f of failures.slice(0, verbose ? 1000 : 40)) process.stdout.write('FAIL ' + f + '\n');
if (failures.length > 40 && !verbose) process.stdout.write(`... ${failures.length - 40} more (use --verbose)\n`);
process.stdout.write(`coretools: ${total} cases, ${total - failed} passed, ${failed} failed (work dir ${runDir})\n`);
process.exit(failed ? 1 : 0);
