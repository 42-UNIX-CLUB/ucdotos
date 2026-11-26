// SPDX-License-Identifier: GPL-2.0-or-later
// The page: a screen, a keyboard and a mouse for the kernel. Nothing here knows
// about windows, text or programs: it copies the kernel's framebuffer to a
// canvas and forwards raw input events. Everything you see is drawn in C.
import { boot, checkSupport } from './host.mjs';
import { openStore } from './store.mjs';
import * as K from './keys.mjs';

const canvas = document.getElementById('screen');
const logEl = document.getElementById('log');
const ctx = canvas.getContext('2d', { alpha: false, desynchronized: true });
const params = new URLSearchParams(location.search);
const say = (s) => { logEl.textContent += s; logEl.scrollTop = logEl.scrollHeight; };

// ?scale=1 forces one framebuffer pixel per CSS pixel; default follows the display's pixel ratio (capped at 2).
const scale = Number(params.get('scale')) || Math.min(window.devicePixelRatio || 1, 2);
const size = () => ({ width: Math.max(320, Math.round(innerWidth * scale)), height: Math.max(200, Math.round(innerHeight * scale)) });

// One tab at a time (owner decision): a tab boots only while it holds the Web Lock
// "ucos-store", which it keeps for its whole life. A second tab of the same origin waits for
// the lock and says why; the browser grants it the moment the first tab closes, and the
// second tab then boots by itself. The first tab never notices. ?nostore boots at once,
// without the lock and without storage: such a session keeps nothing and touches nothing,
// so it can run next to another tab.
function oneTab() {
  if (!navigator.locks) return Promise.resolve(true); // no Web Locks: nothing to coordinate with
  return new Promise((ready) => {
    navigator.locks.request('ucos-store', { ifAvailable: true }, (lock) => {
      if (lock) { ready(true); return new Promise(() => {}); } // held until the tab goes away
      waitForTurn(ready);
      return null;
    });
  });
}
function waitForTurn(ready) {
  window.ucosWaiting = true; // for tests
  const box = document.createElement('div');
  box.id = 'busy';
  box.innerHTML = '<p><b>UC.OS is already open in another tab.</b></p>' +
    '<p>One machine runs at a time, so the files you keep stay in one piece.<br>' +
    'Close the other tab and this one starts on its own.</p>' +
    '<p class="dim">Waiting for the other tab to close.</p>' +
    '<p><button id="busy-retry" type="button">Check again</button></p>';
  document.body.appendChild(box);
  logEl.classList.add('hidden');
  const go = () => { box.remove(); logEl.classList.remove('hidden'); window.ucosWaiting = false; };
  // The plain request waits in the browser's queue and is granted when the other tab closes.
  navigator.locks.request('ucos-store', (lock) => { go(); ready(true); return new Promise(() => {}); });
  // The button asks the browser what it knows right now; it never takes the lock twice.
  box.querySelector('#busy-retry').addEventListener('click', async () => {
    const q = await navigator.locks.query();
    const held = (q.held || []).some((l) => l.name === 'ucos-store');
    box.querySelector('.dim').textContent = held ? 'Still open somewhere. This tab will start as soon as it closes.' : 'The other tab is closing. Starting...';
  });
}

async function main() {
  say('UC.OS boot loader\n');
  const missing = checkSupport();
  if (missing.length) {
    say('\nThis browser cannot run UC.OS. Missing: ' + missing.join(', ') + '.\nUse a current Chrome, Edge, Firefox or Safari.\n');
    return;
  }
  const nostore = params.has('nostore') || typeof indexedDB === 'undefined';
  if (!nostore) await oneTab();
  else if (params.has('nostore')) say('  ?nostore: this session keeps nothing and may run next to another tab\n');
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

  // ---- persistence (boot/store.mjs). This tab holds the "ucos-store" lock (oneTab above),
  // so no other tab has the database open: ?reset (empty the store first) is safe here too.
  let store = null;
  if (!nostore) {
    try {
      if (params.has('reset')) { await new Promise((r) => { const q = indexedDB.deleteDatabase('ucos-store'); q.onsuccess = q.onerror = q.onblocked = r; }); say('  store emptied (?reset)\n'); }
      store = await openStore({ kind: 'idb', log: say });
      if (store) say(`  store: ${store.records} records\n`);
    } catch (e) { say(`  storage is not available (${e && e.message}): changes will not be kept\n`); store = null; }
  }
  marks.store = performance.now();

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
  // Mouse capture for a fullscreen game (the window manager asks through EVIOCGRAB): lock
  // the browser's pointer so it is hidden and cannot leave the page, and send movement
  // instead of positions. A browser only grants the lock right after a click or key press;
  // if this request is refused, the next click on the page takes it (and is not passed on).
  // Esc always gives the mouse back (the browser's rule); a click takes it again. If the
  // browser refuses the lock outright (some do, e.g. headless Firefox), clicks are passed
  // on as usual, so a game still gets its buttons.
  let wantLock = false, lockRefused = false;
  document.addEventListener('pointerlockerror', () => { lockRefused = true; });
  document.addEventListener('pointerlockchange', () => { if (document.pointerLockElement === canvas) lockRefused = false; });
  const lockPointer = () => { try { const p = canvas.requestPointerLock(); if (p && p.catch) p.catch(() => {}); } catch { /* no Pointer Lock */ } };
  const sys = await boot({
    sound: { context: audioCtx, expose: (s) => { window.ucosAudio = s; } },
    onPointerLock: (on) => {
      wantLock = on;
      if (on) lockPointer();
      else if (document.pointerLockElement === canvas) document.exitPointerLock();
    },
    kernel, initrd,
    // ucos.admin (may this session become root?) is not for the address bar: it comes from
    // the server that serves the page, as <meta name="ucos-admin" content="1">. Default: no.
    cmdline: [(params.get('cmdline') || '').replace(/(^|\s)ucos\.admin=\S*/g, ' ').trim(), document.querySelector('meta[name="ucos-admin"]')?.content === '1' ? 'ucos.admin=1' : ''].filter(Boolean).join(' '),
    screen: s,
    log: (text) => { say(text); console.log(text.replace(/\n$/, '')); },
    onFrame: () => { dirty = true; },
    onIdle: () => { logEl.classList.remove('hidden'); say('\nSystem halted: every process has exited. Reload to boot again.\n'); },
    debug: params.has('debug'),
    remote: (url) => fetch(`${base}/${url}`), // remote files (kernel/remote.c) sit beside the image
    store,
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
    if (e.ctrlKey && e.altKey && e.code === 'KeyO' && !e.repeat) { e.preventDefault(); picker.click(); return; } // see "files from the user's disk"
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
  document.addEventListener('visibilitychange', () => { if (document.hidden) { releaseAll(); if (store) store.flush(); } });
  addEventListener('pagehide', () => { if (store) store.flush(); }); // best effort: the tab may be gone before it finishes

  // ---- pointer ----
  const pos = (e) => { sys.input(K.EV_ABS, K.ABS_X, Math.round(e.clientX * scale)); sys.input(K.EV_ABS, K.ABS_Y, Math.round(e.clientY * scale)); };
  const buttons = [K.BTN_LEFT, K.BTN_MIDDLE, K.BTN_RIGHT];
  const locked = () => document.pointerLockElement === canvas;
  canvas.addEventListener('pointermove', (e) => {
    if (locked()) { /* captured: how far it moved, not where it is */
      const dx = Math.round(e.movementX * scale), dy = Math.round(e.movementY * scale);
      if (dx || dy) { sys.input(K.EV_REL, K.REL_X, dx); sys.input(K.EV_REL, K.REL_Y, dy); syn(); }
      return;
    }
    pos(e); syn();
  });
  canvas.addEventListener('pointerdown', (e) => {
    canvas.focus();
    e.preventDefault();
    if (wantLock && !locked()) {
      lockPointer();
      if (!lockRefused) return;            /* this click takes the mouse back, it is not a click in the game */
    }
    if (!locked()) {                      /* pointer capture is refused (an exception) while the pointer is locked */
      try { canvas.setPointerCapture(e.pointerId); } catch { /* not needed */ }
      pos(e);
    }
    if (buttons[e.button]) sys.input(K.EV_KEY, buttons[e.button], 1);
    syn();
  });
  canvas.addEventListener('pointerup', (e) => { if (!locked()) pos(e); if (buttons[e.button]) sys.input(K.EV_KEY, buttons[e.button], 0); syn(); e.preventDefault(); });
  canvas.addEventListener('wheel', (e) => { sys.input(K.EV_REL, K.REL_WHEEL, e.deltaY < 0 ? 1 : -1); syn(); e.preventDefault(); }, { passive: false });
  canvas.addEventListener('contextmenu', (e) => e.preventDefault());
  canvas.focus();
}

// ---- files from the user's disk ----
// A file dropped onto the page, or picked with Ctrl+Alt+O (a key press is the user gesture a
// browser requires before it opens a file picker), is copied into the OS as
// /home/user/Downloads/NAME. Nothing is sent anywhere: the bytes go from the browser
// straight into kernel memory (boot/host.mjs importFile, kernel/remote.c). A second file
// with the same name replaces the first, as cp would.
const IMPORT_DIR = '/home/user/Downloads';
const IMPORT_MAX = 256 * 2 ** 20; // the kernel refuses more (IMPORT_MAX in kernel/remote.c)
const noteEl = document.getElementById('note');
let noteTimer = 0;
function tell(text, ms = 5000) {
  noteEl.textContent = text;
  noteEl.classList.remove('hidden');
  clearTimeout(noteTimer);
  if (ms) noteTimer = setTimeout(() => noteEl.classList.add('hidden'), ms);
}
// A name the OS accepts: no slash or NUL, at most 63 bytes of UTF-8.
function osName(name) {
  let n = name.replace(/[\/\0]/g, '_');
  while (new TextEncoder().encode(n).length > 63) n = [...n].slice(0, -1).join('');
  return n;
}
async function importFiles(files) {
  const sys = window.ucos;
  if (!sys) return;
  for (const f of files) {
    const name = osName(f.name);
    if (f.size > IMPORT_MAX) { tell(`${name} is too big: the limit is ${IMPORT_MAX / 2 ** 20} MB.`); continue; }
    tell(`Copying ${name} to ~/Downloads ...`, 0);
    const r = await sys.importFile(`${IMPORT_DIR}/${name}`, f);
    tell(r === 0 ? `${name} is in ~/Downloads.` : `Could not copy ${name} (error ${-r}).`);
    console.log(`[page] import ${name}: ${f.size} bytes, ${r === 0 ? 'done' : 'error ' + -r}`);
  }
}
const picker = document.createElement('input');
picker.type = 'file';
picker.multiple = true;
picker.hidden = true;
document.body.append(picker);
picker.addEventListener('change', () => { importFiles([...picker.files]); picker.value = ''; canvas.focus(); });
addEventListener('dragover', (e) => { e.preventDefault(); e.dataTransfer.dropEffect = 'copy'; });
addEventListener('drop', (e) => { e.preventDefault(); importFiles([...e.dataTransfer.files]); });

main().catch((e) => { logEl.classList.remove('hidden'); say('\nBoot failed: ' + (e && e.message || e) + '\n'); console.error(e); });
