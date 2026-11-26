// SPDX-License-Identifier: GPL-2.0-or-later
// Run -timedemo and wait for its result line.
// Image: bin/doom/tests/mkimage.sh <wad> -timedemo demo1 -stats
export default async (t) => {
  for (let i = 0; i < 240 && !/timed \d+ gametics/.test(t.log); i++) await t.sleep(500);
  const m = t.log.match(/timed (\d+) gametics in (\d+) realtics.*/);
  console.error('[test] ' + (m ? m[0] : 'no timedemo result'));
};
