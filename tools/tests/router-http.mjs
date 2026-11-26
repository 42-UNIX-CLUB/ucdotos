// SPDX-License-Identifier: GPL-2.0-or-later
// The router's HTTP path, end to end and offline: a local HTTP server, and UC.OS booted
// under Node with the host in test mode (boot/hostnet.mjs), running bin/routertest.
//   node tools/tests/router-http.mjs [--build=build] [--home-script]
// Prints routertest's output; the last line is "routertest: N checks, M failed".
// --home-script instead boots a shell, starts a router as init does at boot, and runs
// ~/net-tests.sh, the copy of these suites in the image (nettest, then routertest).
import http from 'node:http';
import zlib from 'node:zlib';
import { spawn } from 'node:child_process';

const root = new URL('../..', import.meta.url).pathname;
const build = (process.argv.find((a) => a.startsWith('--build=')) || '--build=build').slice(8);
const big = Buffer.alloc(2 << 20);
for (let i = 0; i < big.length; i++) big[i] = (i * 7 + (i >> 8) * 13) & 255;
const hello = 'hello from the test server\n';

const server = http.createServer((req, res) => {
  const chunks = [];
  req.on('data', (c) => chunks.push(c));
  req.on('end', () => {
    const body = Buffer.concat(chunks);
    switch (req.url) {
      case '/hello': res.writeHead(200, { 'content-type': 'text/plain', 'x-test': 'yes' }); res.end(req.method === 'HEAD' ? undefined : hello); break;
      case '/host': res.writeHead(200, { 'content-type': 'text/plain' }); res.end(req.headers['x-forwarded-host'] || ''); break;
      case '/echo': res.writeHead(200, { 'content-type': 'application/octet-stream' }); res.end(body); break;
      case '/bytes': res.writeHead(200, { 'content-type': 'application/octet-stream' }); res.end(Buffer.from([...Array(256).keys()])); break;
      case '/big': res.writeHead(200, { 'content-type': 'application/octet-stream' }); res.end(big); break;
      case '/redir': res.writeHead(302, { location: '/hello' }); res.end(); break;
      case '/gzip': res.writeHead(200, { 'content-type': 'text/plain', 'content-encoding': 'gzip' }); res.end(zlib.gzipSync('compressed hello\n')); break;
      default: res.writeHead(404, { 'content-type': 'text/plain' }); res.end('no\n');
    }
  });
});
server.listen(0, '127.0.0.1', () => {
  const port = server.address().port;
  const home = process.argv.includes('--home-script');
  const run = home ? ['--init=/bin/sh', '--input=routerd < /dev/null > /dev/null &\nsleep 1\nsh /home/user/net-tests.sh\nexit\n']
                   : ['--init=/bin/routertest'];
  const child = spawn(process.execPath, [root + 'tools/run.mjs', `--build=${build}`, ...run, `--net-test=http://127.0.0.1:${port}`],
                      { cwd: root, stdio: ['ignore', 'inherit', 'inherit'] });
  const timer = setTimeout(() => { console.log('routertest: timed out, 1 checks, 1 failed'); child.kill(); }, 120000);
  child.on('exit', (code) => { clearTimeout(timer); server.close(); process.exit(code || 0); });
});
