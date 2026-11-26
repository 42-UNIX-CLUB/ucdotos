// SPDX-License-Identifier: GPL-2.0-or-later
// Keyboard codes. The kernel's input device speaks Linux evdev, so the host
// translates the browser's physical key names (KeyboardEvent.code) to Linux
// key codes. Shared by the page and by the headless test tools.

export const EV_SYN = 0, EV_KEY = 1, EV_REL = 2, EV_ABS = 3, EV_MSC = 4;
export const MSC_UNICODE = 0x10;  // value = the Unicode code point this key press typed
export const MSC_RESIZE = 0x11;   // value = (width << 16) | height
export const ABS_X = 0, ABS_Y = 1, REL_WHEEL = 8;
export const BTN_LEFT = 0x110, BTN_RIGHT = 0x111, BTN_MIDDLE = 0x112;

export const KEY = {
  Escape: 1, Digit1: 2, Digit2: 3, Digit3: 4, Digit4: 5, Digit5: 6, Digit6: 7, Digit7: 8, Digit8: 9, Digit9: 10, Digit0: 11,
  Minus: 12, Equal: 13, Backspace: 14, Tab: 15,
  KeyQ: 16, KeyW: 17, KeyE: 18, KeyR: 19, KeyT: 20, KeyY: 21, KeyU: 22, KeyI: 23, KeyO: 24, KeyP: 25,
  BracketLeft: 26, BracketRight: 27, Enter: 28, ControlLeft: 29,
  KeyA: 30, KeyS: 31, KeyD: 32, KeyF: 33, KeyG: 34, KeyH: 35, KeyJ: 36, KeyK: 37, KeyL: 38,
  Semicolon: 39, Quote: 40, Backquote: 41, ShiftLeft: 42, Backslash: 43,
  KeyZ: 44, KeyX: 45, KeyC: 46, KeyV: 47, KeyB: 48, KeyN: 49, KeyM: 50,
  Comma: 51, Period: 52, Slash: 53, ShiftRight: 54, NumpadMultiply: 55, AltLeft: 56, Space: 57, CapsLock: 58,
  F1: 59, F2: 60, F3: 61, F4: 62, F5: 63, F6: 64, F7: 65, F8: 66, F9: 67, F10: 68,
  NumLock: 69, ScrollLock: 70, Numpad7: 71, Numpad8: 72, Numpad9: 73, NumpadSubtract: 74,
  Numpad4: 75, Numpad5: 76, Numpad6: 77, NumpadAdd: 78, Numpad1: 79, Numpad2: 80, Numpad3: 81, Numpad0: 82, NumpadDecimal: 83,
  IntlBackslash: 86, F11: 87, F12: 88, NumpadEnter: 96, ControlRight: 97, NumpadDivide: 98, AltRight: 100,
  Home: 102, ArrowUp: 103, PageUp: 104, ArrowLeft: 105, ArrowRight: 106, End: 107, ArrowDown: 108, PageDown: 109,
  Insert: 110, Delete: 111, MetaLeft: 125, MetaRight: 126, ContextMenu: 127,
};

// For tests: which key (and whether Shift is held) types an ASCII character on a US layout.
const plain = "`1234567890-=qwertyuiop[]\\asdfghjkl;'zxcvbnm,./ ";
const shifted = '~!@#$%^&*()_+QWERTYUIOP{}|ASDFGHJKL:"ZXCVBNM<>? ';
const codes = [
  'Backquote', 'Digit1', 'Digit2', 'Digit3', 'Digit4', 'Digit5', 'Digit6', 'Digit7', 'Digit8', 'Digit9', 'Digit0', 'Minus', 'Equal',
  'KeyQ', 'KeyW', 'KeyE', 'KeyR', 'KeyT', 'KeyY', 'KeyU', 'KeyI', 'KeyO', 'KeyP', 'BracketLeft', 'BracketRight', 'Backslash',
  'KeyA', 'KeyS', 'KeyD', 'KeyF', 'KeyG', 'KeyH', 'KeyJ', 'KeyK', 'KeyL', 'Semicolon', 'Quote',
  'KeyZ', 'KeyX', 'KeyC', 'KeyV', 'KeyB', 'KeyN', 'KeyM', 'Comma', 'Period', 'Slash', 'Space',
];
export function keyForChar(ch) {
  if (ch === '\n') return { code: 'Enter', shift: false, text: 0 };
  if (ch === '\t') return { code: 'Tab', shift: false, text: 9 };
  let i = plain.indexOf(ch);
  if (i >= 0) return { code: codes[i], shift: false, text: ch.codePointAt(0) };
  i = shifted.indexOf(ch);
  if (i >= 0) return { code: codes[i], shift: true, text: ch.codePointAt(0) };
  return null;
}
