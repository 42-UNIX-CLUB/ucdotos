// SPDX-License-Identifier: GPL-2.0-or-later
// The host half of /dev/hostnet (kernel/hostnet.c): network work the page itself can do,
// on behalf of the router (bin/routerd.c). Three requests:
//   resolve  a name to IPv4 addresses: DNS over HTTPS in a browser, the system resolver under Node
//   http     one HTTP/1.x request through fetch(); in a browser it goes out as https and the
//            site must allow cross-origin reads (CORS), or the browser refuses it
//   tcp      a raw TCP stream: only under Node, which has real sockets
// Replies go into kernel memory with hn_alloc/hn_push; hn_end(id, errno) closes them.
//
// Test mode (tools/tests/router-http.mjs): hostnet(getK, { test: 'http://127.0.0.1:PORT' })
// makes the host behave as a browser does (no TCP; HTTP through fetch with redirects
// followed; a failed fetch reported as EACCES, since a browser does not say why) but
// sends every HTTP request to that local server, with the original Host in
// x-forwarded-host. Names ending in .test resolve to 203.0.113.10, except missing.test.
// down.test points at a closed port, to see a failed fetch.
const isNode = typeof process !== 'undefined' && !!(process.versions && process.versions.node);
const net = isNode ? await import('node:net') : null;
const dns = isNode ? await import('node:dns') : null;

const E = { ENOENT: 2, EAGAIN: 11, EACCES: 13, EINVAL: 22, ENETUNREACH: 101, ECONNRESET: 104, ETIMEDOUT: 110, ECONNREFUSED: 111, EHOSTUNREACH: 113 };
const errnoOf = (e) => E[e && e.code] || E.ECONNREFUSED;
// HTTP/2 and HTTP/3 carry no reason phrase; HTTP/1.1 clients expect one.
const REASON = { 200: 'OK', 201: 'Created', 204: 'No Content', 206: 'Partial Content', 301: 'Moved Permanently', 302: 'Found',
  303: 'See Other', 304: 'Not Modified', 307: 'Temporary Redirect', 308: 'Permanent Redirect', 400: 'Bad Request', 401: 'Unauthorized',
  403: 'Forbidden', 404: 'Not Found', 405: 'Method Not Allowed', 410: 'Gone', 429: 'Too Many Requests', 500: 'Internal Server Error',
  502: 'Bad Gateway', 503: 'Service Unavailable', 504: 'Gateway Timeout' };
// Request headers a page may not set (Fetch standard), plus hop-by-hop ones.
const SKIP = /^(host|connection|keep-alive|content-length|transfer-encoding|upgrade|te|trailer|expect|accept-encoding|accept-charset|cookie|cookie2|origin|referer|date|dnt|via|proxy-.*|sec-.*)$/i;

export function hostnet(getK, opts = {}) {
  const test = opts.test || null;
  const browser = !isNode || !!test;     // behave as a browser: no sockets, CORS-style failures
  const chans = new Map();
  const enc = new TextEncoder(), dec = new TextDecoder();
  const kmem = () => getK().memory.buffer;
  function push(c, bytes) {
    if (c.closed) return;
    const k = getK();
    for (let off = 0; off < bytes.length;) {
      const n = Math.min(65536, bytes.length - off);
      const p = k.hn_alloc(n);
      if (!p) { end(c, E.EAGAIN); return; }
      new Uint8Array(k.memory.buffer, p, n).set(bytes.subarray(off, off + n));
      if (k.hn_push(c.id, p, n) <= 0) c.paused = true;
      off += n;
    }
  }
  function end(c, err) {
    if (c.closed || c.ended) return;
    c.ended = true;
    getK().hn_end(c.id, err);
  }
  const waitRoom = (c) => c.paused ? new Promise((r) => { c.resume = r; }) : null;

  async function resolve(c) {
    try {
      let lines = [];
      if (test) {
        if (!c.host.endsWith('.test') || c.host === 'missing.test') { end(c, E.ENOENT); return; }
        lines = ['203.0.113.10 60'];
      } else if (isNode) {
        try { lines = (await dns.promises.resolve4(c.host, { ttl: true })).map((r) => `${r.address} ${r.ttl}`); }
        catch { lines = (await dns.promises.lookup(c.host, { family: 4, all: true })).map((r) => `${r.address} 60`); }
      } else {
        let j = null;
        for (const base of ['https://cloudflare-dns.com/dns-query', 'https://dns.google/resolve']) {
          try {
            const r = await fetch(`${base}?name=${encodeURIComponent(c.host)}&type=A`, { headers: { accept: 'application/dns-json' }, signal: c.abort.signal, credentials: 'omit' });
            if (r.ok) { j = await r.json(); break; }
          } catch (e) { if (c.closed) return; }
        }
        if (!j) { end(c, E.EAGAIN); return; }
        if (j.Status === 3) { end(c, E.ENOENT); return; }
        lines = (j.Answer || []).filter((a) => a.type === 1).map((a) => `${a.data} ${a.TTL}`);
      }
      if (!lines.length) { end(c, E.ENOENT); return; }
      push(c, enc.encode(lines.join('\n') + '\n'));
      end(c, 0);
    } catch (e) {
      end(c, e && (e.code === 'ENOTFOUND' || e.code === 'ENODATA') ? E.ENOENT : E.EAGAIN);
    }
  }

  async function http(c) {
    const raw = concat(c.out);
    c.out = [];
    let head = raw.length, sep = 4;
    for (let i = 0; i + 3 < raw.length; i++) if (raw[i] === 13 && raw[i + 1] === 10 && raw[i + 2] === 13 && raw[i + 3] === 10) { head = i; break; }
    if (head === raw.length) { end(c, E.EINVAL); return; }
    const lines = dec.decode(raw.subarray(0, head)).split('\r\n');
    const [method, target] = lines[0].split(' ');
    const headers = new Headers();
    let host = c.host;
    for (const l of lines.slice(1)) {
      const i = l.indexOf(':');
      if (i < 1) continue;
      const name = l.slice(0, i).trim(), value = l.slice(i + 1).trim();
      if (/^host$/i.test(name)) host = value;
      else if (!SKIP.test(name)) { try { headers.append(name, value); } catch { /* a name or value fetch rejects */ } }
    }
    const body = raw.subarray(head + sep);
    // A page served over https may only fetch https; under Node the request goes out as asked.
    let url;
    if (/^https?:\/\//i.test(target)) url = new URL(target);
    else url = new URL(`http://${host}${target && target.startsWith('/') ? target : '/'}`);
    if (test) {
      headers.set('x-forwarded-host', host);
      url = new URL(url.pathname + url.search, host === 'down.test' ? 'http://127.0.0.1:1' : test);
    } else if (!isNode) { url.protocol = 'https:'; if (url.port === '80') url.port = ''; }
    let res;
    try {
      // credentials: 'omit': a program's request never carries the browser's cookies or
      // stored logins, not even to the page's own host. Code inside the OS is not trusted
      // with the session of whoever opened the page.
      res = await fetch(url, {
        method, headers, signal: c.abort.signal, credentials: 'omit',
        body: method === 'GET' || method === 'HEAD' || !body.length ? undefined : body,
        redirect: browser ? 'follow' : 'manual',
      });
    } catch (e) {
      if (!c.closed) end(c, browser ? E.EACCES : errnoOf(e.cause || e)); // a browser does not say why (CORS, network, mixed content)
      return;
    }
    let out = `HTTP/1.1 ${res.status} ${res.statusText || REASON[res.status] || 'Unknown'}\r\n`;
    res.headers.forEach((v, n) => { if (!/^(content-length|content-encoding|transfer-encoding|connection|keep-alive)$/i.test(n)) out += `${n}: ${v}\r\n`; });
    out += 'connection: close\r\n';
    if (browser) out += `x-ucos-fetched: ${url.href}\r\n`;
    out += '\r\n';
    push(c, enc.encode(out));
    if (res.body && method !== 'HEAD') {
      const reader = res.body.getReader();
      try {
        for (;;) {
          const w = waitRoom(c);
          if (w) await w;
          if (c.closed) { reader.cancel(); return; }
          const { done, value } = await reader.read();
          if (done) break;
          push(c, value);
        }
      } catch (e) { if (!c.closed) end(c, E.ECONNRESET); return; }
    }
    end(c, 0);
  }

  function tcp(c) {
    const s = net.connect({ host: c.host, port: c.port });
    c.sock = s;
    s.on('data', (d) => { push(c, new Uint8Array(d.buffer, d.byteOffset, d.length)); if (c.paused) s.pause(); });
    s.on('end', () => end(c, 0));
    s.on('error', (e) => end(c, errnoOf(e)));
    s.on('close', () => end(c, 0));
  }

  return {
    hn_caps: () => (1 << 1) | (1 << 2) | (net && !browser ? 1 << 3 : 0) | (test ? 1 << 4 : 0),
    hn_open(id, op, port, ptr, len) {
      const c = { id, op, port, host: dec.decode(new Uint8Array(kmem(), ptr, len).slice()), out: [], paused: false, resume: null,
                  closed: false, ended: false, sock: null, abort: new AbortController() };
      chans.set(id, c);
      if (op === 1) resolve(c);
      else if (op === 3) tcp(c);
    },
    hn_send(id, ptr, len) {
      const c = chans.get(id);
      if (!c) return;
      const bytes = new Uint8Array(kmem().slice(ptr, ptr + len));
      if (c.op === 3) c.sock.write(bytes); else c.out.push(bytes);
    },
    hn_done(id) {
      const c = chans.get(id);
      if (!c) return;
      if (c.op === 2 && !c.started) { c.started = true; http(c); }
      else if (c.op === 3) c.sock.end();
    },
    hn_more(id) {
      const c = chans.get(id);
      if (!c) return;
      c.paused = false;
      if (c.sock) c.sock.resume();
      if (c.resume) { const r = c.resume; c.resume = null; r(); }
    },
    hn_close(id) {
      const c = chans.get(id);
      if (!c) return;
      chans.delete(id);
      c.closed = true;
      c.abort.abort();
      if (c.sock) c.sock.destroy();
      if (c.resume) { const r = c.resume; c.resume = null; r(); }
    },
  };
}
function concat(parts) {
  let n = 0;
  for (const p of parts) n += p.length;
  const out = new Uint8Array(n);
  let o = 0;
  for (const p of parts) { out.set(p, o); o += p.length; }
  return out;
}
