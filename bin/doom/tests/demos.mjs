// SPDX-License-Identifier: GPL-2.0-or-later
// Let the built-in demos play for 62 seconds without input; screenshot every
// 10 seconds and look for errors in the log.
// Image: bin/doom/tests/mkimage.sh <wad> -stats
export default async (t) => {
  const prefix = process.env.SHOT_PREFIX || '';
  for (let s = 10; s <= 60; s += 10) {
    await t.sleep(10000);
    t.shot(`${prefix}demo-${s}s`);
  }
  await t.sleep(2000);
  const bad = t.log.match(/.*(desync|different game version|Error|error|trap|unreachable).*/gi);
  console.error('[test] demo log problems: ' + (bad ? bad.join(' | ') : 'none'));
};
