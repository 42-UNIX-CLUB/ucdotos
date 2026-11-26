// SPDX-License-Identifier: GPL-2.0-or-later
// Prefix each line of standard input with the seconds since this started.
// Used to time steps of a run inside UC.OS:  node tools/run.mjs ... | node stamp.mjs
const t0 = Date.now();
let buf = '';
process.stdin.on('data', (d) => {
  buf += d;
  let i;
  while ((i = buf.indexOf('\n')) >= 0) {
    console.log(((Date.now() - t0) / 1000).toFixed(3).padStart(8) + '  ' + buf.slice(0, i));
    buf = buf.slice(i + 1);
  }
});
process.stdin.on('end', () => { if (buf) console.log(buf); });
