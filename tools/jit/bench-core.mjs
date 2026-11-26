// SPDX-License-Identifier: GPL-2.0-or-later
// The translator benchmark (docs/BINARY-TRANSLATION.md section 3), shared by Node
// (tools/jit/bench.mjs) and Chromium (tools/jit/bench.html).
// For every program: the trivial interpreter gives the reference final state and the
// guest instruction count; every translated configuration must end in the same state
// (exit code, output, registers, memory); the same C compiled straight to wasm gives the
// ceiling. Configurations are run round-robin (`rounds` fresh engines each, `reps` runs
// per engine, the first run of an engine includes translation).
import { runEngine, runDirect } from './sa-host.mjs';

export const PROGRAMS = ['sieve', 'sort', 'sha256', 'fib', 'matmul', 'strloop', 'indirect', 'switch'];
export const CONFIGS = {
  interp: 'mode=interp',
  predecode: 'mode=pre',
  block: 'strategy=block calls=0',
  dispatch: 'strategy=dispatch calls=0',
  struct: 'strategy=struct calls=0',
  'dispatch+calls': 'strategy=dispatch calls=1',
  'struct+calls': 'strategy=struct calls=1',
  aot: 'mode=aot strategy=struct calls=1',
};

const parse = (out) => {
  const runs = [...out.matchAll(/@@RUN \d+ ([\d.]+)/g)].map((m) => Number(m[1]));
  const st = out.match(/@@STATE (.*)/)?.[1] || '';
  const stats = out.match(/@@STATS (.*)/)?.[1] || '';
  const kv = (s) => Object.fromEntries(s.split(' ').filter(Boolean).map((x) => x.split('=')));
  return { runs, state: kv(st), stats: kv(stats) };
};
const sameState = (a, b) => a.exit === b.exit && a.out === b.out && a.regs === b.regs && a.mem === b.mem;
const median = (a) => { const s = [...a].sort((x, y) => x - y); return s.length ? s[s.length >> 1] : NaN; };

export async function bench({ load, log, programs = PROGRAMS, configs = Object.keys(CONFIGS), rounds = 2, reps = 5, extra = '', native = null, gc = false }) {
  const engine = await load('engine.wasm');
  const results = [];
  for (const p of programs) {
    const elf = await load(`guest/${p}${gc ? '-gc' : ''}.elf`);
    // reference: the trivial interpreter, once, counting instructions
    const ref = parse((await runEngine(engine, elf, 'mode=interp stats=1 ' + extra)).out);
    const icount = Number(ref.state.icount);
    const row = { program: p, icount, configs: {} };
    const samples = {};
    for (let r = 0; r < rounds; r++) {
      for (const c of configs) {
        const n = c === 'interp' ? 1 : c === 'predecode' ? 2 : reps;
        const res = await runEngine(engine, elf, `${CONFIGS[c]} repeat=${n} stats=1 ${extra}`);
        const o = parse(res.out);
        const s = samples[c] || (samples[c] = { first: [], warm: [], ok: true, stats: o.stats, loads: [] });
        s.first.push(o.runs[0]);
        s.warm.push(...o.runs.slice(1));
        s.loads.push(...res.loads);
        if (!sameState(o.state, ref.state)) { s.ok = false; log(`MISMATCH ${p} ${c}: ${JSON.stringify(o.state)} vs ${JSON.stringify(ref.state)}`); }
        s.stats = o.stats;
      }
    }
    for (const c of configs) {
      const s = samples[c];
      const all = s.warm.length ? s.warm : s.first;
      row.configs[c] = { best: Math.min(...all), median: median(all), first: median(s.first), ok: s.ok, stats: s.stats,
        loadMedian: median(s.loads), loads: s.loads.length };
    }
    // the ceiling: the same C compiled to wasm
    try {
      const wasm = await load(`guest/${p}.wasm`);
      const times = [];
      let checksum = '';
      for (let r = 0; r < rounds; r++) { const d = await runDirect(wasm, reps); times.push(...d.times.slice(1)); checksum = d.checksum; row.directFirst = d.times[0]; }
      row.direct = { best: Math.min(...times), median: median(times), ok: ref.state && true, checksum };
    } catch (e) { log(`${p}: direct wasm: ${e.message}`); }
    if (native) row.native = await native(p);
    results.push(row);
    log(formatRow(row, configs));
  }
  return results;
}

const mips = (n, ms) => (n / ms / 1000);
export function formatHeader(configs) {
  return `| program | guest insns | ${configs.join(' | ')} | wasm direct | native |\n|---|---|${configs.map(() => '---|').join('')}---|---|`;
}
export function formatRow(row, configs) {
  const cell = (c) => { const x = row.configs[c]; if (!x) return ''; return `${mips(row.icount, x.best).toFixed(0)} (${mips(row.icount, x.median).toFixed(0)})${x.ok ? '' : ' WRONG'}`; };
  const d = row.direct ? `${mips(row.icount, row.direct.best).toFixed(0)} (${mips(row.icount, row.direct.median).toFixed(0)})` : '';
  const n = row.native ? `${mips(row.icount, row.native.best).toFixed(0)}` : '';
  return `| ${row.program} | ${(row.icount / 1e6).toFixed(0)}M | ${configs.map(cell).join(' | ')} | ${d} | ${n} |`;
}
export function formatDetail(results, configs) {
  const lines = ['| program | config | best ms | median ms | first run ms (incl. translation) | regions | wasm bytes | guest insns translated | translate ms (ours) | load ms (host compile+instantiate) | median load per module ms |', '|---|---|---|---|---|---|---|---|---|---|---|'];
  for (const r of results) for (const c of configs) {
    const x = r.configs[c];
    if (!x) continue;
    const s = x.stats;
    lines.push(`| ${r.program} | ${c} | ${x.best.toFixed(1)} | ${x.median.toFixed(1)} | ${x.first.toFixed(1)} | ${s.regions || 0} | ${s.wasm_bytes || 0} | ${s.guest_insns || 0} | ${Number(s.t_translate || 0).toFixed(2)} | ${Number(s.t_load || 0).toFixed(2)} | ${isNaN(x.loadMedian) ? '' : x.loadMedian.toFixed(3)} |`);
  }
  return lines.join('\n');
}
