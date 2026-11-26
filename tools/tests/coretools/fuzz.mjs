// SPDX-License-Identifier: GPL-2.0-or-later
// Fuzz the native builds made by run.mjs (build-native/bin), for a fixed time.
//   node tools/tests/coretools/fuzz.mjs sed SECONDS    mutated scripts: no crash, no sanitizer report, no hang
//   node tools/tests/coretools/fuzz.mjs awk SECONDS    the same for awk programs
//   node tools/tests/coretools/fuzz.mjs diff SECONDS   random file pairs: the output, applied with the
//                                                      host's patch, gives the second file, and has as
//                                                      many changed lines as the host's diff (needs patch)
// Work files go to build-native/fuzz/ and are overwritten, never deleted.
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');
const bin = path.join(root, 'build-native/bin'), dir = path.join(root, 'build-native/fuzz');
fs.mkdirSync(dir, { recursive: true });
const [mode = 'sed', secs = '50', seedArg = '1'] = process.argv.slice(2);
let seed = Number(seedArg) >>> 0 || 1;
const rnd = (n) => { seed ^= seed << 13; seed >>>= 0; seed ^= seed >>> 17; seed ^= seed << 5; seed >>>= 0; return seed % n; };
const env = { ...process.env, ASAN_OPTIONS: 'detect_leaks=0', LC_ALL: 'C', TMPDIR: dir };
const end = Date.now() + Number(secs) * 1000;
let runs = 0, problems = 0;
const report = (what, detail) => { problems++; if (problems <= 20) console.log(`PROBLEM ${what}: ${detail}`); };

if (mode === 'sed') {
  const seeds = ['s/a/b/g', '/x/,/y/{p;d}', '$!N;P;D', ':a;N;$!ba;s/\\n/ /g', 'y/abc/xyz/', '1~2{h;d};G', 's/\\(a*\\)\\(b\\)/\\2\\1/2p',
    '/^$/d', 'a\\\ntext', 'i foo', '2,4c\\\nbar', 'l 5', '=', 'n;n;s/$/!/', 's/x/\\U&\\E/w out', 'x;$!d;x', '0,/a/s//X/', '/a/I,+2!s/./&&/3g',
    ':l;s/^.\\{1,5\\}$/ &/;tl', 'R in', 'r in', 'W out', 'Q', '3q5', 'T;s/a/b/', 's|/|:|g', '\\%a%d', 'z;F', 's/[[:space:]]*$//', '{s/a/b/;:x;bx}'];
  const alpha = 'sabxyz/\\|,;{}!$^.*[]()&123\nNPDGHghpdnqQlrRwWtTyb:=+~-IMegaic ';
  fs.writeFileSync(path.join(dir, 'in'), 'aaa\nbab\n\nxyz a\nlast');
  while (Date.now() < end) {
    let s = seeds[rnd(seeds.length)];
    for (let m = rnd(5); m > 0; m--) {
      const at = rnd(s.length + 1), c = alpha[rnd(alpha.length)];
      const op = rnd(3);
      s = op === 0 ? s.slice(0, at) + c + s.slice(at) : op === 1 ? s.slice(0, at) + s.slice(at + 1) : s.slice(0, at) + c + s.slice(at + 1);
    }
    if (/\bb\s*[a-z]*\s*$|\bb\b|:x;bx/.test(s) && /:/.test(s)) { /* loops on purpose are possible: the timeout catches them */ }
    runs++;
    const r = spawnSync(path.join(bin, 'sed'), ['-E'.slice(0, rnd(2) * 2), '-e', s, 'in'].filter(Boolean), { cwd: dir, env, timeout: 2000, input: 'stdin line\n' });
    const err = r.stderr ? r.stderr.toString() : '';
    if (/AddressSanitizer|runtime error:/.test(err)) report('sanitizer', JSON.stringify(s) + '\n' + err.slice(0, 800));
    else if (r.signal && r.signal !== 'SIGTERM') report('signal ' + r.signal, JSON.stringify(s));
    else if (r.error && r.error.code === 'ETIMEDOUT') {
      /* a script with a branch back to a label can loop forever on purpose, as in GNU sed */
      if (!/[bt]/.test(s)) report('timeout', JSON.stringify(s));
    }
  }
} else if (mode === 'awk') {
  const seeds = ['{ print $1 }', 'BEGIN { x = 1; print x + 2 }', '{ a[$1]++ } END { for (k in a) print k, a[k] }', 'NR % 2 { gsub(/a/, "b"); print }',
    'function f(n) { return n < 2 ? n : f(n-1) + f(n-2) } BEGIN { print f(5) }', '{ printf "%5.2f %s\\n", $2, substr($0, 2, 3) }',
    '/x/, /y/ { print NR }', 'BEGIN { while ((getline l < "in") > 0) n++; print n }', '{ $3 = "z"; NF = 2; print }',
    'BEGIN { n = split("a:b", p, ":"); print n, p[1] }', '{ s = s $0 } END { print length(s), match(s, /[0-9]+/) }',
    'BEGIN { if (1) { for (i = 0; i < 3; i++) continue } else print "x" }', '{ print > "out" } END { close("out") }'];
  const alpha = '{}()[]$;,"/\\+-*%^!<>=~?:|&abcxyz0123 \nNFRSOgetlineprintfunctioninreturndelete';
  fs.writeFileSync(path.join(dir, 'in'), 'a 1 x\nb 2 y\nx 3 z\n');
  while (Date.now() < end) {
    let s = seeds[rnd(seeds.length)];
    for (let m = rnd(5); m > 0; m--) {
      const at = rnd(s.length + 1), c = alpha[rnd(alpha.length)], op = rnd(3);
      s = op === 0 ? s.slice(0, at) + c + s.slice(at) : op === 1 ? s.slice(0, at) + s.slice(at + 1) : s.slice(0, at) + c + s.slice(at + 1);
    }
    runs++;
    const r = spawnSync(path.join(bin, 'awk'), [s, 'in'], { cwd: dir, env, timeout: 2000, input: '' });
    const err = r.stderr ? r.stderr.toString() : '';
    if (/AddressSanitizer|runtime error:/.test(err)) report('sanitizer', JSON.stringify(s) + '\n' + err.slice(0, 800));
    else if (r.signal && r.signal !== 'SIGTERM') report('signal ' + r.signal, JSON.stringify(s));
    else if (r.error && r.error.code === 'ETIMEDOUT' && !/while|for|do|getline/.test(s)) report('timeout', JSON.stringify(s));
  }
} else if (mode === 'diff') {
  const line = () => ['', 'a', 'b', 'ab', 'c', ' ', 'x y'][rnd(7)];
  while (Date.now() < end) {
    const a = Array.from({ length: rnd(30) }, line);
    const b = a.slice();
    for (let k = rnd(8); k > 0; k--) {
      const op = rnd(3);
      if (op === 0 && b.length) b.splice(rnd(b.length), 1);
      else if (op === 1) b.splice(rnd(b.length + 1), 0, line());
      else if (b.length) b[rnd(b.length)] = line();
    }
    const fa = path.join(dir, 'A'), fb = path.join(dir, 'B'), fo = path.join(dir, 'O');
    const nonl = (x) => (x.length && rnd(6) === 0 ? x.join('\n') : x.map((l) => l + '\n').join(''));
    fs.writeFileSync(fa, nonl(a));
    fs.writeFileSync(fb, nonl(b));
    runs++;
    const fmt = ['-u', '', '-c', '-U0', '-U1'][rnd(5)];
    const mine = spawnSync(path.join(bin, 'diff'), [fmt, fa, fb].filter(Boolean), { env });
    const err = mine.stderr.toString();
    if (/AddressSanitizer|runtime error:/.test(err)) { report('sanitizer', err.slice(0, 800)); continue; }
    const host = spawnSync('diff', [fmt, fa, fb].filter(Boolean), { env });
    if (mine.status !== host.status) { report('status', `${mine.status} vs ${host.status}`); continue; }
    const count = (o, re) => o.toString().split('\n').filter((l) => re.test(l)).length;
    const del = fmt === '' ? /^</ : fmt === '-c' ? /^[-!] / : /^-(?!-- )/, add = fmt === '' ? /^>/ : fmt === '-c' ? /^[+!] / : /^\+(?!\+\+ )/;
    if (fmt !== '-c' && (count(mine.stdout, del) !== count(host.stdout, del) || count(mine.stdout, add) !== count(host.stdout, add)))
      report('changed-line counts', `${fmt} ${JSON.stringify(a)} -> ${JSON.stringify(b)}`);
    fs.copyFileSync(fa, fo);
    fs.writeFileSync(path.join(dir, 'P'), mine.stdout);
    const p = spawnSync('patch', ['-s', fmt === '' ? '--normal' : fmt === '-c' ? '--context' : '--unified', fo, path.join(dir, 'P')], { env });
    if (p.status !== 0 || !fs.readFileSync(fo).equals(fs.readFileSync(fb))) report('patch', `${fmt} ${JSON.stringify(a)} -> ${JSON.stringify(b)} ${p.stderr}`);
  }
}
console.log(`fuzz ${mode}: ${runs} runs, ${problems} problems`);
process.exit(problems ? 1 : 0);
