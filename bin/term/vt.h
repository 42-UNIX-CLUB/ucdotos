// SPDX-License-Identifier: GPL-2.0-or-later
/* vt.h - escape sequence parser and screen model of an xterm-compatible terminal.
 *
 * Pure C11 with no system dependencies (needs malloc, free, memcpy, memmove,
 * memset), so it builds both for UC.OS and natively for the tests.
 * The parser is the Paul Williams VT500 state machine with incremental UTF-8
 * decoding; input may be split anywhere between vt_feed() calls. */
#ifndef VT_H
#define VT_H
#include <stddef.h>
#include <stdint.h>

/* cell attributes */
enum { VT_BOLD = 1, VT_DIM = 2, VT_ITALIC = 4, VT_UNDERLINE = 8, VT_BLINK = 16,
       VT_INVERSE = 32, VT_HIDDEN = 64, VT_STRIKE = 128 };

/* colours: 0 = default, palette index, or direct 24-bit RGB */
#define VT_DEFAULT 0u
#define VT_PAL(n) (0x01000000u | ((unsigned)(n) & 255u))
#define VT_RGB(r, g, b) (0x02000000u | (((unsigned)(r) & 255u) << 16) | (((unsigned)(g) & 255u) << 8) | ((unsigned)(b) & 255u))
#define VT_IS_PAL(c) (((c) >> 24) == 1)
#define VT_IS_RGB(c) (((c) >> 24) == 2)

typedef struct {
  uint32_t ch;      /* Unicode code point, ' ' when blank */
  uint32_t fg, bg;  /* VT_DEFAULT, VT_PAL(n) or VT_RGB(r,g,b) */
  uint16_t attr;    /* VT_BOLD ... */
  uint16_t pad;
} vt_cell;

/* mouse reporting modes (vt.mouse) */
enum { VT_MOUSE_NONE = 0, VT_MOUSE_X10 = 9, VT_MOUSE_PRESS = 1000, VT_MOUSE_DRAG = 1002, VT_MOUSE_ANY = 1003 };

#define VT_MAXP 32
#define VT_OSC_MAX 1024

typedef struct {
  int x, y, pending, origin, gl;
  vt_cell pen;
  uint8_t cs[4];
} vt_saved;

typedef struct vt {
  int cols, rows;
  vt_cell **scr;           /* the visible screen: main or alt */
  vt_cell **main, **alt;
  int alt_active;
  uint8_t *dirty;          /* per row, set on change; the renderer clears it */
  int all_dirty;           /* everything must be redrawn (view moved, resize, reset) */
  uint8_t *tabs;           /* per column */

  /* cursor and pen */
  int x, y, pending;       /* pending = the "last column" wrap flag */
  vt_cell pen;
  int top, bot;            /* scroll region, inclusive */
  vt_saved saved[2];       /* DECSC per screen */
  uint8_t cs[4];           /* G0..G3 designations: 'B' ASCII, '0' DEC special graphics */
  int gl;                  /* 0 = G0 in GL, 1 = G1 (SO/SI) */
  uint32_t last_ch;        /* for REP */

  /* modes */
  int autowrap, origin, insert, newline, appcursor, appkeypad, cursor_visible;
  int mouse, mouse_sgr, focus_events, bracketed_paste, cursor_style;

  /* scrollback: a ring of lines, each with its own width */
  vt_cell **sb;
  int *sb_w;
  int sb_cap, sb_len, sb_head; /* sb_head = index of the oldest line */
  int view;                    /* lines scrolled back from the bottom (0 = live) */

  /* parser */
  int state;
  int params[VT_MAXP];     /* -1 = empty (default) */
  uint8_t sub[VT_MAXP];    /* 1 if the parameter followed ':' (sub-parameter) */
  int np;
  char prefix;             /* private marker < = > ? or 0 */
  char inter[2];
  int ninter;
  char osc[VT_OSC_MAX];
  int osc_len;
  uint32_t u_cp;           /* UTF-8 decoder */
  int u_need;
  uint8_t u_lo, u_hi;      /* allowed range of the next continuation byte */

  /* outputs for the front end */
  char title[256];
  int title_changed;
  int bell;
  char reply[4096];        /* bytes to send back to the application (DSR, DA ...) */
  int reply_len;
} vt;

vt *vt_new(int cols, int rows, int scrollback_lines);
void vt_free(vt *t);
void vt_feed(vt *t, const void *data, size_t len);
void vt_resize(vt *t, int cols, int rows);
void vt_reset(vt *t);                       /* RIS: full reset (keeps scrollback) */

/* Display rows, taking the scrollback view into account. Returns the cells of
 * row y (0 = top of the window) and their count in *w (may differ from cols
 * for old scrollback lines). */
const vt_cell *vt_view_row(const vt *t, int y, int *w);
void vt_scroll_view(vt *t, int delta);     /* > 0 = back into history */
void vt_view_bottom(vt *t);
int vt_cursor_in_view(const vt *t);        /* cursor visible and on the live screen */

/* Helpers (tests, debugging): row text as UTF-8 with trailing blanks removed. */
int vt_row_text(const vt *t, int y, char *buf, int cap);
int vt_utf8_encode(uint32_t cp, char *out);
void vt_take_reply(vt *t, char *out, int *len);

#endif
