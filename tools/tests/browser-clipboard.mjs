// SPDX-License-Identifier: GPL-2.0-or-later
// The computer's clipboard in a real browser. In: Ctrl+V makes it the newest clipd entry and
// Ctrl+Shift+V in the terminal pastes it (checked by eye on the screenshots). Out: what is
// copied in UC.OS lands on the browser's clipboard (printed as [test] lines).
//   make B=build-all dist && node tools/browser.mjs --build=build-all --script=tools/tests/browser-clipboard.mjs
export default async (t) => {
  await t.sleep(4000);
  await t.type('clear; clip -c\n');
  await t.sleep(600);
  // 1. the real path: text on the browser's clipboard, then a real Ctrl+V key press
  const wrote = await t.eval(`navigator.clipboard.writeText('real-ctrl-v-text').then(() => 'ok', (e) => String(e))`);
  console.error(`[test] navigator.clipboard.writeText: ${wrote}`);
  await t.tap('KeyV', { ctrl: true, commands: ['paste'] });
  await t.sleep(600);
  await t.type('echo "[$(clip -o)]"\n');
  await t.sleep(800);
  await t.shot('bc01-after-real-ctrl-v');
  // 2. the page's handler, with a paste event made in the page (works whatever the
  // browser allows a page to put on its clipboard)
  const prevented = await t.eval(`(() => {
    const k = new KeyboardEvent('keydown', { code: 'KeyV', key: 'v', ctrlKey: true, bubbles: true, cancelable: true });
    return !document.querySelector('canvas').dispatchEvent(k);
  })()`);
  console.error(`[test] Ctrl+V keydown prevented by the page (must be false): ${prevented}`);
  await t.eval(`(() => {
    const dt = new DataTransfer();
    dt.setData('text/plain', 'synthetic-paste-text');
    document.dispatchEvent(new ClipboardEvent('paste', { clipboardData: dt, bubbles: true, cancelable: true }));
    return true;
  })()`);
  await t.sleep(600);
  await t.type('echo "[$(clip -o)]"; echo P:');
  await t.tap('KeyV', { ctrl: true, shift: true });
  await t.type('\n');
  await t.sleep(800);
  await t.shot('bc02-after-synthetic-paste');

  // 3. out: a real double click and Ctrl+Shift+C put the word on the browser's clipboard.
  // The terminal's grid starts at (22, 62) with 9x19 cells (default config and theme).
  // Headless Chromium will not let a page read its clipboard, so read it through a real paste:
  // a listener of our own records what the browser hands to the paste event.
  await t.eval(`(document.addEventListener('paste', (e) => { window.__pasted = e.clipboardData.getData('text/plain'); }), true)`);
  const read = async () => { await t.eval('window.__pasted = null'); await t.tap('KeyV', { ctrl: true, commands: ['paste'] }); await t.sleep(300); return t.eval('window.__pasted'); };
  await t.type('clear; echo copied-out-word\n');
  await t.sleep(800);
  await t.click(22 + 9 * 3, 62 + 9); // row 0, inside the word
  await t.click(22 + 9 * 3, 62 + 9);
  await t.sleep(200);
  await t.shot('bc03-word-selected');
  await t.tap('KeyC', { ctrl: true, shift: true });
  await t.sleep(800);
  console.error(`[test] browser clipboard after Ctrl+Shift+C (want copied-out-word): ${await read()}`);
  await t.type('echo from-the-clip-command | clip\n');
  await t.sleep(1000);
  console.error(`[test] browser clipboard after clip (want from-the-clip-command + newline): ${JSON.stringify(await read())}`);
  const warned = t.log.split('\n').filter((l) => /could not copy|cannot write/.test(l));
  console.error(`[test] page warnings about copying out: ${warned.length ? warned.join(' | ') : 'none'}`);
};
