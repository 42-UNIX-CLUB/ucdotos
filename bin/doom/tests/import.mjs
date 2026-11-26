// SPDX-License-Identifier: GPL-2.0-or-later
// A user's own data file, on an image without game data (make WADS=/nonexistent):
// Doom from the launcher says no data is installed; the file is then pushed in the way
// the page does it for a dropped file (sys.importFile, into ~/Downloads), and Doom plays it.
//   node tools/shot.mjs --build=B --script=bin/doom/tests/import.mjs   (DOOM_FILE=path to a WAD)
import fs from 'node:fs';
const check = (name, ok) => console.error(`[test] ${ok ? 'ok  ' : 'FAIL'} ${name}`);
const launch = async (t) => { await t.tap('KeyD', { alt: true }); await t.sleep(800); await t.type('doom'); await t.sleep(300); await t.tap('Enter'); };
export default async (t) => {
  const file = process.env.DOOM_FILE || '/home/ensui/uc-os/bin/doom/wads/doom1.wad';
  await t.sleep(3000);
  await launch(t);
  await t.sleep(3000);
  t.shot('import-1-no-data');
  check('Doom says no game data is installed', /No game data is installed/.test(t.log));
  await t.tap('Space');
  await t.sleep(1000);
  t.shot('import-2-closed');
  // the size limit, straight at the kernel: refused before any byte moves
  const k = t.sys.k, path = new TextEncoder().encode('/home/user/Downloads/big.wad');
  new Uint8Array(k.memory.buffer).set(path, k.remote_scratch());
  check('a file over the limit is refused (ENOSPC)', k.import_begin(path.length, 300 * 2 ** 20) === -28);
  // a first copy with wrong bytes, then the real one under the same name: the second replaces it
  check('import a file', await t.sys.importFile('/home/user/Downloads/doom1.wad', new Blob(['not a wad'])) === 0);
  const bytes = fs.readFileSync(file);
  const t0 = Date.now();
  check('import again under the same name', await t.sys.importFile('/home/user/Downloads/doom1.wad', new Blob([bytes])) === 0);
  console.error(`[test] imported ${bytes.length} bytes in ${Date.now() - t0} ms`);
  await launch(t);
  await t.sleep(4000);
  t.shot('import-3-title');
  check('Doom uses the file from ~/Downloads', /adding \/home\/user\/Downloads\/doom1\.wad/.test(t.log) && !/doom: fetching/.test(t.log));
  await t.tap('KeyQ', { alt: true, shift: true });
  await t.sleep(800);
};
