// SPDX-License-Identifier: GPL-2.0-or-later
// The mixer, on the browser's audio thread (an AudioWorklet). Every stream the kernel
// opens on /dev/dsp arrives here as 16-bit stereo chunks; this sums them with their
// volumes into the output, so mixing and sample conversion never run on the page's
// main thread, where the whole system lives. Every 4 render quanta (about 11 ms) it
// tells the page how many frames of each stream have played, and which streams ran dry
// while they were being fed (gaps a listener hears).
class Mixer extends AudioWorkletProcessor {
  constructor() {
    super();
    this.streams = new Map();   // id -> { q: [Int16Array], off, gl, gr, fed, dry, dryAt, closing }
    this.played = new Map();    // id -> frames played since the last report
    this.under = [];
    this.quanta = 0;
    this.port.onmessage = (e) => {
      const m = e.data;
      if (m.t === 'w') {
        let s = this.streams.get(m.id);
        if (!s) { s = this.stream(); this.streams.set(m.id, s); }
        if (s.dry && currentTime - s.dryAt < 0.5) this.under.push(m.id); // it was still being fed: a gap
        s.dry = false;
        s.fed = true;
        s.q.push(m.d);
      } else if (m.t === 'open') this.streams.set(m.id, this.stream());
      else if (m.t === 'close') { const s = this.streams.get(m.id); if (s) s.closing = true; }
      else if (m.t === 'reset') { const s = this.streams.get(m.id); if (s) { s.q = []; s.off = 0; s.dry = false; } }
      else if (m.t === 'vol') { const s = this.streams.get(m.id); if (s) { s.gl = m.l / 100 / 32768; s.gr = m.r / 100 / 32768; } }
    };
  }
  stream() { return { q: [], off: 0, gl: 1 / 32768, gr: 1 / 32768, fed: false, dry: false, dryAt: 0, closing: false }; }
  process(inputs, outputs) {
    const L = outputs[0][0], R = outputs[0][1] || L, n = L.length;
    L.fill(0);
    if (R !== L) R.fill(0);
    for (const [id, s] of this.streams) {
      let i = 0;
      while (i < n && s.q.length) {
        const c = s.q[0], take = Math.min((c.length - s.off) >> 1, n - i), gl = s.gl, gr = s.gr;
        for (let k = 0, o = s.off; k < take; k++, o += 2) { L[i + k] += c[o] * gl; R[i + k] += c[o + 1] * gr; }
        i += take;
        s.off += take * 2;
        if (s.off >= c.length) { s.q.shift(); s.off = 0; }
      }
      if (i) this.played.set(id, (this.played.get(id) || 0) + i);
      if (!s.q.length) {
        if (s.closing) { this.streams.delete(id); continue; }
        if (s.fed && !s.dry && i < n) { s.dry = true; s.dryAt = currentTime; }
      }
    }
    if (++this.quanta % 4 === 0 && (this.played.size || this.under.length)) {
      this.port.postMessage({ played: [...this.played], under: this.under });
      this.played.clear();
      this.under = [];
    }
    return true;
  }
}
registerProcessor('ucos-mixer', Mixer);
