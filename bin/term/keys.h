// SPDX-License-Identifier: GPL-2.0-or-later
/* keys.h - translate key presses (Linux key codes) into xterm byte sequences. */
#ifndef KEYS_H
#define KEYS_H

#define KM_SHIFT 1
#define KM_CTRL 2
#define KM_ALT 4

/* Linux input key codes used by the terminal */
enum {
  K_ESC = 1, K_MINUS = 12, K_EQUAL = 13, K_BACKSPACE = 14, K_TAB = 15, K_LEFTBRACE = 26, K_RIGHTBRACE = 27,
  K_ENTER = 28, K_LEFTCTRL = 29, K_LEFTSHIFT = 42, K_BACKSLASH = 43, K_SLASH = 53, K_RIGHTSHIFT = 54,
  K_LEFTALT = 56, K_SPACE = 57, K_CAPSLOCK = 58, K_F1 = 59, K_F10 = 68, K_F11 = 87, K_F12 = 88,
  K_KPENTER = 96, K_RIGHTCTRL = 97, K_RIGHTALT = 100, K_HOME = 102, K_UP = 103, K_PAGEUP = 104,
  K_LEFT = 105, K_RIGHT = 106, K_END = 107, K_DOWN = 108, K_PAGEDOWN = 109, K_INSERT = 110,
  K_DELETE = 111, K_LEFTMETA = 125, K_RIGHTMETA = 126,
  K_BTN_LEFT = 0x110, K_BTN_RIGHT = 0x111, K_BTN_MIDDLE = 0x112,
};

/* Bytes for a key press, written to out (at least 16 bytes). Returns the length,
 * or 0 when the key only types text (no Ctrl/Alt): the caller then sends the
 * character from the text event instead. appcursor = DECCKM. */
int key_encode(int key, int mods, int appcursor, char *out);
/* 1 for modifier keys (Shift, Ctrl, Alt, Meta, CapsLock) */
int key_is_modifier(int key);

#endif
