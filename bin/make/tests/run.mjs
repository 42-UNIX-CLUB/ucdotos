// SPDX-License-Identifier: GPL-2.0-or-later
// Differential tests for bin/make: each case runs with GNU Make and with ours
// in scratch directories; standard output, standard error, exit status and the
// files left behind must be the same.
//
//   node bin/make/tests/run.mjs [options] [name-filter ...]
//     --make=PATH      the make to test (default: build bin/make natively with cc)
//     --gnu=PATH       GNU Make (default: "make" on PATH, if it is GNU Make 4.x)
//     --update         rewrite tests/expected/ from GNU Make
//     --stored         compare with tests/expected/ even if GNU Make is present
//     --keep           keep the scratch directories
//     --jobs=N         cases run at once (default 1)
//     -v               print every case, not only failures
//
// Case files (tests/cases/*.mk): sections start with a line "--- WHAT":
//   --- file NAME      a file to create (the first section may be "--- Makefile")
//   --- run            one shell command per line, run in order in the same directory
// Lines before the first section are a description.
import { spawnSync, spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';

const here = path.dirname(new URL(import.meta.url).pathname);
const srcdir = path.resolve(here, '..');
const opts = { jobs: 1, filters: [] };
for (const a of process.argv.slice(2)) {
  if (a.startsWith('--make=')) opts.make = path.resolve(a.slice(7));
  else if (a.startsWith('--gnu=')) opts.gnu = a.slice(6);
  else if (a === '--update') opts.update = true;
  else if (a === '--stored') opts.stored = true;
  else if (a === '--keep') opts.keep = true;
  else if (a.startsWith('--jobs=')) opts.jobs = Math.max(1, parseInt(a.slice(7), 10) || 1);
  else if (a === '-v') opts.verbose = true;
  else opts.filters.push(a);
}

const tmpRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'mktest-'));

function which(cmd) {
  const r = spawnSync('sh', ['-c', `command -v ${cmd}`], { encoding: 'utf8' });
  return r.status === 0 ? r.stdout.trim() : null;
}

// ---- the make under test ----
if (!opts.make) {
  const cc = process.env.CC || 'cc';
  const out = path.join(tmpRoot, 'ucmake');
  const srcs = fs.readdirSync(srcdir).filter(f => f.endsWith('.c')).map(f => path.join(srcdir, f));
  const flags = ['-O1', '-g', '-Wall', '-Wextra', '-fsanitize=address,undefined', '-o', out, ...srcs];
  const r = spawnSync(cc, flags, { encoding: 'utf8' });
  if (r.status !== 0) {
    process.stderr.write(r.stderr || '');
    console.error(`could not build make with ${cc}`);
    process.exit(2);
  }
  opts.make = out;
}

// ---- GNU Make ----
let gnu = opts.gnu || which('gmake') || which('make');
if (gnu) {
  const r = spawnSync(gnu, ['--version'], { encoding: 'utf8' });
  if (r.status !== 0 || !/^GNU Make 4\./.test(r.stdout)) gnu = null;
}
if (opts.update && !gnu) { console.error('--update needs GNU Make 4.x'); process.exit(2); }
const useStored = opts.stored || !gnu;
if (!gnu) console.log('GNU Make 4.x not found: comparing with the stored expected outputs in tests/expected/.');

// wrapper directories so that both programs are called "make" (messages use argv[0])
function wrapdir(name, target) {
  const d = path.join(tmpRoot, name);
  fs.mkdirSync(d);
  fs.symlinkSync(target, path.join(d, 'make'));
  return d;
}
const oursBin = wrapdir('bin-ours', opts.make);
const gnuBin = gnu ? wrapdir('bin-gnu', gnu) : null;

// ---- cases ----
function parseCase(text) {
  const c = { desc: [], files: [], runs: [] };
  let cur = null;
  for (const line of text.split('\n')) {
    const m = /^--- (\S+)(?: (.*))?$/.exec(line);
    if (m) {
      if (m[1] === 'Makefile') cur = { name: 'Makefile', lines: [] }, c.files.push(cur);
      else if (m[1] === 'file') cur = { name: m[2], lines: [] }, c.files.push(cur);
      else if (m[1] === 'run') cur = { run: true, lines: [] }, c.runs.push(cur);
      else throw new Error(`bad section: ${line}`);
      continue;
    }
    if (!cur) c.desc.push(line);
    else cur.lines.push(line);
  }
  for (const f of c.files) {
    while (f.lines.length && f.lines[f.lines.length - 1] === '') f.lines.pop();
    f.text = f.lines.join('\n') + '\n';
  }
  c.cmds = c.runs.flatMap(r => r.lines.filter(l => l.trim() && !l.startsWith('#')));
  return c;
}

function snapshot(dir) {
  const out = [];
  const walk = (d, rel) => {
    for (const e of fs.readdirSync(d, { withFileTypes: true }).sort((a, b) => (a.name < b.name ? -1 : 1))) {
      const p = path.join(d, e.name), r = rel ? `${rel}/${e.name}` : e.name;
      if (e.isDirectory()) { out.push(`${r}/`); walk(p, r); continue; }
      if (!e.isFile()) { out.push(`${r} (special)`); continue; }
      const data = fs.readFileSync(p);
      const text = data.length <= 4096 && !data.includes(0);
      if (text) out.push(`${r}: ${JSON.stringify(data.toString('utf8'))}`);
      else out.push(`${r}: ${data.length} bytes sha1 ${crypto.createHash('sha1').update(data).digest('hex')}`);
    }
  };
  walk(dir, '');
  return out.join('\n');
}

function normalize(s, dir) {
  return s.split(dir).join('<DIR>')
    .replace(/ --jobserver-auth=[^\s\]'"]+/g, '')
    .replace(/\r/g, '');
}

function runOne(bin, c, dir) {
  return new Promise(resolve => {
    fs.mkdirSync(dir, { recursive: true });
    for (const f of c.files) {
      fs.mkdirSync(path.dirname(path.join(dir, f.name)), { recursive: true });
      fs.writeFileSync(path.join(dir, f.name), f.text);
    }
    const env = { PATH: `${bin}:/usr/local/bin:/usr/bin:/bin`, HOME: dir, LANG: 'C', LC_ALL: 'C', TZ: 'UTC',
      ASAN_OPTIONS: 'detect_leaks=0:abort_on_error=0', UBSAN_OPTIONS: 'print_stacktrace=1' };
    const parts = [];
    let i = 0;
    const next = () => {
      if (i >= c.cmds.length) { resolve({ parts, files: normalize(snapshot(dir), dir) }); return; }
      const cmd = c.cmds[i++];
      const p = spawn('/bin/sh', ['-c', cmd], { cwd: dir, env });
      let out = '', err = '';
      const t = setTimeout(() => { p.kill('SIGKILL'); err += '\n<timeout>'; }, 20000);
      p.stdout.on('data', d => (out += d));
      p.stderr.on('data', d => (err += d));
      p.on('close', (code, sig) => {
        clearTimeout(t);
        parts.push(`=== ${cmd}\n--- status ${sig ? 'signal ' + sig : code}\n--- stdout\n${normalize(out, dir)}--- stderr\n${normalize(err, dir)}`);
        next();
      });
    };
    next();
  });
}

function transcript(r) { return r.parts.join('\n') + '\n=== files\n' + r.files + '\n'; }

function firstDiff(a, b) {
  const x = a.split('\n'), y = b.split('\n');
  for (let i = 0; i < Math.max(x.length, y.length); i++) {
    if (x[i] !== y[i]) {
      const from = Math.max(0, i - 3);
      return `line ${i + 1}:\n  expected: ${JSON.stringify(x[i])}\n  got:      ${JSON.stringify(y[i])}\n` +
        `context (expected):\n${x.slice(from, i + 4).map(l => '    | ' + l).join('\n')}\n` +
        `context (got):\n${y.slice(from, i + 4).map(l => '    | ' + l).join('\n')}`;
    }
  }
  return 'identical';
}

const casedir = path.join(here, 'cases'), expdir = path.join(here, 'expected');
fs.mkdirSync(expdir, { recursive: true });
let names = fs.readdirSync(casedir).filter(f => f.endsWith('.mk')).sort();
const fileFiltered = opts.filters.length && names.some(n => opts.filters.some(f => n.includes(f)));
if (fileFiltered) names = names.filter(n => opts.filters.some(f => n.includes(f)));

let pass = 0, fail = 0, drift = 0, total = 0;
const failed = [];
// a file may hold several cases, each starting with a line "##### NAME"
function splitCases(text, base) {
  if (!/^##### /m.test(text)) return [{ name: base, text }];
  const out = [];
  let head = '';
  let cur = null;
  for (const line of text.split('\n')) {
    const m = /^##### (.*)$/.exec(line);
    if (m) { cur = { name: m[1].trim(), lines: [] }; out.push(cur); continue; }
    if (cur) cur.lines.push(line); else head += line + '\n';
  }
  return out.map(c => ({ name: c.name, text: c.lines.join('\n') }));
}
function splitStored(text) {
  const map = new Map();
  for (const chunk of text.split(/^##### /m)) {
    if (!chunk) continue;
    const nl = chunk.indexOf('\n');
    map.set(chunk.slice(0, nl), chunk.slice(nl + 1));
  }
  return map;
}
async function doFile(fname) {
  const base = fname.replace(/\.mk$/, '');
  const cases = splitCases(fs.readFileSync(path.join(casedir, fname), 'utf8'), base);
  const expFile = path.join(expdir, base + '.out');
  const stored = fs.existsSync(expFile) ? splitStored(fs.readFileSync(expFile, 'utf8')) : new Map();
  const newStored = [];
  for (const cs of cases) {
    if (opts.filters.length && !fileFiltered && !opts.filters.some(f => cs.name.includes(f))) continue;
    total++;
    const c = parseCase(cs.text);
    const id = cs.name.replace(/[^A-Za-z0-9_.-]/g, '_');
    let expected;
    if (!useStored || opts.update) {
      const g = await runOne(gnuBin, c, path.join(tmpRoot, 'g', base, id));
      expected = transcript(g);
      newStored.push(`##### ${cs.name}\n${expected}`);
      if (!opts.update && stored.get(cs.name) !== expected) drift++;
    } else {
      expected = stored.get(cs.name);
      if (expected === undefined) { fail++; failed.push(cs.name); console.log(`FAIL  ${base}/${cs.name}: no stored expected output`); continue; }
    }
    const o = await runOne(oursBin, c, path.join(tmpRoot, 'o', base, id));
    const got = transcript(o);
    if (got === expected) { pass++; if (opts.verbose) console.log(`ok    ${base}/${cs.name}`); }
    else {
      fail++;
      failed.push(`${base}/${cs.name}`);
      console.log(`FAIL  ${base}/${cs.name}  (${c.desc.find(l => l.trim()) || ''})\n${firstDiff(expected, got)}\n`);
    }
  }
  if (opts.update && !(opts.filters.length && !fileFiltered)) fs.writeFileSync(expFile, newStored.join(''));
}

const queue = [...names];
await Promise.all(Array.from({ length: opts.jobs }, async () => {
  while (queue.length) await doFile(queue.shift());
}));

if (!opts.keep) fs.rmSync(tmpRoot, { recursive: true, force: true });
else console.log(`scratch directories kept in ${tmpRoot}`);
if (drift) console.log(`note: ${drift} stored expected outputs differ from this GNU Make; run with --update to refresh them`);
console.log(`make tests: ${total} cases in ${names.length} files, ${pass} passed, ${fail} failed${useStored ? ' (against stored outputs)' : ' (against GNU Make)'}`);
process.exit(fail ? 1 : 0);
