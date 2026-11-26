// SPDX-License-Identifier: GPL-2.0-or-later
// The page: a screen, a keyboard and a mouse for the kernel. Nothing here knows
// about windows, text or programs: it copies the kernel's framebuffer to a
// canvas and forwards raw input events. Everything you see is drawn in C.
import { boot, checkSupport } from './host.mjs';
import * as K from './keys.mjs';

const canvas = document.getElementById('screen');
const logEl = document.getElementById('log');
const ctx = canvas.getContext('2d', { alpha: false, desynchronized: true });
const params = new URLSearchParams(location.search);
const say = (s) => { logEl.textContent += s; logEl.scrollTop = logEl.scrollHeight; };

// ?scale=1 forces one framebuffer pixel per CSS pixel; default follows the display's pixel ratio (capped at 2).
const scale = Number(params.get('scale')) || Math.min(window.devicePixelRatio || 1, 2);
const size = () => ({ width: Math.max(320, Math.round(innerWidth * scale)), height: Math.max(200, Math.round(innerHeight * scale)) });

async function main() {
  say('UC.OS boot loader\n');
  const missing = checkSupport();
  if (missing.length) {
    say('\nThis browser cannot run UC.OS. Missing: ' + missing.join(', ') + '.\nUse a current Chrome, Edge, Firefox or Safari.\n');
    return;
  }
  const get = async (url) => {
    const r = await fetch(url);
    if (!r.ok) throw new Error(`${url}: HTTP ${r.status}`);
    const b = new Uint8Array(await r.arrayBuffer());
    say(`  loaded ${url} (${(b.length / 1024).toFixed(0)} KiB)\n`);
    return b;
  };
  const base = params.get('base') || '.';
  // Boot phases in milliseconds since navigation, for the developer console and tools/bench/boot.mjs.
  const marks = window.ucosBoot = { script: performance.now() };
  // The image: initrd.tar.gz (about half the bytes; unpacked with the browser's own gzip
  // decoder) when the folder has one, else the plain initrd.tar. A server that already sends
  // the .gz with Content-Encoding: gzip hands over the tar itself, so look at the bytes.
  const image = async () => {
    const r = await fetch(`${base}/initrd.tar.gz`);
    if (!r.ok) return get(`${base}/initrd.tar`);
    let b = new Uint8Array(await r.arrayBuffer());
    const packed = b.length;
    if (b[0] === 0x1f && b[1] === 0x8b) b = new Uint8Array(await new Response(new Blob([b]).stream().pipeThrough(new DecompressionStream('gzip'))).arrayBuffer());
    say(`  loaded ${base}/initrd.tar.gz (${(packed / 1024).toFixed(0)} KiB, ${(b.length / 1024).toFixed(0)} KiB unpacked)\n`);
    return b;
  };
  const [kernel, initrd] = await Promise.all([get(`${base}/kernel.wasm`), image()]);
  marks.fetched = performance.now();

  let dirty = false, frames = 0;
  const s = size();
  canvas.width = s.width; canvas.height = s.height;
  // Sound: one AudioContext at the device's own rate (the kernel reports it to programs),
  // so samples are resampled once, by the program. Browsers keep it silent until the
  // first click or key press on the page, so it is resumed then.
  let audioCtx = null;
  try { audioCtx = new AudioContext({ latencyHint: 'interactive' }); } catch { /* no Web Audio: the host plays silently, in real time */ }
  if (audioCtx) {
    const unlock = () => { audioCtx.resume(); removeEventListener('pointerdown', unlock, true); removeEventListener('keydown', unlock, true); };
    addEventListener('pointerdown', unlock, true);
    addEventListener('keydown', unlock, true);
  }
  const sys = await boot({
    sound: { context: audioCtx, expose: (s) => { window.ucosAudio = s; } },
    kernel, initrd,
    cmdline: params.get('cmdline') || '',
    screen: s,
    log: (text) => { say(text); console.log(text.replace(/\n$/, '')); },
    onFrame: () => { dirty = true; },
    onIdle: () => { logEl.classList.remove('hidden'); say('\nSystem halted: every process has exited. Reload to boot again.\n'); },
    debug: params.has('debug'),
  });
  window.ucos = sys; // for the developer console
  marks.kernelUp = performance.now();

  // ---- screen ----
  // Only the rectangle the kernel saw written since the last frame is copied to the canvas.
  // The ImageData is a view of kernel memory, rebuilt when the buffer moves or grows.
  let img = null;
  const stats = params.has('stats') ? (window.ucosStats = { frames: 0, ms: 0, maxMs: 0, pixels: 0 }) : null;
  function present() {
    if (dirty) {
      const f = sys.frame();
      if (f.addr && f.width === canvas.width && f.height === canvas.height) {
        dirty = false;
        const t0 = stats && performance.now();
        const d = sys.damage();
        if (!img || img.data.buffer !== sys.memory.buffer || img.data.byteOffset !== f.addr || img.width !== f.width || img.height !== f.height)
          img = new ImageData(new Uint8ClampedArray(sys.memory.buffer, f.addr, f.width * f.height * 4), f.width, f.height);
        if (d.w) ctx.putImageData(img, 0, 0, d.x, d.y, d.w, d.h);
        if (stats) { const ms = performance.now() - t0; stats.frames++; stats.ms += ms; if (ms > stats.maxMs) stats.maxMs = ms; stats.pixels += d.w * d.h; }
        if (++frames === 1) { logEl.classList.add('hidden'); marks.firstFrame = performance.now(); } // the desktop takes over from the boot log
      }
    }
    requestAnimationFrame(present);
  }
  requestAnimationFrame(present);
  let resizeTimer = 0;
  addEventListener('resize', () => {
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(() => {
      const n = size();
      if (n.width === canvas.width && n.height === canvas.height) return;
      canvas.width = n.width; canvas.height = n.height;
      sys.resize(n.width, n.height);
    }, 80);
  });

  // ---- keyboard ----
  const syn = () => sys.input(K.EV_SYN, 0, 0);
  const held = new Set();
  const KEY_UNKNOWN = 240;
  function typed(e) { // the character this key press produces, if any
    if (e.ctrlKey || e.metaKey || e.isComposing) return 0;
    if (e.key.length === 0 || e.key === 'Dead') return 0;
    const cp = e.key.codePointAt(0);
    return [...e.key].length === 1 && cp >= 32 && cp !== 127 ? cp : 0;
  }
  canvas.addEventListener('keydown', (e) => {
    const code = K.KEY[e.code];
    // Let the browser keep a few keys: reload, devtools, fullscreen.
    const browserKey = e.code === 'F5' || e.code === 'F11' || e.code === 'F12' || (e.ctrlKey && e.shiftKey && (e.code === 'KeyI' || e.code === 'KeyJ'));
    if (!browserKey) e.preventDefault();
    if (code === undefined || browserKey) return;
    held.add(code);
    sys.input(K.EV_KEY, code, e.repeat ? 2 : 1);
    const cp = typed(e);
    if (cp) sys.input(K.EV_MSC, K.MSC_UNICODE, cp);
    syn();
  });
  canvas.addEventListener('keyup', (e) => {
    const code = K.KEY[e.code];
    if (code === undefined || !held.delete(code)) return;
    e.preventDefault();
    sys.input(K.EV_KEY, code, 0);
    syn();
  });
  // Dead keys and input methods deliver finished text here.
  canvas.addEventListener('compositionend', (e) => {
    for (const ch of e.data || '') {
      sys.input(K.EV_KEY, KEY_UNKNOWN, 1); sys.input(K.EV_MSC, K.MSC_UNICODE, ch.codePointAt(0)); syn();
      sys.input(K.EV_KEY, KEY_UNKNOWN, 0); syn();
    }
  });
  const releaseAll = () => { for (const code of held) { sys.input(K.EV_KEY, code, 0); } if (held.size) syn(); held.clear(); };
  addEventListener('blur', releaseAll);
  document.addEventListener('visibilitychange', () => { if (document.hidden) releaseAll(); });

  // ---- pointer ----
  const pos = (e) => { sys.input(K.EV_ABS, K.ABS_X, Math.round(e.clientX * scale)); sys.input(K.EV_ABS, K.ABS_Y, Math.round(e.clientY * scale)); };
  const buttons = [K.BTN_LEFT, K.BTN_MIDDLE, K.BTN_RIGHT];
  canvas.addEventListener('pointermove', (e) => { pos(e); syn(); });
  canvas.addEventListener('pointerdown', (e) => { canvas.focus(); canvas.setPointerCapture(e.pointerId); pos(e); if (buttons[e.button]) sys.input(K.EV_KEY, buttons[e.button], 1); syn(); e.preventDefault(); });
  canvas.addEventListener('pointerup', (e) => { pos(e); if (buttons[e.button]) sys.input(K.EV_KEY, buttons[e.button], 0); syn(); e.preventDefault(); });
  canvas.addEventListener('wheel', (e) => { sys.input(K.EV_REL, K.REL_WHEEL, e.deltaY < 0 ? 1 : -1); syn(); e.preventDefault(); }, { passive: false });
  canvas.addEventListener('contextmenu', (e) => e.preventDefault());
  canvas.focus();
}

main().catch((e) => { logEl.classList.remove('hidden'); say('\nBoot failed: ' + (e && e.message || e) + '\n'); console.error(e); });
