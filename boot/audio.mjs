// SPDX-License-Identifier: GPL-2.0-or-later
// The host half of /dev/dsp (kernel/audio.c). The kernel hands over each stream's
// 16-bit stereo samples; this file only moves them:
//   in a browser, to the mixer on the audio thread (boot/audio-worklet.js), one message per
//   write, with the buffer transferred, not copied again;
//   under Node (tests) or a browser without Web Audio, to a stand-in that "plays" in real
//   time, so writers are paced as on a real device, and that can save the mixed output
//   to a file (raw 16-bit stereo) for tests to check.
// Either way the kernel hears back how many frames of each stream have played
// (audio_played) and when a stream ran dry while it was being fed (audio_underrun).
const isNode = typeof process !== 'undefined' && !!(process.versions && process.versions.node);
const fs = isNode ? await import('node:fs') : null;

export function audio(getK, opts = {}) {
  const ctx = opts.context || null;
  const rate = ctx ? ctx.sampleRate : 48000;
  return ctx ? worklet(getK, ctx, opts) : standIn(getK, rate, opts);
}

function worklet(getK, ctx, opts) {
  let node = null, fallback = null;
  const pending = [];          // messages sent before the worklet module has loaded
  // What the page can show when sound does not work: window.ucosAudio in the console.
  // played stays 0 while the context is suspended (no click or key press yet) or the
  // mixer did not load.
  const stats = { rate: ctx.sampleRate, context: ctx.state, mixer: 'loading', writes: 0, played: 0, underruns: 0 };
  ctx.addEventListener('statechange', () => { stats.context = ctx.state; });
  if (opts.expose) opts.expose(stats);
  const say = (m) => { console.warn('[audio] ' + m); if (opts.log) opts.log(`[audio] ${m}\n`); };
  const send = (m, transfer) => { if (node) node.port.postMessage(m, transfer || []); else if (!fallback) pending.push([m, transfer]); };
  // Browsers give AudioWorklet only on https and on localhost. Anywhere else the page must
  // still boot: without it the mixer cannot load, and sound falls back to silence (below).
  const load = ctx.audioWorklet
    ? ctx.audioWorklet.addModule(new URL('./audio-worklet.js', import.meta.url))
    : Promise.reject(new Error('no AudioWorklet here: the page is not on https or localhost'));
  load.then(() => {
    node = new AudioWorkletNode(ctx, 'ucos-mixer', { numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2] });
    node.connect(ctx.destination);
    node.port.onmessage = (e) => {
      const k = getK();
      for (const [id, frames] of e.data.played) { k.audio_played(id, frames); stats.played += frames; }
      for (const id of e.data.under) { k.audio_underrun(id); stats.underruns++; }
    };
    node.onprocessorerror = (e) => { stats.mixer = 'crashed'; say('the mixer stopped: ' + (e.message || 'processor error')); };
    for (const [m, t] of pending) node.port.postMessage(m, t || []);
    pending.length = 0;
    stats.mixer = 'ready';
  }).catch((e) => {
    // No mixer: play silently in real time instead, so programs that wait for their sound
    // to play (audiod, play) do not wait forever.
    stats.mixer = 'failed: ' + e.message;
    say('no sound, the mixer did not load: ' + e.message);
    fallback = standIn(getK, ctx.sampleRate, {});
    for (const [m] of pending) if (m.t === 'open') fallback.audio_open(m.id);
    pending.length = 0;
  });
  // A fallback stand-in takes over calls only after the mixer failed.
  const via = (name, f) => (...a) => (fallback ? fallback[name](...a) : f(...a));
  return {
    audio_rate: () => ctx.sampleRate,
    audio_open: via('audio_open', (id) => send({ t: 'open', id })),
    audio_write: via('audio_write', (id, ptr, len) => {
      stats.writes++;
      const d = new Int16Array(getK().memory.buffer.slice(ptr, ptr + len)); // the one copy out of kernel memory
      send({ t: 'w', id, d }, [d.buffer]);
    }),
    audio_close: via('audio_close', (id) => send({ t: 'close', id })),
    audio_volume: via('audio_volume', (id, l, r) => send({ t: 'vol', id, l, r })),
    audio_reset: via('audio_reset', (id) => send({ t: 'reset', id })),
  };
}

// A device that plays in real time without a sound card. Mixed output, if asked for,
// goes to opts.capture (a file path): 16-bit stereo, integer arithmetic, so a test can
// compare it exactly.
function standIn(getK, rate, opts) {
  const streams = new Map();   // id -> { q: [Int16Array], off, l, r, fed, dry, dryAt, closing }
  const out = opts.capture && fs ? fs.openSync(opts.capture, 'w') : null;
  let timer = null, last = 0, owed = 0;
  const stats = { rate, underruns: 0, writes: 0 };
  if (opts.expose) opts.expose(stats);
  const fresh = () => ({ q: [], off: 0, l: 100, r: 100, fed: false, dry: false, dryAt: 0, closing: false });
  function tick() {
    const now = performance.now();
    owed += (now - last) * rate / 1000;
    last = now;
    const n = Math.floor(owed);
    owed -= n;
    if (!n) return;
    const mix = out ? new Int32Array(2 * n) : null;
    const k = getK();
    for (const [id, s] of streams) {
      let i = 0;
      while (i < n && s.q.length) {
        const c = s.q[0], take = Math.min((c.length - s.off) >> 1, n - i);
        if (mix) for (let j = 0, o = s.off; j < take; j++, o += 2) { mix[2 * (i + j)] += c[o] * s.l / 100 | 0; mix[2 * (i + j) + 1] += c[o + 1] * s.r / 100 | 0; }
        i += take;
        s.off += take * 2;
        if (s.off >= c.length) { s.q.shift(); s.off = 0; }
      }
      if (i) k.audio_played(id, i);
      if (!s.q.length) {
        if (s.closing) { streams.delete(id); continue; }
        if (s.fed && !s.dry && i < n) { s.dry = true; s.dryAt = now; }
      }
    }
    if (mix) {
      const b = new Int16Array(2 * n);
      for (let i = 0; i < b.length; i++) b[i] = mix[i] > 32767 ? 32767 : mix[i] < -32768 ? -32768 : mix[i];
      fs.writeSync(out, b);
    }
    let busy = false;
    for (const s of streams.values()) if (s.q.length) busy = true;
    if (!busy) { clearInterval(timer); timer = null; }   // nothing queued: no timer, no cost
  }
  function wake() { if (!timer) { last = performance.now(); owed = 0; timer = setInterval(tick, 10); } }
  return {
    audio_rate: () => rate,
    audio_open: (id) => streams.set(id, fresh()),
    audio_write(id, ptr, len) {
      stats.writes++;
      let s = streams.get(id);
      if (!s) { s = fresh(); streams.set(id, s); }
      const now = performance.now();
      if (s.dry && now - s.dryAt < 500) { getK().audio_underrun(id); stats.underruns++; }
      s.dry = false;
      s.fed = true;
      s.q.push(new Int16Array(getK().memory.buffer.slice(ptr, ptr + len)));
      wake();
    },
    audio_close: (id) => { const s = streams.get(id); if (s) s.closing = true; },
    audio_volume: (id, l, r) => { const s = streams.get(id); if (s) { s.l = l; s.r = r; } },
    audio_reset: (id) => { const s = streams.get(id); if (s) { s.q = []; s.off = 0; s.dry = false; } },
  };
}
