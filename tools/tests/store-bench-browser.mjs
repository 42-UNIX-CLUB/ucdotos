// SPDX-License-Identifier: GPL-2.0-or-later
// IndexedDB against the Origin Private File System, on the page's main thread (where the
// kernel runs), for the two shapes the store writes: many small records and one large
// file. OPFS's synchronous access handles exist only in Workers, so the main thread uses
// createWritable(), as boot/store.mjs would have to.
//   make B=build dist && node tools/browser.mjs --build=build --query=nostore --script=tools/tests/store-bench-browser.mjs
export default async (t) => {
  await t.sleep(1500);
  const r = await t.eval(`(async () => {
    const out = {};
    const N = 20000, SMALL = 120, BIG = 100 << 20, BLK = 65536;
    const small = crypto.getRandomValues(new Uint8Array(SMALL)), block = crypto.getRandomValues(new Uint8Array(BLK)); /* random: incompressible */
    const now = () => performance.now();
    // ---- IndexedDB: 20,000 small records in 20 transactions of 1,000 (as batches) ----
    await new Promise((r) => { const q = indexedDB.deleteDatabase('bench'); q.onsuccess = q.onerror = q.onblocked = r; });
    const db = await new Promise((res, rej) => { const q = indexedDB.open('bench', 1); q.onupgradeneeded = () => q.result.createObjectStore('kv'); q.onsuccess = () => res(q.result); q.onerror = () => rej(q.error); });
    const done = (tx) => new Promise((r, j) => { tx.oncomplete = r; tx.onerror = tx.onabort = () => j(tx.error); });
    let t0 = now();
    for (let b = 0; b < N / 1000; b++) {
      const tx = db.transaction('kv', 'readwrite', { durability: 'relaxed' }), st = tx.objectStore('kv');
      for (let i = 0; i < 1000; i++) { const k = new Uint8Array(9); k[0] = 66; new DataView(k.buffer).setUint32(1, b * 1000 + i); st.put(small.slice().buffer, k.buffer); }
      await done(tx);
    }
    out.idb_small_ms = now() - t0;
    t0 = now();
    { const tx = db.transaction('kv', 'readwrite', { durability: 'strict' }); tx.objectStore('kv').put(small.slice().buffer, new Uint8Array([1]).buffer); await done(tx); }
    out.idb_one_strict_ms = now() - t0;
    t0 = now();
    { const tx = db.transaction('kv', 'readwrite', { durability: 'relaxed' }); tx.objectStore('kv').put(small.slice().buffer, new Uint8Array([2]).buffer); await done(tx); }
    out.idb_one_relaxed_ms = now() - t0;
    t0 = now();
    { const tx = db.transaction('kv', 'readonly'), st = tx.objectStore('kv'); const ks = await new Promise((r) => { const q = st.getAllKeys(); q.onsuccess = () => r(q.result); }); out.idb_keys = ks.length; }
    out.idb_read_keys_ms = now() - t0;
    // ---- IndexedDB: one 100 MiB file as 64 KiB blocks, 8 MiB per transaction ----
    t0 = now();
    for (let off = 0, n = 0; off < BIG; ) {
      const tx = db.transaction('kv', 'readwrite', { durability: 'relaxed' }), st = tx.objectStore('kv');
      for (let j = 0; j < 128 && off < BIG; j++, off += BLK, n++) { const k = new Uint8Array(9); k[0] = 67; new DataView(k.buffer).setUint32(1, n); st.put(block.slice().buffer, k.buffer); }
      await done(tx);
    }
    out.idb_big_ms = now() - t0;
    t0 = now();
    { const tx = db.transaction('kv', 'readonly'), st = tx.objectStore('kv'); const lo = new Uint8Array(9); lo[0] = 67; const hi = new Uint8Array(9); hi[0] = 68;
      const v = await new Promise((r) => { const q = st.getAll(IDBKeyRange.bound(lo.buffer, hi.buffer, false, true)); q.onsuccess = () => r(q.result); }); out.idb_big_read_blocks = v.length; }
    out.idb_big_read_ms = now() - t0;
    t0 = now();
    { const tx = db.transaction('kv', 'readwrite', { durability: 'relaxed' }); const k = new Uint8Array(9); k[0] = 67; new DataView(k.buffer).setUint32(1, 1599); tx.objectStore('kv').put(block.slice().buffer, k.buffer); await done(tx); }
    out.idb_rewrite_last_block_ms = now() - t0;
    db.close();
    await new Promise((r) => { const q = indexedDB.deleteDatabase('bench'); q.onsuccess = q.onerror = q.onblocked = r; });
    // ---- OPFS from the main thread: 20,000 small files, then one 100 MiB file ----
    if (navigator.storage && navigator.storage.getDirectory) {
      const rootDir = await navigator.storage.getDirectory();
      try { await rootDir.removeEntry('bench', { recursive: true }); } catch {}
      const dir = await rootDir.getDirectoryHandle('bench', { create: true });
      const M = 2000; // 20,000 takes minutes on the main thread; measure 2,000 and say so
      t0 = now();
      for (let i = 0; i < M; i++) { const f = await dir.getFileHandle('f' + i, { create: true }); const w = await f.createWritable(); await w.write(small); await w.close(); }
      out.opfs_small_files = M;
      out.opfs_small_ms = now() - t0;
      const f = await dir.getFileHandle('big', { create: true });
      t0 = now();
      { const w = await f.createWritable(); for (let off = 0; off < BIG; off += BLK) await w.write(block); await w.close(); }
      out.opfs_big_ms = now() - t0;
      t0 = now();
      { const w = await f.createWritable({ keepExistingData: true }); await w.write({ type: 'write', position: BIG - BLK, data: block }); await w.close(); }
      out.opfs_rewrite_last_block_ms = now() - t0;
      t0 = now();
      { const file = await f.getFile(); const b = await file.arrayBuffer(); out.opfs_big_read_bytes = b.byteLength; }
      out.opfs_big_read_ms = now() - t0;
      await rootDir.removeEntry('bench', { recursive: true });
    }
    const e = await navigator.storage.estimate();
    out.quota = e.quota; out.usage_after_cleanup = e.usage;
    return out;
  })()`);
  console.log('store-bench: ' + JSON.stringify(r, null, 1));
};
