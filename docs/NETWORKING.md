# Networking: what works, and what a full TCP/IP connection still needs

State on 2026-11-26. The decisions behind this are DECISIONS.md 18 (our own TCP/IP stack
in the kernel) and 19 (loopback only, other ways out to be added later). How the stack
works inside is ARCHITECTURE.md section 16.

## 1. The picture

```
programs (nc, ping, your code)
   |  socket calls
kernel TCP/IP:  lo 127.0.0.1/8          gw0 10.0.2.15/24 (default route, set up by routerd)
                                           |  every TCP connection to an outside address
                                           v
routerd (bin/routerd.c, a normal program)  transparent listener, DNS on 127.0.0.53
   |  /dev/hostnet: one descriptor per request
host (boot/hostnet.mjs)
   browser: DNS over HTTPS, fetch()          Node: system resolver, fetch(), real TCP sockets
```

Nothing about the router is special to the kernel. The kernel offers three general
things, and `routerd` builds the router out of them:

- **gw0**, an interface with no hardware behind it. A datagram sent through it comes
  straight back into the stack. When gw0 is the default route, traffic for any outside
  address is handed back to the machine itself.
- **Transparent listening** (`IP_TRANSPARENT`, named after Linux's option). A listener with this option
  accepts TCP connections to addresses outside the machine. `getsockname()` on the
  accepted socket gives the address the client wanted. UC.OS adds
  `IP_TRANSPARENT_PORT`, which limits such a listener to one destination port.
- **/dev/hostnet**, a device through which a program asks the host to resolve a name,
  make an HTTP request, or (where the host can) open a TCP connection.

`init` starts `routerd` at boot, before the desktop. It writes its pid to
`/run/routerd.pid` and leaves any terminal's session, so closing a terminal does not stop
it. `routerd -d` stops it and takes gw0 down (no address, no default route). To watch what
it does, run `routerd -d`, then `routerd -v < /dev/null &` (`-v` logs every lookup and
connection, and stays in the terminal's session). A machine booted with another init (the
test suites) has no router until one is started by hand.

## 2. What works today

| | In a browser tab | Under Node (`tools/run.mjs`, `tools/shot.mjs`) |
|---|---|---|
| Address for the machine | 10.0.2.15/24 on gw0, set by routerd | same |
| DNS (`getaddrinfo`, `ping name`, `nc name`) | yes: routerd asks Cloudflare's DNS over HTTPS (Google's as a fallback); both allow any page to ask | yes: the system resolver |
| TCP to any port | no: refused at once | **yes**: each connection becomes a real socket on the computer running Node |
| HTTP on port 80 | yes, one request per connection, sent with `fetch()` **as https**. Works when the site allows cross-origin reads (CORS): many APIs and CDNs do (checked: Wikipedia's API, GitHub's raw files, jsDelivr). Most ordinary web pages do not (checked: example.com, google.com); the client then gets `502 Bad Gateway` with the reason in the body | yes, as a real TCP connection (see above) |
| HTTPS (port 443) from a UC.OS program | no: there is no TLS in UC.OS | the bytes pass through, but no UC.OS program speaks TLS yet |
| UDP to outside addresses | no: `EHOSTUNREACH` | no (only DNS, through routerd) |
| ping to outside addresses | no: "Destination Host Unreachable" | no (Node has no ICMP) |
| Connections from outside into UC.OS | no | no |
| Machine to machine (two UC.OS tabs) | no | no |

Checked automatically, in `make test`:

- `nettest` (89 checks) covers gw0, the transparent listener, the port limit,
  `SO_LINGER`, the DNS client against a test server and `/dev/hostnet`'s capability report.
- `routertest` (24 checks, run by `tools/tests/router-http.mjs`) covers routerd's HTTP
  path end to end, offline. The script starts a local HTTP server and boots UC.OS under
  Node with the host in test mode: it behaves as a browser does (no TCP, HTTP through
  `fetch()` with redirects followed, a failed fetch reported without a reason) but sends
  every request to the local server, and names ending in `.test` resolve to
  203.0.113.10. routertest starts routerd and checks, through ordinary sockets: DNS
  answers and NXDOMAIN; GET with exact body, passed-through headers and `connection:
  close`; the Host header reaching the server; a request written in three pieces; HEAD;
  a 5000-byte POST echoed back unchanged; a binary body; a 2 MiB download intact; 404
  with its reason; a redirect followed; a gzip body decoded; a failed fetch giving the
  502 with its explanation; a chunked request body giving 501; port 22 refused; and
  that routerd stayed idle between requests (29 ms of CPU in the whole run).

The same suites are in the image as `~/net-tests.sh`: it stops the router, runs
`nettest` and `routertest`, and starts the router again. `routertest` needs the host in
test mode, so in a browser tab it says it was skipped; `nettest` runs anywhere. Checked:
in headless Chromium, `nettest` 89 of 89 and routertest skipped; under Node with the test
server (`node tools/tests/router-http.mjs --build=B --home-script`), 89 and 24 of 24.

What the test mode does not cover: the browser's own refusals (cross-origin, mixed
content), the upgrade from http to https, and DNS over HTTPS. Those were checked by hand: under Node, `nc` to example.com:80 returns its real page and
`nc -z` reaches real ports; in headless Chromium (`tools/browser.mjs`), Wikipedia's API
answers `200 OK` with its data, example.com gets the 502 with the reason, port 22 is
refused and `ping 1.1.1.1` reports the host unreachable. Not tried: Firefox, Safari.

There is no web client program yet: requests above were typed by hand with `printf ... | nc`.

## 3. What is still needed for a full TCP/IP connection

A full connection means: any program in any visitor's browser tab can reach any address
and port, with TCP, UDP and ICMP, the way a virtual machine does. In order of what each
step adds:

### 3.1 A way out that carries raw traffic (owners decide)

A browser tab can only use `fetch`, WebSocket, WebTransport and WebRTC, and the browser
enforces cross-origin rules on them. No code in the page gets around that; it is the
browser's security model, not a limit of WebAssembly. So for raw TCP, UDP and ICMP from an
ordinary tab, one of these must exist (all of them plug into routerd as another uplink):

| Uplink | Gives | Needs |
|---|---|---|
| Relay server, Ethernet frames over WebSocket (websockproxy or wsnic style, or our own in C) | everything, including ping and DHCP | someone runs a server; abuse policy; per-visitor isolation |
| Relay server, Wisp (TCP and UDP streams over one WebSocket) | TCP and UDP, no ICMP | someone runs a server |
| Web Serial to a device (a Raspberry Pi in USB gadget mode running `pppd`, or an ESP32) | a real IP link with PPP or SLIP, no server | the device on the visitor's computer; Chromium-based desktop browsers only |
| Chrome Isolated Web App with Direct Sockets | real TCP and UDP from the browser | UC.OS packaged as a signed app; today a developer flag or managed ChromeOS |
| A helper program on the visitor's computer | everything | an install, which the owners have ruled out for now |
| Running UC.OS under Node or as a desktop app | TCP to any port (works today) | it is no longer the website |

Without any of these, an ordinary tab keeps what section 2 lists.

### 3.2 In the kernel

- **Ethernet and ARP**, and a real network card whose other end is an uplink (the relay
  or the serial link), in place of the looped-back gw0. Part of decision 18, not built.
- **DHCP client**, to take the address, gateway and DNS server from the uplink. Today
  routerd sets 10.0.2.15/24 itself.
- **Forwarding**: the kernel does not forward datagrams between interfaces. A relay that
  carries Ethernet does not need it (the kernel sends straight out of the card); a router
  serving several UC.OS machines would.
- **Path MTU discovery**: datagrams leave with "don't fragment" set, so the kernel must
  lower its segment size when an ICMP "fragmentation needed" comes back, or some sites
  will hang. Arriving fragments are dropped today; reassembly may be needed.
- **TCP window scaling** (RFC 7323): without it one connection carries at most 64 KiB
  per round trip, about 640 KB/s at 100 ms. SACK and timestamps help on lossy links.
- **ICMP time exceeded** and a `traceroute`.
- **UDP and ICMP through routerd**: the transparent path is TCP only.

### 3.3 In user space

- **TLS** (owners decide: our own, a library such as BearSSL as an exception to "no
  third-party code", or none). Without it no UC.OS program can speak HTTPS, which most of
  the web now requires. With it, routerd could also accept a UC.OS program's HTTPS in a
  browser: it would end TLS with a certificate authority that only UC.OS trusts, then
  send the request with `fetch()`, which does the real TLS.
- **WebSocket in routerd**: turning an HTTP upgrade into a browser WebSocket would let
  UC.OS programs reach any public WebSocket service from a tab, without cross-origin
  limits.
- **A web client** (`wget` or `curl`-like) and perhaps a text browser.
- **HTTP/1.1 keep-alive and chunked request bodies** in routerd (one request per
  connection today; chunked bodies get `501`).
- **Machine to machine**: BroadcastChannel between tabs of one browser, or WebRTC
  between browsers (signalling by copy and paste or a QR code needs no server).

## 4. Known rough edges

- A TCP connection to an outside port is accepted by the kernel before routerd can
  decide. Under Node that is fine (the host connects for real); if the host connection
  then fails, the client sees a reset rather than "connection refused". In a browser,
  `IP_TRANSPARENT_PORT` keeps everything but port 80 refused at once.
- If routerd stops while gw0 is still the default route, outside addresses are refused
  and names fail with "temporary failure". `routerd -d` puts things back.
- `fetch()` in a browser follows redirects itself, drops `Set-Cookie` and most response
  headers, and decompresses bodies; routerd's answer is what the browser lets it see.
- Both fetches the host makes for programs, the HTTP request and the DNS-over-HTTPS
  lookup, pass `credentials: 'omit'` (`boot/hostnet.mjs`): a program's request never
  carries the browser's cookies or HTTP authentication, not even to the page's own host.
  Checked under Node (every request of the router suite records `omit`); not checked in a
  browser, where it follows from the Fetch standard. The site sets no cookies today. This is
  the first half of decision 30; the admin program's own channel is not built.
  Under Node, redirects are passed to the client as they are.
- The host does not slow down a sender on its side: data a UC.OS program writes to a
  Node socket is buffered by Node without limit.
- A program that keeps the CPU busy without blocking delays the host's network
  callbacks too (the browser and Node run them on the same thread).
