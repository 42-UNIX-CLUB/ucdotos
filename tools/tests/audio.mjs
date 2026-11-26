// SPDX-License-Identifier: GPL-2.0-or-later
// Sound, end to end, under Node: boots UC.OS running bin/audiotest with the host saving
// everything /dev/dsp plays (boot/audio.mjs), then checks that recording sample by sample.
//   node tools/tests/audio.mjs [--build=build]
// The last line is "audiotest: N checks, M failed" (audiotest's checks plus these).
import fs from 'node:fs';
import { spawnSync } from 'node:child_process';

const root = new URL('../..', import.meta.url).pathname;
const build = (process.argv.find((a) => a.startsWith('--build=')) || '--build=build').slice(8);
const file = `${root}${build}/audiotest.raw`;   /* in the (git-ignored) build directory, rewritten each run */
const r = spawnSync(process.execPath, [root + 'tools/run.mjs', `--build=${build}`, '--init=/bin/audiotest', `--audio-capture=${file}`],
                    { cwd: root, encoding: 'utf8', timeout: 120000 });
const lines = (r.stdout || '').trimEnd().split('\n');
const last = lines.pop() || '';
for (const l of lines) console.log(l);
const m = /audiotest: (\d+) checks, (\d+) failed/.exec(last);
let checks = m ? +m[1] : 1, fails = m ? +m[2] : 1;
if (!m) console.log('  FAIL  audiotest did not finish: ' + last + (r.stderr || ''));
const check = (name, ok) => { checks++; if (!ok) fails++; console.log(`  ${ok ? 'ok  ' : 'FAIL'}  ${name}`); };

const pcm = fs.existsSync(file) ? new Int16Array(new Uint8Array(fs.readFileSync(file)).buffer) : new Int16Array(0);
const frames = pcm.length >> 1;
console.log(`        (recording: ${(frames / 48000).toFixed(2)} s)`);
// 1. The pattern written to /dev/dsp, unchanged.
const pat = (i, ch) => { const l = (i * 97) % 30000 - 15000; return ch ? -l : l; };
let found = -1;
for (let i = 0; i + 48000 <= frames && found < 0; i++) {
  if (pcm[2 * i] !== pat(0, 0) || pcm[2 * i + 1] !== pat(0, 1) || pcm[2 * i + 2] !== pat(1, 0)) continue;
  let same = true;
  for (let j = 0; j < 48000 && same; j++) same = pcm[2 * (i + j)] === pat(j, 0) && pcm[2 * (i + j) + 1] === pat(j, 1);
  if (same) found = i;
}
check('recording: 1 s written to /dev/dsp arrives unchanged, in order', found >= 0);
// 2. audiod's mix of two clients: 1000 (16-bit, 24 kHz) + 2048 at volume 50 (8-bit) = 2024.
let run = 0, best = 0;
for (let i = 0; i < frames; i++) {
  if (pcm[2 * i] === 2024 && pcm[2 * i + 1] === 2024) { run++; if (run > best) best = run; } else run = 0;
}
check('recording: audiod mixed two clients (converted, resampled, volume) into a steady 2024', best >= 24000);
console.log(`        (longest run of 2024: ${best} frames)`);
console.log(`audiotest: ${checks} checks, ${fails} failed`);
process.exit(fails ? 1 : 0);
