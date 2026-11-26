// SPDX-License-Identifier: GPL-2.0-or-later
// Run a guest ELF file with the standalone engine under Node.
//   node tools/jit/run.mjs [--build=build-NAME] file.elf [key=value ...]
import fs from 'node:fs';
import { runEngine } from './sa-host.mjs';
const args = process.argv.slice(2);
const opt = (n, d) => { const a = args.find((x) => x.startsWith(`--${n}=`)); return a ? a.slice(n.length + 3) : d; };
const build = opt('build', 'build');
const root = new URL('../..', import.meta.url).pathname;
const file = args.find((a) => !a.startsWith('--') && !a.includes('='));
const opts = args.filter((a) => !a.startsWith('--') && a.includes('=')).join(' ');
const engine = fs.readFileSync(`${root}${build}/jit/engine.wasm`);
const r = await runEngine(engine, fs.readFileSync(file), opts, (s) => process.stdout.write(s));
process.exit(r.code);
