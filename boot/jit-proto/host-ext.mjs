// SPDX-License-Identifier: GPL-2.0-or-later
// boot/host.mjs with the translator extension switched on. jit.html maps "host.mjs" to this
// file with an import map, so the ordinary page.mjs runs unchanged.
import { boot as realBoot, checkSupport } from '../host.mjs?real';
import { jitExtension } from './ext.mjs';

export { checkSupport };
export async function boot(opts) {
  const ext = jitExtension();
  globalThis.ucosJit = ext;
  return realBoot({ ...opts, extend: ext.extend });
}
