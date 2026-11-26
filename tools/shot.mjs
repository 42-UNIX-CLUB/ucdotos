// SPDX-License-Identifier: GPL-2.0-or-later
// Boot the system headless under Node, drive it with scripted input, and save
// screenshots of the kernel framebuffer as PNG files. For testing the desktop.
//
//   node tools/shot.mjs [--build=build] [--init=/bin/init] [--size=1280x720] [--out=build/shots]
//                       [--script=path/to/script.mjs] [--wait=1500] [--debug] [--quiet]
//
// Without --script: waits --wait milliseconds and writes <out>/screen.png.
// With --script: the module's default export is `async (t) => { ... }` where t has:
//   t.sleep(ms)                 wait
//   t.shot(name)                write <out>/<name>.png, returns its path
//   t.key(code, down)           press or release a key by KeyboardEvent.code name ('KeyA', 'Enter', 'AltLeft'...)
//   t.tap(code, {shift,ctrl,alt,meta})   press and release, with modifiers held
//   t.type('text\n')            type ASCII text (US layout)
//   t.move(x, y)                move the pointer
//   t.click(x, y, button=0)     press and release a mouse button at x, y
//   t.wheel(dy)                 scroll
//   t.resize(w, h)              change the screen size
//   t.pixel(x, y)               -> [r, g, b, a] from the framebuffer
//   t.log                       everything the console printed so far (string)
//   t.sys                       the booted system (see boot/host.mjs)
import fs from 'node:fs';
import zlib from 'node:zlib';
import { boot, checkSupport } from '../boot/host.mjs';
import * as K from '../boot/keys.mjs';

const root = new URL('..', import.meta.url).pathname;
const args = process.argv.slice(2);
const opt = (n, d) => { const a = args.find((x) => x.startsWith(`--${n}=`)); return a ? a.slice(n.length + 3) : d; };
const missing = checkSupport();
if (missing.length) { console.error('missing: ' + missing.join(', ')); process.exit(2); }
const build = opt('build', 'build');
const [W, H] = opt('size', '1280x720').split('x').map(Number);
const outDir = opt('out', root + build + '/shots');
fs.mkdirSync(outDir, { recursive: true });
const quiet = args.includes('--quiet');
let logText = '';

const sys = await boot({
  kernel: fs.readFileSync(root + build + '/kernel.wasm'),
  initrd: fs.readFileSync(root + build + '/initrd.tar'),
  cmdline: 'init=' + opt('init', '/bin/init'),
  screen: { width: W, height: H },
  log: (s) => { logText += s; if (!quiet) process.stdout.write(s); },
  debug: args.includes('--debug'),
  onIdle: () => { console.error('[shot] every process has exited'); },
});

function png(rgba, w, h) {
  const raw = Buffer.alloc((w * 4 + 1) * h);
  for (let y = 0; y < h; y++) {
    raw[y * (w * 4 + 1)] = 0;
    for (let x = 0; x < w; x++) { // force opaque: the screen has no alpha
      const s = (y * w + x) * 4, d = y * (w * 4 + 1) + 1 + x * 4;
      raw[d] = rgba[s]; raw[d + 1] = rgba[s + 1]; raw[d + 2] = rgba[s + 2]; raw[d + 3] = 255;
    }
  }
  const chunk = (type, data) => {
    const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type), data]);
    const crc = Buffer.alloc(4); crc.writeUInt32BE(zlib.crc32(td) >>> 0);
    return Buffer.concat([len, td, crc]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 6;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const syn = () => sys.input(K.EV_SYN, 0, 0);
const t = {
  sys, sleep,
  get log() { return logText; },
  shot(name = 'screen') {
    const f = sys.frame();
    const px = new Uint8Array(sys.memory.buffer, f.addr, f.width * f.height * 4);
    const path = `${outDir}/${name}.png`;
    fs.writeFileSync(path, png(px, f.width, f.height));
    console.error(`[shot] ${path} (${f.width}x${f.height})`);
    return path;
  },
  pixel(x, y) {
    const f = sys.frame();
    const px = new Uint8Array(sys.memory.buffer, f.addr + (y * f.width + x) * 4, 4);
    return [px[0], px[1], px[2], px[3]];
  },
  key(code, down, text = 0) {
    if (!(code in K.KEY)) throw new Error('unknown key ' + code);
    sys.input(K.EV_KEY, K.KEY[code], down ? 1 : 0);
    if (down && text) sys.input(K.EV_MSC, K.MSC_UNICODE, text);
    syn();
  },
  async tap(code, mods = {}, text = 0) {
    const held = [mods.ctrl && 'ControlLeft', mods.alt && 'AltLeft', mods.shift && 'ShiftLeft', mods.meta && 'MetaLeft'].filter(Boolean);
    for (const m of held) t.key(m, true);
    t.key(code, true, (mods.ctrl || mods.alt || mods.meta) ? 0 : text);
    await sleep(4);
    t.key(code, false);
    for (const m of held.reverse()) t.key(m, false);
    await sleep(4);
  },
  async type(text) {
    for (const ch of text) {
      const k = K.keyForChar(ch);
      if (!k) throw new Error('cannot type ' + JSON.stringify(ch));
      await t.tap(k.code, { shift: k.shift }, k.text);
    }
  },
  move(x, y) { sys.input(K.EV_ABS, K.ABS_X, x); sys.input(K.EV_ABS, K.ABS_Y, y); syn(); },
  async click(x, y, button = 0) {
    t.move(x, y);
    const b = [K.BTN_LEFT, K.BTN_MIDDLE, K.BTN_RIGHT][button];
    sys.input(K.EV_KEY, b, 1); syn();
    await sleep(8);
    sys.input(K.EV_KEY, b, 0); syn();
    await sleep(8);
  },
  wheel(dy) { sys.input(K.EV_REL, K.REL_WHEEL, dy); syn(); },
  resize(w, h) { sys.resize(w, h); },
};

const script = opt('script', null);
if (script) {
  const mod = await import(new URL(script, 'file://' + process.cwd() + '/').href);
  await mod.default(t);
} else {
  await sleep(Number(opt('wait', 1500)));
  t.shot('screen');
}
process.exit(0);
