// SPDX-License-Identifier: GPL-2.0-or-later
// Persistence, host half. The kernel (kernel/store.c) decides what to store and hands over
// batches of key/value operations; this file writes each batch to storage as one atomic
// transaction, fetches file contents when the kernel asks, and loads the stored records at
// boot. Two backends behind one interface:
//   idb   the browser: IndexedDB, one transaction per batch (atomic by the browser)
//   file  Node: one append-only log file; a batch counts only once its commit record is
//         written, so a crash at any moment leaves the last complete batch
// Nothing here runs on the system call path: the kernel calls store_kick once when
// changes start to pile up, and the drain runs later from the event loop.

const SCHEMA = 1;
const DELAY_MS = 500;            // write-behind: how long changes may wait when nobody asked
const OP_PUT = 1, OP_DEL = 2, OP_DELRANGE = 3;

// Walk the operations of a batch (after its 12-byte header) or of a log record.
function* ops(b, off = 0, end = b.length) {
  const dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
  while (off < end) {
    const op = b[off], kl = dv.getUint16(off + 1, true);
    const key = b.subarray(off + 3, off + 3 + kl);
    off += 3 + kl;
    if (op === OP_PUT) { const vl = dv.getUint32(off, true); yield { op, key, val: b.subarray(off + 4, off + 4 + vl) }; off += 4 + vl; }
    else if (op === OP_DELRANGE) { const l = dv.getUint16(off, true); yield { op, key, hi: b.subarray(off + 2, off + 2 + l) }; off += 2 + l; }
    else if (op === OP_DEL) yield { op, key };
    else throw new Error('bad store operation ' + op);
  }
}
const u32be = (b, o) => ((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) >>> 0;
const bkey = (sid, n) => { const k = new Uint8Array(9); k[0] = 66; new DataView(k.buffer).setUint32(1, sid); new DataView(k.buffer).setUint32(5, n); return k; };
const BLK = 65536;
// The load blob for the kernel: every record but the data blocks, as PUT operations.
function blobOf(entries) {
  let n = 0;
  for (const [k, v] of entries) n += 7 + k.length + v.length;
  const out = new Uint8Array(n), dv = new DataView(out.buffer);
  let o = 0;
  for (const [k, v] of entries) {
    out[o] = OP_PUT; dv.setUint16(o + 1, k.length, true); out.set(k, o + 3); o += 3 + k.length;
    dv.setUint32(o, v.length, true); out.set(v, o + 4); o += 4 + v.length;
  }
  return out;
}

// ---- browser backend: IndexedDB ----
class IdbBackend {
  async open(name) {
    this.db = await new Promise((res, rej) => {
      const r = indexedDB.open(name, 1);
      r.onupgradeneeded = () => { r.result.createObjectStore('meta'); r.result.createObjectStore('blocks'); };
      r.onsuccess = () => res(r.result);
      r.onerror = () => rej(r.error);
    });
  }
  req(r) { return new Promise((res, rej) => { r.onsuccess = () => res(r.result); r.onerror = () => rej(r.error); }); }
  async load() {
    const tx = this.db.transaction('meta', 'readonly'), st = tx.objectStore('meta');
    const [keys, vals] = await Promise.all([this.req(st.getAllKeys()), this.req(st.getAll())]);
    return keys.map((k, i) => [new Uint8Array(k), new Uint8Array(vals[i])]);
  }
  commit(batch, strict) {
    const tx = this.db.transaction(['meta', 'blocks'], 'readwrite', { durability: strict ? 'strict' : 'relaxed' });
    const meta = tx.objectStore('meta'), blocks = tx.objectStore('blocks');
    const buf = (u) => u.slice().buffer;
    for (const o of ops(batch, 12)) {
      const st = o.key[0] === 66 ? blocks : meta;
      if (o.op === OP_PUT) st.put(buf(o.val), buf(o.key));
      else if (o.op === OP_DEL) st.delete(buf(o.key));
      else st.delete(IDBKeyRange.bound(buf(o.key), buf(o.hi), false, true));
    }
    return new Promise((res, rej) => { tx.oncomplete = res; tx.onerror = tx.onabort = () => rej(tx.error || new Error('aborted')); });
  }
  async fetch(sid, size) {
    const st = this.db.transaction('blocks', 'readonly').objectStore('blocks');
    const vals = await this.req(st.getAll(IDBKeyRange.bound(bkey(sid, 0).buffer, bkey(sid + 1, 0).buffer, false, true)));
    return assemble(vals.map((v) => new Uint8Array(v)), size);
  }
  async put1(key, val) { const tx = this.db.transaction('meta', 'readwrite'); tx.objectStore('meta').put(val.slice().buffer, key.slice().buffer); await new Promise((r, j) => { tx.oncomplete = r; tx.onerror = () => j(tx.error); }); }
  async wipe() { const tx = this.db.transaction(['meta', 'blocks'], 'readwrite'); tx.objectStore('meta').clear(); tx.objectStore('blocks').clear(); await new Promise((r, j) => { tx.oncomplete = r; tx.onerror = () => j(tx.error); }); }
  async usage() { const e = navigator.storage && navigator.storage.estimate ? await navigator.storage.estimate() : {}; return { used: e.usage || 0, quota: e.quota || 0 }; }
}
function assemble(parts, size) {
  const out = new Uint8Array(size);
  let o = 0;
  for (const p of parts) { if (o >= size) break; out.set(p.subarray(0, Math.min(p.length, size - o)), o); o += p.length; }
  if (o < size) throw new Error(`store holds ${o} of ${size} bytes`);
  return out;
}

// ---- Node backend: one append-only log file ----
// File: "UCOSLOG1", then transactions: 'T' u32 seq u32 length, the batch's operations, then
// 'C' u32 seq. A transaction without its commit record (a crash) is cut off at load.
// The index lives in memory: metadata values, and file offsets of data blocks.
class FileBackend {
  constructor(fs, path, crashAfter) { this.fs = fs; this.path = path; this.crashAfter = crashAfter || 0; this.writes = 0; }
  write(fd, buf, pos) {
    if (this.crashAfter && ++this.writes >= this.crashAfter) { // test mode: die in the middle of this write
      this.fs.writeSync(fd, buf, 0, buf.length >> 1, pos);
      process.stdout.write(`\n[store] crash test: stopped at storage write ${this.writes}\n`);
      process.exit(137);
    }
    this.fs.writeSync(fd, buf, 0, buf.length, pos);
  }
  async open() {
    const fs = this.fs;
    if (!fs.existsSync(this.path)) fs.writeFileSync(this.path, 'UCOSLOG1');
    this.fd = fs.openSync(this.path, 'r+');
    this.meta = new Map(); this.blocks = new Map(); // key (latin1) -> value; sid -> Map(block -> [off, len])
    const size = fs.fstatSync(this.fd).size;
    // a buffered reader: metadata is read, block data only skipped (its offset is kept)
    let bufOff = 0, buf = Buffer.alloc(0);
    const at = (off, len) => {
      if (off < bufOff || off + len > bufOff + buf.length) { bufOff = off; buf = Buffer.alloc(Math.max(len, 1 << 14)); /* small: block data is skipped, not read */ buf = buf.subarray(0, fs.readSync(this.fd, buf, 0, buf.length, off)); }
      return buf.subarray(off - bufOff, off - bufOff + len);
    };
    if (size < 8 || at(0, 8).toString('latin1') !== 'UCOSLOG1') throw new Error('not a UC.OS store file');
    let off = 8, good = 8;
    while (off + 9 <= size) {
      const h = at(off, 9);
      if (h[0] !== 84 /* T */) break;
      const len = h.readUInt32LE(5), end = off + 9 + len;
      if (end + 5 > size || at(end, 1)[0] !== 67 /* C */) break; // a torn last transaction
      const ops = [];
      for (let q = off + 9; q < end;) {
        const oh = at(q, 3), op = oh[0], kl = oh.readUInt16LE(1), key = Uint8Array.from(at(q + 3, kl));
        q += 3 + kl;
        if (op === OP_PUT) { const vl = at(q, 4).readUInt32LE(0); ops.push({ op, key, off: q + 4, len: vl, val: key[0] === 66 ? null : Uint8Array.from(at(q + 4, vl)) }); q += 4 + vl; }
        else if (op === OP_DELRANGE) { const l = at(q, 2).readUInt16LE(0); ops.push({ op, key, hi: Uint8Array.from(at(q + 2, l)) }); q += 2 + l; }
        else if (op === OP_DEL) ops.push({ op, key });
        else throw new Error('bad store operation ' + op);
      }
      this.index(ops);
      off = good = end + 5;
    }
    if (good < size) fs.ftruncateSync(this.fd, good);
    this.size = good;
  }
  // apply parsed operations to the index
  index(list) {
    for (const o of list) {
      const kind = o.key[0];
      if (kind === 66) {
        const sid = u32be(o.key, 1), n = u32be(o.key, 5);
        if (o.op === OP_PUT) { let m = this.blocks.get(sid); if (!m) this.blocks.set(sid, (m = new Map())); m.set(n, [o.off, o.len]); }
        else if (o.op === OP_DEL) this.blocks.get(sid)?.delete(n);
        else { const hs = u32be(o.hi, 1), m = this.blocks.get(sid); if (m) { for (const k of m.keys()) if (k >= n) m.delete(k); if (!m.size || hs > sid + 1) this.blocks.delete(sid); } }
      } else {
        const k = Buffer.from(o.key).toString('latin1');
        if (o.op === OP_PUT) this.meta.set(k, o.val);
        else if (o.op === OP_DEL) this.meta.delete(k);
        else { const hi = Buffer.from(o.hi).toString('latin1'); for (const x of [...this.meta.keys()]) if (x >= k && x < hi) this.meta.delete(x); }
      }
    }
  }
  async load() { return [...this.meta].map(([k, v]) => [Buffer.from(k, 'latin1'), v]); }
  async commit(batch) {
    const body = batch.subarray(12), head = Buffer.alloc(9), tail = Buffer.alloc(5);
    head[0] = 84;
    const seq = new DataView(batch.buffer, batch.byteOffset).getUint32(4, true);
    head.writeUInt32LE(seq, 1); head.writeUInt32LE(body.length, 5);
    tail[0] = 67; tail.writeUInt32LE(seq, 1);
    const start = this.size;
    this.write(this.fd, head, start);
    // the operations go out one by one, so that a crash test can stop between any two
    let o = 0;
    for (const op of ops(body)) {
      const end = op.op === OP_PUT ? op.val.byteOffset + op.val.length - body.byteOffset : op.op === OP_DELRANGE ? op.hi.byteOffset + op.hi.length - body.byteOffset : op.key.byteOffset + op.key.length - body.byteOffset;
      this.write(this.fd, body.subarray(o, end), start + 9 + o);
      o = end;
    }
    this.write(this.fd, tail, start + 9 + body.length);
    this.fs.fsyncSync(this.fd);
    this.size = start + 9 + body.length + 5;
    this.index([...ops(body)].map((o) => ({ ...o, off: o.val ? start + 9 + (o.val.byteOffset - body.byteOffset) : 0, len: o.val ? o.val.length : 0, val: o.val && o.key[0] !== 66 ? Uint8Array.from(o.val) : null })));
    if (this.size > 64 << 20 && this.size > 2 * this.live()) await this.compact();
  }
  live() { let n = 0; for (const [k, v] of this.meta) n += k.length + v.length + 7; for (const m of this.blocks.values()) for (const [, l] of m.values()) n += l + 16; return n; }
  readAt(off, len) { const b = Buffer.alloc(len); this.fs.readSync(this.fd, b, 0, len, off); return b; }
  async fetch(sid, size) {
    const m = this.blocks.get(sid), parts = [];
    for (let n = 0; n * BLK < size; n++) { const e = m && m.get(n); if (!e) throw new Error(`block ${n} of ${sid} missing`); parts.push(this.readAt(e[0], e[1])); }
    return assemble(parts, size);
  }
  // Rewrite the live records into a new file, then rename it over the old one (atomic).
  compact() {
    const fs = this.fs, tmp = this.path + '.new';
    const entries = [...this.meta].map(([k, v]) => [Buffer.from(k, 'latin1'), v]);
    for (const [sid, m] of this.blocks) for (const [n, [off, len]] of m) entries.push([bkey(sid, n), this.readAt(off, len)]);
    const body = blobOf(entries), head = Buffer.alloc(9), tail = Buffer.alloc(5);
    head[0] = 84; head.writeUInt32LE(0, 1); head.writeUInt32LE(body.length, 5); tail[0] = 67;
    const fd = fs.openSync(tmp, 'w');
    this.write(fd, Buffer.from('UCOSLOG1'), 0); this.write(fd, head, 8); this.write(fd, body, 17); this.write(fd, tail, 17 + body.length);
    fs.fsyncSync(fd); fs.closeSync(fd);
    fs.renameSync(tmp, this.path);
    fs.closeSync(this.fd);
    return this.open();
  }
  async put1(key, val) { const b = blobOf([[key, val]]), h = new Uint8Array(12 + b.length); h.set(b, 12); new DataView(h.buffer).setUint32(4, 0, true); await this.commit(Buffer.from(h.buffer)); }
  async wipe() { this.fs.ftruncateSync(this.fd, 8); this.size = 8; this.meta.clear(); this.blocks.clear(); }
  async usage() {
    let quota = 0;
    try { const s = this.fs.statfsSync(this.path); quota = this.size + s.bavail * s.bsize; } catch {}
    return { used: this.size, quota };
  }
}

// openStore({ kind: 'idb' | 'file', name, path, fs, crashAfter, readOnly, log }) -> store or null.
// Call before boot(); pass the result to boot({ store }).
export async function openStore(o) {
  const log = o.log || (() => {});
  const be = o.kind === 'file' ? new FileBackend(o.fs, o.path, o.crashAfter) : new IdbBackend();
  await be.open(o.name || 'ucos-store');
  let entries = await be.load();
  const info = entries.find(([k]) => k.length === 1 && k[0] === 73 /* I */);
  if (info && new DataView(info[1].buffer, info[1].byteOffset).getUint32(4, true) !== SCHEMA) {
    log(`[store] the stored files use format ${new DataView(info[1].buffer, info[1].byteOffset).getUint32(4, true)}, this build reads ${SCHEMA}: booting without them, the store is left untouched\n`);
    return null;
  }
  if (!info && !o.readOnly) {
    const v = new Uint8Array(8); v.set([85, 67, 79, 83]); new DataView(v.buffer).setUint32(4, SCHEMA, true);
    await be.put1(new Uint8Array([73]), v);
  }
  entries = entries.filter(([k]) => k[0] !== 73);
  const blob = blobOf(entries);
  let k = null, mem = null, busy = false, timer = 0, urgent = false, stopped = false;
  const stats = { batches: 0, bytes: 0, ms: 0, fetches: 0, fetchMs: 0 };
  async function usage() { try { const u = await be.usage(); k.store_usage(u.used, u.quota); } catch {} }
  async function drain() {
    timer = 0;
    if (busy || stopped || !k) return;
    busy = true;
    try {
      while (k.store_pending()) {
        const strict = urgent; urgent = false;
        const len = k.store_drain(0), ptr = k.store_batch();
        const batch = mem().slice(ptr, ptr + len), seq = new DataView(batch.buffer).getUint32(4, true);
        const t0 = performance.now();
        let ok = 1;
        try { await be.commit(o.kind === 'file' ? Buffer.from(batch.buffer) : batch, strict); }
        catch (e) { ok = 0; log(`[store] writing a batch failed: ${e && e.message}\n`); }
        stats.batches++; stats.bytes += len; stats.ms += performance.now() - t0;
        k.store_done(seq, ok);
        if (!ok) { stopped = true; break; }
      }
      await usage();
    } finally { busy = false; }
    if (k.store_pending() && !stopped) schedule(urgent);
  }
  function schedule(now) {
    if (now) urgent = true;
    if (busy) return;
    if (now) { clearTimeout(timer); timer = 0; Promise.resolve().then(drain); }
    else if (!timer) timer = setTimeout(drain, DELAY_MS);
  }
  return {
    blob, stats, records: entries.length,
    // host imports the kernel calls; none of them calls back into the kernel directly
    imports: {
      store_kick: (u) => schedule(!!u),
      store_fetch: (sid, size) => {
        const t0 = performance.now();
        be.fetch(sid, size).then((data) => {
          const p = k.store_fill_ptr(sid, size);
          if (p || !size) { if (size) mem().set(data, p); k.store_filled(sid, 1); }
          else k.store_filled(sid, 0);
          stats.fetches++; stats.fetchMs += performance.now() - t0;
        }, (e) => { log(`[store] reading inode ${sid}: ${e && e.message}\n`); k.store_filled(sid, 0); });
      },
      store_wipe: () => { stopped = true; clearTimeout(timer); be.wipe().catch((e) => log(`[store] wipe: ${e && e.message}\n`)); },
    },
    // before kmain: hand the stored records to the kernel
    preboot(kernel, put, flags) {
      k = kernel; mem = () => new Uint8Array(k.memory.buffer);
      if (k.store_schema() !== SCHEMA) throw new Error('kernel and boot/store.mjs disagree on the store format');
      k.store_boot(blob.length ? put(blob) : 0, blob.length, (o.readOnly ? 2 : 1) | (flags || 0));
    },
    started() { usage(); },
    flush() { schedule(true); return new Promise((res) => { const t = () => (busy || (k && k.store_pending() && !stopped) ? setTimeout(t, 5) : res()); t(); }); },
    wipe: () => be.wipe(),
  };
}
