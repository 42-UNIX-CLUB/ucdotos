// SPDX-License-Identifier: GPL-2.0-or-later
// The host half of /dev/hostnet (kernel/hostnet.c): network work the page itself can do,
// on behalf of the router (bin/routerd.c). Three requests:
//   resolve  a name to IPv4 addresses: DNS over HTTPS in a browser, the system resolver under Node
//   http     one HTTP/1.x request through fetch(); in a browser it goes out as https and the
//            site must allow cross-origin reads (CORS), or the browser refuses it
//   tcp      a raw TCP stream: under Node, which has real sockets, and in a browser through
//            the platform's relay when a member is signed in
//
// The platform (the ucdotweb service on the page's own origin; opts.platform, filled in by
// boot/page.mjs from GET /api/me) adds two ways out. Both belong to the host layer: the
// session cookie and the relay token never reach a program.
//   proxy    GET, HEAD and POST for the hosts on the platform's list, at /proxy/{host}/{path}.
//            Tried when the direct fetch fails (those hosts send no CORS headers).
//   relay    for members: one WebSocket to /relay carrying Wisp v1 streams, each a TCP
//            connection to port 80 or 443 of a public address. The token comes from
//            POST /api/relay-token and travels in the subprotocol list.
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
// Wisp v1 (https://github.com/MercuryWorkshop/wisp-protocol): one binary message per packet,
//   type u8 | stream id u32 LE | payload.   CONNECT: stream type u8 (1 = TCP) | port u16 LE | host
//   DATA: bytes.   CONTINUE: u32 LE, how many DATA packets we may send.   CLOSE: reason u8
const W_CONNECT = 1, W_DATA = 2, W_CONTINUE = 3, W_CLOSE = 4, W_CHUNK = 65536;
const W_ERRNO = { 0x01: 0, 0x02: 0, 0x03: E.ECONNRESET, 0x41: E.EINVAL, 0x42: E.EHOSTUNREACH, 0x43: E.ETIMEDOUT, 0x44: E.ECONNREFUSED,
  0x47: E.ETIMEDOUT, 0x48: E.EACCES, 0x49: E.EAGAIN };
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
  // The platform: { base, signedIn, member }. base is '' in a page (same origin). A test under
  // Node gives an absolute address, the session cookie (Node keeps none), asBrowser (no
  // sockets of our own) and proxyFirst (Node has no CORS, so the direct fetch never fails).
  const browser = !isNode || !!test || !!(opts.platform && opts.platform.asBrowser); // no sockets, CORS-style failures
  const platform = opts.platform && opts.platform.signedIn ? opts.platform : null;
  const pbase = platform ? (platform.base || '') : '';
  const pcred = platform && platform.cookie ? { headers: { cookie: platform.cookie } } : {};
  const relayOn = !!(platform && platform.member && typeof WebSocket === 'function');
  const proxied = new Set();             // hosts that only answered through the proxy: ask it first
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
    const payload = method === 'GET' || method === 'HEAD' || !body.length ? undefined : body;
    const canProxy = platform && !test && /^(GET|HEAD|POST)$/.test(method) && (!url.port || url.port === '80' || url.port === '443');
    let res = null;
    if (canProxy && (proxied.has(url.hostname) || platform.proxyFirst)) res = await viaProxy(c, method, url, headers, payload);
    if (c.closed) return;
    if (!res) {
      try {
        // credentials: 'omit': a program's request never carries the browser's cookies or
        // stored logins, not even to the page's own host. Code inside the OS is not trusted
        // with the session of whoever opened the page.
        res = await fetch(url, {
          method, headers, signal: c.abort.signal, credentials: 'omit', body: payload,
          redirect: browser ? 'follow' : 'manual',
        });
      } catch (e) {
        if (c.closed) return;
        // a browser does not say why (CORS, network, mixed content): the proxy may still carry it
        if (canProxy && !proxied.has(url.hostname)) {
          res = await viaProxy(c, method, url, headers, payload);
          if (c.closed) return;
          if (res) proxied.add(url.hostname);
        }
        if (!res) { end(c, browser ? E.EACCES : errnoOf(e.cause || e)); return; }
      }
    }
    let out = `HTTP/1.1 ${res.status} ${res.statusText || REASON[res.status] || 'Unknown'}\r\n`;
    res.headers.forEach((v, n) => { if (!/^(content-length|content-encoding|transfer-encoding|connection|keep-alive)$/i.test(n)) out += `${n}: ${v}\r\n`; });
    out += 'connection: close\r\n';
    if (browser || res.viaProxy) out += `x-ucos-fetched: ${url.href}${res.viaProxy ? ' (through the platform\'s proxy)' : ''}\r\n`;
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

  // One request through the platform's proxy. Returns the response, or null when the proxy
  // will not carry it (not signed in any more, or the host or path is not on its list), so
  // that the caller can say what the direct fetch said. Its other refusals (a quota, a
  // switch, a failed upstream) go to the program as they are: their text says why.
  async function viaProxy(c, method, url, headers, payload) {
    // The session goes with this request, so the address must stay inside /proxy/{host}/. A
    // program chooses the host name: ".." in its place would make the address a page of the
    // platform itself (its API, with the session). Only real names pass, and the finished
    // address is checked once more.
    const name = url.hostname;
    if (!/^[a-z0-9_-]+(\.[a-z0-9_-]+)*\.?$/i.test(name)) return null;
    let at;
    try { at = new URL(`${pbase}/proxy/${name}${url.pathname}${url.search}`, pbase || globalThis.location.href); } catch { return null; }
    if (!at.pathname.startsWith(`/proxy/${name}/`)) return null;
    const h = new Headers(headers);
    h.set('x-ucos-proxy', '1');
    if (pcred.headers) h.set('cookie', pcred.headers.cookie);
    let res;
    try {
      res = await fetch(at, {
        method, headers: h, signal: c.abort.signal, credentials: 'same-origin', body: payload, redirect: 'manual', cache: 'no-store',
      });
    } catch { return null; }
    // X-UCOS-Proxy-Refused names the proxy's own refusal; an older platform has no such header
    const word = res.headers.get('x-ucos-proxy-refused');
    const own = word !== null || !/\bsandbox\b/.test(res.headers.get('content-security-policy') || '');
    if (word !== null ? /^(signed-out|not-listed)$/.test(word)
        : own && (res.status === 401 || res.status === 403 || res.status === 404 || res.type === 'opaqueredirect')) {
      if (res.body) res.body.cancel().catch(() => {});
      return null;
    }
    res.viaProxy = true;
    return res;
  }

  // The members' relay. One WebSocket for every stream, opened at the first connection and
  // again (with a new token) when it has dropped.
  let relay = null;
  function relayGet() {
    if (relay) return relay.ready;
    const R = { ws: null, streams: new Map(), nextId: 1, initial: 0, ready: null };
    relay = R;
    const drop = () => {
      if (relay === R) relay = null;
      for (const s of R.streams.values()) relayEnd(s, E.ECONNRESET);
      R.streams.clear();
    };
    R.ready = (async () => {
      const r = await fetch(`${pbase}/api/relay-token`, { method: 'POST', credentials: 'same-origin', cache: 'no-store',
        headers: pcred.headers ? { ...pcred.headers, origin: pbase } : undefined });
      if (!r.ok) throw new Error('relay token: HTTP ' + r.status);
      const { token } = await r.json();
      const at = new URL(`${pbase}/relay`, globalThis.location ? globalThis.location.href : undefined);
      at.protocol = at.protocol === 'https:' ? 'wss:' : 'ws:';
      const protocols = ['wisp', 'ucos-token.' + token];
      // a page's WebSocket sends the page's Origin by itself; a test under Node has to say it
      const ws = pcred.headers ? new WebSocket(at, { protocols, headers: { origin: pbase } }) : new WebSocket(at, protocols);
      ws.binaryType = 'arraybuffer';
      R.ws = ws;
      await new Promise((ok, no) => {
        ws.onerror = () => no(new Error('relay: cannot open'));
        ws.onclose = () => { no(new Error('relay closed')); drop(); };
        ws.onmessage = (e) => {
          const p = new Uint8Array(e.data);
          if (p.length < 5) return;
          const id = new DataView(p.buffer).getUint32(1, true);
          if (p[0] === W_CONTINUE && id === 0) { R.initial = new DataView(p.buffer).getUint32(5, true); ok(); return; }
          const s = R.streams.get(id);
          if (!s) return;
          if (p[0] === W_DATA) { s.inbox.push(p.subarray(5)); relayIn(s); }
          else if (p[0] === W_CONTINUE) { s.credit = new DataView(p.buffer).getUint32(5, true); relayOut(s); }
          else if (p[0] === W_CLOSE) { R.streams.delete(id); s.open = false; s.err = W_ERRNO[p[5]] ?? E.ECONNRESET; relayIn(s); }
        };
      });
      return R;
    })();
    R.ready.catch(() => drop());
    return R.ready;
  }
  const wpacket = (R, type, id, payload) => {
    const p = new Uint8Array(5 + payload.length);
    p[0] = type;
    new DataView(p.buffer).setUint32(1, id, true);
    p.set(payload, 5);
    try { R.ws.send(p); } catch { /* the socket is closing: its close event ends every stream */ }
  };
  // towards the kernel: what arrived waits here while the kernel has no room
  function relayIn(s) {
    const c = s.c;
    while (s.inbox.length && !c.paused && !c.closed) push(c, s.inbox.shift());
    if (!s.inbox.length && !s.open && s.err !== undefined) relayEnd(s, s.err);
  }
  // towards the relay: at most `credit` DATA packets until its next CONTINUE
  function relayOut(s) {
    while (s.open && s.R && s.credit > 0 && s.c.out.length) {
      const b = s.c.out.shift();
      for (let off = 0; off < b.length; off += W_CHUNK) {
        if (s.credit <= 0) { s.c.out.unshift(b.subarray(off)); return; }
        wpacket(s.R, W_DATA, s.id, b.subarray(off, off + W_CHUNK));
        s.credit--;
      }
    }
  }
  function relayEnd(s, err) { s.open = false; s.inbox = []; end(s.c, err); }
  async function relayTcp(c) {
    const s = { c, R: null, id: 0, credit: 0, inbox: [], open: true, err: undefined };
    c.stream = s;
    let R;
    try { R = await relayGet(); } catch { if (!c.closed) end(c, E.ENETUNREACH); return; }
    if (c.closed) return;
    s.R = R; s.id = R.nextId++; s.credit = R.initial;
    R.streams.set(s.id, s);
    const host = enc.encode(c.host), p = new Uint8Array(3 + host.length);
    p[0] = 1; p[1] = c.port & 255; p[2] = c.port >> 8;
    p.set(host, 3);
    wpacket(R, W_CONNECT, s.id, p);
    relayOut(s);
  }

  function tcp(c) {
    if (relayOn) { relayTcp(c); return; }
    const s = net.connect({ host: c.host, port: c.port });
    c.sock = s;
    s.on('data', (d) => { push(c, new Uint8Array(d.buffer, d.byteOffset, d.length)); if (c.paused) s.pause(); });
    s.on('end', () => end(c, 0));
    s.on('error', (e) => end(c, errnoOf(e)));
    s.on('close', () => end(c, 0));
  }

  return {
    // bits 5 and 6 only say how: TCP is the relay's (ports 80 and 443), HTTP may use the proxy
    hn_caps: () => (1 << 1) | (1 << 2) | ((net && !browser) || relayOn ? 1 << 3 : 0) | (test ? 1 << 4 : 0)
      | (relayOn ? 1 << 5 : 0) | (platform && !test ? 1 << 6 : 0),
    hn_open(id, op, port, ptr, len) {
      const c = { id, op, port, host: dec.decode(new Uint8Array(kmem(), ptr, len).slice()), out: [], paused: false, resume: null,
                  closed: false, ended: false, sock: null, stream: null, abort: new AbortController() };
      chans.set(id, c);
      if (op === 1) resolve(c);
      else if (op === 3) tcp(c);
    },
    hn_send(id, ptr, len) {
      const c = chans.get(id);
      if (!c) return;
      const bytes = new Uint8Array(kmem().slice(ptr, ptr + len));
      if (c.op === 3 && c.sock) c.sock.write(bytes);
      else { c.out.push(bytes); if (c.stream) relayOut(c.stream); }
    },
    hn_done(id) {
      const c = chans.get(id);
      if (!c) return;
      if (c.op === 2 && !c.started) { c.started = true; http(c); }
      else if (c.op === 3 && c.sock) c.sock.end(); // Wisp has no half-close: a relayed stream stays open both ways
    },
    hn_more(id) {
      const c = chans.get(id);
      if (!c) return;
      c.paused = false;
      if (c.sock) c.sock.resume();
      if (c.stream) relayIn(c.stream);
      if (c.resume) { const r = c.resume; c.resume = null; r(); }
    },
    hn_close(id) {
      const c = chans.get(id);
      if (!c) return;
      chans.delete(id);
      c.closed = true;
      c.abort.abort();
      if (c.sock) c.sock.destroy();
      const s = c.stream;
      if (s && s.open && s.R) { s.open = false; s.R.streams.delete(s.id); wpacket(s.R, W_CLOSE, s.id, new Uint8Array([2])); }
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
