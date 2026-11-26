// SPDX-License-Identifier: GPL-2.0-or-later
// Worker-process prototype: boot/host.mjs with the worker extension switched on. The page
// desktop.html maps "host.mjs" to this file with an import map, so the ordinary page.mjs
// runs the desktop unchanged and programs built as approach A or B run in Workers.
// The query string makes this URL different from the one the import map rewrites.
import { boot as realBoot, checkSupport } from '../host.mjs?real';
import { workerExtension } from './ext.mjs';

export { checkSupport };
export async function boot(opts) {
  const ext = workerExtension({ onEvent: (what, W, info) => { if (what === 'killed' && info === 'kernel') console.log(`[worker-proto] pid ${W.pid} (${W.mode}) worker terminated by the kernel`); } });
  globalThis.ucosWorkers = ext;
  return realBoot({ ...opts, extend: ext.extend });
}
