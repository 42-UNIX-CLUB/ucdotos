// SPDX-License-Identifier: GPL-2.0-or-later
// Start a new game from the menu, walk, turn, fire, open the automap, save a
// game, and check through the -selftest log that the save file was written.
// Image: bin/doom/tests/mkimage.sh doom1.wad -stats -selftest
const hold = async (t, keys, ms) => {
  for (const k of keys) t.key(k, true);
  await t.sleep(ms);
  for (const k of keys) t.key(k, false);
  await t.sleep(60);
};
export default async (t) => {
  const prefix = process.env.SHOT_PREFIX || '';
  await t.sleep(3000);
  t.shot(prefix + 'title');
  await t.tap('Escape'); await t.sleep(400);
  t.shot(prefix + 'menu');
  await t.tap('Enter'); await t.sleep(400);   // New Game -> episode
  await t.tap('Enter'); await t.sleep(400);   // episode 1 -> skill
  await t.tap('Enter'); await t.sleep(2500);  // skill 3 -> E1M1 (after the screen wipe)
  t.shot(prefix + 'ingame');
  await hold(t, ['ArrowUp'], 1200);           // walk forward
  await hold(t, ['ArrowLeft'], 600);          // turn left
  await hold(t, ['KeyW', 'KeyD'], 900);       // WASD: forward while turning right
  t.shot(prefix + 'walk');
  t.key('ControlLeft', true); await t.sleep(250);
  t.shot(prefix + 'fire');
  await t.sleep(400); t.key('ControlLeft', false); await t.sleep(100);
  await t.tap('Tab'); await t.sleep(500);
  t.shot(prefix + 'automap');
  await t.tap('Tab'); await t.sleep(300);
  // save: Escape, down to "Save Game", slot 1, type a name, Enter
  await t.tap('Escape'); await t.sleep(300);
  for (let i = 0; i < 3; i++) { await t.tap('ArrowDown'); await t.sleep(150); }
  await t.tap('Enter'); await t.sleep(400);
  await t.tap('Enter'); await t.sleep(300);
  await t.type('ucos wasd'); await t.sleep(300);
  t.shot(prefix + 'savename');
  await t.tap('Enter'); await t.sleep(1500);
  t.shot(prefix + 'saved');
  const ok = /selftest: save slot 0: .*doomsav0\.dsg, \d+ bytes/.test(t.log);
  console.error('[test] save file ' + (ok ? 'FOUND' : 'MISSING'));
  // cheat (typed characters reach the cheat code even for W/A/S/D keys)
  await t.type('iddqd'); await t.sleep(400);
  t.shot(prefix + 'cheat');
  // walk away, then load the save (F3 opens the load menu, Enter picks slot 1)
  await hold(t, ['ArrowDown'], 800);
  t.shot(prefix + 'before-load');
  await t.tap('F3'); await t.sleep(400);
  await t.tap('Enter'); await t.sleep(1200);
  t.shot(prefix + 'loaded');
  // pause in the menu for a while, so -stats shows the idle behaviour
  await t.tap('Escape'); await t.sleep(5500);
  t.shot(prefix + 'menu-idle');
  await t.tap('Escape'); await t.sleep(200);
};
