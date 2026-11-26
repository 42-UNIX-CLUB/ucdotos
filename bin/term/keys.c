// SPDX-License-Identifier: GPL-2.0-or-later
/* keys.c - key presses to xterm sequences. See keys.h. */
#include "keys.h"
#include <string.h>

/* US layout of Linux key codes 0..57, plain and shifted (0 = no character) */
static const char us_plain[59] = "\0\x1b" "1234567890-=\x7f\tqwertyuiop[]\r\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
static const char us_shift[59] = "\0\x1b" "!@#$%^&*()_+\x7f\tQWERTYUIOP{}\r\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";

int key_is_modifier(int k) {
  return k == K_LEFTCTRL || k == K_RIGHTCTRL || k == K_LEFTSHIFT || k == K_RIGHTSHIFT || k == K_LEFTALT ||
         k == K_RIGHTALT || k == K_LEFTMETA || k == K_RIGHTMETA || k == K_CAPSLOCK || k == 69 || k == 70;
}

static int csi_key(char *o, int mods, int num, char fin) { /* ESC [ num ; m fin, or ESC [ fin */
  int n = 0, m = 1 + ((mods & KM_SHIFT) ? 1 : 0) + ((mods & KM_ALT) ? 2 : 0) + ((mods & KM_CTRL) ? 4 : 0);
  o[n++] = 27; o[n++] = '[';
  if (num >= 10) o[n++] = (char)('0' + num / 10);
  if (num) o[n++] = (char)('0' + num % 10);
  if (m > 1) { if (!num) o[n++] = '1'; o[n++] = ';'; o[n++] = (char)('0' + m); }
  o[n++] = fin;
  return n;
}

/* Ctrl + character: the control byte, or -1 if there is none */
static int ctrl_byte(char c) {
  if (c >= 'a' && c <= 'z') return c - 'a' + 1;
  if (c >= 'A' && c <= 'Z') return c - 'A' + 1;
  switch (c) {
  case ' ': case '2': case '@': return 0;
  case '[': case '3': case '{': return 27;
  case '\\': case '4': case '|': return 28;
  case ']': case '5': case '}': return 29;
  case '^': case '6': case '~': case '`': return 30;
  case '_': case '7': case '/': case '-': return 31;
  case '8': case '?': return 127;
  }
  return -1;
}

int key_encode(int key, int mods, int app, char *o) {
  int alt = mods & KM_ALT, ctrl = mods & KM_CTRL, shift = mods & KM_SHIFT;
  int plainmods = !(mods & (KM_SHIFT | KM_ALT | KM_CTRL));
  int n = 0;
  switch (key) {
  case K_UP: case K_DOWN: case K_RIGHT: case K_LEFT: case K_HOME: case K_END: {
    char fin = key == K_UP ? 'A' : key == K_DOWN ? 'B' : key == K_RIGHT ? 'C' : key == K_LEFT ? 'D' : key == K_HOME ? 'H' : 'F';
    if (plainmods && app) { o[0] = 27; o[1] = 'O'; o[2] = fin; return 3; }
    return csi_key(o, mods, 0, fin);
  }
  case K_INSERT: return csi_key(o, mods, 2, '~');
  case K_DELETE: return csi_key(o, mods, 3, '~');
  case K_PAGEUP: return csi_key(o, mods, 5, '~');
  case K_PAGEDOWN: return csi_key(o, mods, 6, '~');
  case K_F11: return csi_key(o, mods, 23, '~');
  case K_F12: return csi_key(o, mods, 24, '~');
  case K_ENTER: case K_KPENTER:
    if (alt) o[n++] = 27;
    o[n++] = '\r';
    return n;
  case K_BACKSPACE:
    if (alt) o[n++] = 27;
    o[n++] = ctrl ? 8 : 127;
    return n;
  case K_TAB:
    if (shift) { memcpy(o, "\x1b[Z", 3); return 3; }
    if (alt) o[n++] = 27;
    o[n++] = '\t';
    return n;
  case K_ESC:
    if (alt) o[n++] = 27;
    o[n++] = 27;
    return n;
  }
  if (key >= K_F1 && key <= K_F10) {
    static const int num[10] = {11, 12, 13, 14, 15, 17, 18, 19, 20, 21};
    int f = key - K_F1;
    if (f < 4) {
      if (plainmods) { o[0] = 27; o[1] = 'O'; o[2] = (char)('P' + f); return 3; }
      return csi_key(o, mods, 0, (char)('P' + f));
    }
    return csi_key(o, mods, num[f], '~');
  }
  if (!ctrl && !alt) return 0; /* text: wait for the character */
  if (key <= 0 || key >= (int)sizeof us_plain) return 0;
  char c = shift ? us_shift[key] : us_plain[key];
  if (!c || key_is_modifier(key)) return 0;
  if (alt) o[n++] = 27;
  if (ctrl) {
    int b = ctrl_byte(c);
    if (b < 0) b = ctrl_byte(us_plain[key]);
    if (b < 0) return alt ? (o[n++] = c, n) : 0;
    o[n++] = (char)b;
  } else o[n++] = c;
  return n;
}
