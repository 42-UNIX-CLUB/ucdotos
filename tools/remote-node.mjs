// SPDX-License-Identifier: GPL-2.0-or-later
// Remote files under Node: what the browser fetches next to the page, read from the build
// directory instead (boot/host.mjs, option `remote`). Used by run.mjs and shot.mjs.
//
// Test hooks, for bin/ktest.c: a URL containing ".once." is served on its first request
// only (later requests get HTTP 503), so a second fetch shows up as a failure; a URL
// containing ".flaky." fails its first request and is served after that; a URL containing
// ".slow." is answered after 300 ms, so that two processes can wait for it at once.
import fs from 'node:fs';
import { Readable } from 'node:stream';

//
// Options, for trying the progress display and failures headless (run.mjs and shot.mjs
// pass them from --remote-rate=BYTES_PER_SECOND and --remote-off):
//   rate  send at most this many bytes per second
//   off   every request fails, as when the network is gone
export function nodeRemote(dir, log = () => {}, { rate = 0, off = false } = {}) {
  const seen = new Map();
  return async (url) => {
    if (off) { log(`[remote-node] offline: ${url}\n`); throw new TypeError('fetch failed (--remote-off)'); }
    const n = (seen.get(url) || 0) + 1;
    seen.set(url, n);
    log(`[remote-node] request ${n} for ${url}\n`);
    const path = dir + '/' + url;
    if (url.includes('.slow.')) await new Promise((r) => setTimeout(r, 300));
    if ((url.includes('.once.') && n > 1) || (url.includes('.flaky.') && n === 1)) return new Response(null, { status: 503 });
    if (url.includes('..') || !fs.existsSync(path)) return new Response(null, { status: 404 });
    if (!rate) return new Response(Readable.toWeb(fs.createReadStream(path)), { status: 200 });
    const slow = async function* () {
      for await (const chunk of fs.createReadStream(path, { highWaterMark: 16384 })) {
        await new Promise((r) => setTimeout(r, chunk.length * 1000 / rate));
        yield chunk;
      }
    };
    return new Response(Readable.toWeb(Readable.from(slow())), { status: 200 });
  };
}
