// SPDX-License-Identifier: GPL-2.0-or-later
// Doom's messages for someone who cannot see stderr: a failed download, or no game data.
//   offline:  node tools/shot.mjs --build=B --init=/bin/doom --remote-off --script=bin/doom/tests/message.mjs
//   no data:  an image built with an empty WADS directory, same command without --remote-off
// Screenshot of the message, then a key closes it and Doom exits.
export default async (t) => {
  const name = process.env.SHOT_PREFIX || 'message';
  await t.sleep(3000);
  t.shot(name);
  await t.tap('Space');
  await t.sleep(800);
  const gone = await t.sys.k.proc_state(1) !== 1;
  const msg = (t.log.match(/(Could not fetch[^\n]*|No game data[^\n]*)/) || ['none'])[0];
  console.error(`[test] message: ${msg}`);
  console.error(`[test] ${gone ? 'ok  ' : 'FAIL'} a key closes it and Doom exits`);
};
