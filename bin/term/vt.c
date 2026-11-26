// SPDX-License-Identifier: GPL-2.0-or-later
/* vt.c - escape sequence parser and screen model. See vt.h. */
#include "vt.h"
#include <stdlib.h>
#include <string.h>

enum { S_GROUND, S_ESC, S_ESC_INT, S_CSI_ENTRY, S_CSI_PARAM, S_CSI_INT, S_CSI_IGNORE,
       S_OSC, S_STRING_IGNORE /* DCS passthrough, SOS, PM, APC: consumed until ST */ };

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---------- cells and rows ---------- */

static vt_cell blank_cell(const vt *t) { /* erased cells keep the current background (bce) */
  vt_cell c = {' ', VT_DEFAULT, t->pen.bg, 0, 0};
  return c;
}
static void fill(vt_cell *c, int n, vt_cell v) { for (int i = 0; i < n; i++) c[i] = v; }
static vt_cell *row_new(int cols, vt_cell v) {
  vt_cell *r = malloc(sizeof(vt_cell) * (size_t)(cols > 0 ? cols : 1));
  if (r) fill(r, cols, v);
  return r;
}
static void mark(vt *t, int y0, int y1) { for (int y = imax(y0, 0); y <= y1 && y < t->rows; y++) t->dirty[y] = 1; }
static const vt_cell plain = {' ', VT_DEFAULT, VT_DEFAULT, 0, 0};

/* ---------- scrollback ---------- */

/* Hand a row to the scrollback ring; returns a buffer of `cols` cells to reuse. */
static vt_cell *sb_push(vt *t, vt_cell *row) {
  if (t->sb_cap <= 0) return row;
  if (t->sb_len < t->sb_cap) {
    int i = (t->sb_head + t->sb_len++) % t->sb_cap;
    t->sb[i] = row; t->sb_w[i] = t->cols;
    vt_cell *n = row_new(t->cols, plain);
    if (n) return n;
    t->sb_len--; /* out of memory: keep the row on screen */
    return row;
  }
  int i = t->sb_head;
  vt_cell *old = t->sb[i];
  if (t->sb_w[i] != t->cols) {
    vt_cell *n = realloc(old, sizeof(vt_cell) * (size_t)t->cols);
    if (!n) return row;
    old = n;
  }
  t->sb[i] = row; t->sb_w[i] = t->cols;
  t->sb_head = (t->sb_head + 1) % t->sb_cap;
  if (t->view > 0 && t->view < t->sb_len) t->view++; /* keep the viewed text still */
  return old;
}
static void sb_clear(vt *t) {
  for (int i = 0; i < t->sb_len; i++) free(t->sb[(t->sb_head + i) % t->sb_cap]);
  t->sb_len = t->sb_head = 0;
  if (t->view) { t->view = 0; t->all_dirty = 1; }
}

/* ---------- scrolling ---------- */

static void scroll_up(vt *t, int top, int bot, int n) {
  if (n <= 0 || top > bot) return;
  n = imin(n, bot - top + 1);
  int save = top == 0 && !t->alt_active;
  vt_cell b = blank_cell(t);
  for (int k = 0; k < n; k++) {
    vt_cell *r = t->scr[top];
    if (save) r = sb_push(t, r);
    memmove(&t->scr[top], &t->scr[top + 1], sizeof(vt_cell *) * (size_t)(bot - top));
    fill(r, t->cols, b);
    t->scr[bot] = r;
  }
  mark(t, top, bot);
}
static void scroll_down(vt *t, int top, int bot, int n) {
  if (n <= 0 || top > bot) return;
  n = imin(n, bot - top + 1);
  vt_cell b = blank_cell(t);
  for (int k = 0; k < n; k++) {
    vt_cell *r = t->scr[bot];
    memmove(&t->scr[top + 1], &t->scr[top], sizeof(vt_cell *) * (size_t)(bot - top));
    fill(r, t->cols, b);
    t->scr[top] = r;
  }
  mark(t, top, bot);
}
static void index_down(vt *t) { /* LF, IND */
  if (t->y == t->bot) scroll_up(t, t->top, t->bot, 1);
  else if (t->y < t->rows - 1) t->y++;
}
static void reverse_index(vt *t) {
  if (t->y == t->top) scroll_down(t, t->top, t->bot, 1);
  else if (t->y > 0) t->y--;
}

/* ---------- cursor ---------- */

static void move_to(vt *t, int x, int y) { /* absolute, y relative to the origin */
  int lo = 0, hi = t->rows - 1;
  if (t->origin) { lo = t->top; hi = t->bot; y += t->top; }
  t->x = clamp(x, 0, t->cols - 1);
  t->y = clamp(y, lo, hi);
  t->pending = 0;
}
static void move_rel(vt *t, int dx, int dy) { /* CUU/CUD stop at the margins when inside them */
  int lo = 0, hi = t->rows - 1;
  if (t->y >= t->top && t->y <= t->bot) { lo = t->top; hi = t->bot; }
  if (dy < 0 && t->y < t->top) lo = 0;
  if (dy > 0 && t->y > t->bot) hi = t->rows - 1;
  t->y = clamp(t->y + dy, lo, hi);
  t->x = clamp(t->x + dx, 0, t->cols - 1);
  t->pending = 0;
}
static void save_cursor(vt *t) {
  vt_saved *s = &t->saved[t->alt_active];
  s->x = t->x; s->y = t->y; s->pending = t->pending; s->pen = t->pen;
  s->origin = t->origin; s->gl = t->gl; memcpy(s->cs, t->cs, 4);
}
static void restore_cursor(vt *t) {
  vt_saved *s = &t->saved[t->alt_active];
  t->x = clamp(s->x, 0, t->cols - 1); t->y = clamp(s->y, 0, t->rows - 1);
  t->pending = s->pending && s->x == t->cols - 1;
  t->pen = s->pen; t->origin = s->origin; t->gl = s->gl; memcpy(t->cs, s->cs, 4);
}

/* ---------- printing ---------- */

static const uint16_t dec_graphics[32] = { /* 0x5f .. 0x7e */
  0x00a0, 0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0, 0x00b1, 0x2424, 0x240b,
  0x2518, 0x2510, 0x250c, 0x2514, 0x253c, 0x23ba, 0x23bb, 0x2500, 0x23bc, 0x23bd, 0x251c,
  0x2524, 0x2534, 0x252c, 0x2502, 0x2264, 0x2265, 0x03c0, 0x2260, 0x00a3, 0x00b7};

static int zero_width(uint32_t c) { /* combining marks and invisible format characters: dropped */
  return c >= 0x300 && ((c >= 0x300 && c <= 0x36f) || (c >= 0x483 && c <= 0x489) || (c >= 0x591 && c <= 0x5bd) ||
         (c >= 0x610 && c <= 0x61a) || (c >= 0x64b && c <= 0x65f) || (c >= 0x1ab0 && c <= 0x1aff) ||
         (c >= 0x1dc0 && c <= 0x1dff) || (c >= 0x200b && c <= 0x200f) || (c >= 0x2028 && c <= 0x202e) ||
         (c >= 0x2060 && c <= 0x2064) || (c >= 0x20d0 && c <= 0x20ff) || (c >= 0xfe00 && c <= 0xfe0f) ||
         (c >= 0xfe20 && c <= 0xfe2f) || c == 0xfeff || (c >= 0xe0100 && c <= 0xe01ef));
}

static void put_char(vt *t, uint32_t c) {
  if (c >= 0x5f && c <= 0x7e && t->cs[t->gl] == '0') c = dec_graphics[c - 0x5f];
  if (zero_width(c)) return;
  if (t->pending) {
    if (t->autowrap) { t->x = 0; index_down(t); }
    t->pending = 0;
  }
  vt_cell *row = t->scr[t->y];
  if (t->insert && t->x < t->cols - 1)
    memmove(&row[t->x + 1], &row[t->x], sizeof(vt_cell) * (size_t)(t->cols - 1 - t->x));
  vt_cell v = t->pen;
  v.ch = c;
  row[t->x] = v;
  t->dirty[t->y] = 1;
  t->last_ch = c;
  if (t->x == t->cols - 1) t->pending = 1; /* wrap happens on the next printable */
  else t->x++;
}

/* ---------- erase, insert, delete ---------- */

static void erase(vt *t, int y, int x0, int x1) { /* [x0, x1) */
  x0 = imax(x0, 0); x1 = imin(x1, t->cols);
  if (x0 >= x1) return;
  fill(&t->scr[y][x0], x1 - x0, blank_cell(t));
  t->dirty[y] = 1;
}
static void erase_display(vt *t, int mode) {
  t->pending = 0;
  switch (mode) {
  case 0:
    erase(t, t->y, t->x, t->cols);
    for (int y = t->y + 1; y < t->rows; y++) erase(t, y, 0, t->cols);
    break;
  case 1:
    for (int y = 0; y < t->y; y++) erase(t, y, 0, t->cols);
    erase(t, t->y, 0, t->x + 1);
    break;
  case 2:
    for (int y = 0; y < t->rows; y++) erase(t, y, 0, t->cols);
    break;
  case 3: sb_clear(t); break;
  }
}
static void erase_line(vt *t, int mode) {
  t->pending = 0;
  if (mode == 0) erase(t, t->y, t->x, t->cols);
  else if (mode == 1) erase(t, t->y, 0, t->x + 1);
  else if (mode == 2) erase(t, t->y, 0, t->cols);
}
static void insert_chars(vt *t, int n) {
  vt_cell *r = t->scr[t->y];
  n = imin(n, t->cols - t->x);
  memmove(&r[t->x + n], &r[t->x], sizeof(vt_cell) * (size_t)(t->cols - t->x - n));
  fill(&r[t->x], n, blank_cell(t));
  t->dirty[t->y] = 1; t->pending = 0;
}
static void delete_chars(vt *t, int n) {
  vt_cell *r = t->scr[t->y];
  n = imin(n, t->cols - t->x);
  memmove(&r[t->x], &r[t->x + n], sizeof(vt_cell) * (size_t)(t->cols - t->x - n));
  fill(&r[t->cols - n], n, blank_cell(t));
  t->dirty[t->y] = 1; t->pending = 0;
}
static void insert_lines(vt *t, int n) {
  if (t->y < t->top || t->y > t->bot) return;
  scroll_down(t, t->y, t->bot, n);
  t->x = 0; t->pending = 0;
}
static void delete_lines(vt *t, int n) {
  if (t->y < t->top || t->y > t->bot) return;
  int save_alt = t->alt_active;
  t->alt_active = 1; /* deleted lines never go to the scrollback */
  scroll_up(t, t->y, t->bot, n);
  t->alt_active = save_alt;
  t->x = 0; t->pending = 0;
}

/* ---------- screens and modes ---------- */

static void clear_screen(vt *t, vt_cell **s) {
  for (int y = 0; y < t->rows; y++) fill(s[y], t->cols, plain);
}
static void set_alt(vt *t, int on, int clear) {
  if (on == t->alt_active) return;
  if (on && clear) { vt_cell b = blank_cell(t); for (int y = 0; y < t->rows; y++) fill(t->alt[y], t->cols, b); }
  t->alt_active = on;
  t->scr = on ? t->alt : t->main;
  t->all_dirty = 1;
  if (t->view) t->view = 0;
}
static void reset_tabs(vt *t, int from) {
  for (int x = from; x < t->cols; x++) t->tabs[x] = x && x % 8 == 0;
}
static void soft_reset(vt *t) {
  t->pen = plain;
  t->top = 0; t->bot = t->rows - 1;
  t->autowrap = 1; t->origin = 0; t->insert = 0; t->appcursor = 0; t->appkeypad = 0;
  t->cursor_visible = 1; t->gl = 0; memset(t->cs, 'B', 4);
  t->pending = 0;
  t->saved[0].pen = t->saved[1].pen = plain;
  t->saved[0].x = t->saved[0].y = t->saved[1].x = t->saved[1].y = 0;
  memset(t->saved[0].cs, 'B', 4); memset(t->saved[1].cs, 'B', 4);
  t->saved[0].origin = t->saved[1].origin = t->saved[0].gl = t->saved[1].gl = 0;
  t->saved[0].pending = t->saved[1].pending = 0;
}
void vt_reset(vt *t) {
  set_alt(t, 0, 0);
  soft_reset(t);
  t->newline = 0; t->mouse = 0; t->mouse_sgr = 0; t->focus_events = 0; t->bracketed_paste = 0;
  t->cursor_style = 0;
  clear_screen(t, t->main); clear_screen(t, t->alt);
  reset_tabs(t, 0);
  t->x = t->y = 0;
  t->last_ch = 0;
  t->state = S_GROUND; t->u_need = 0;
  t->all_dirty = 1;
  t->title[0] = 0;
}

static void reply(vt *t, const char *s, int n) {
  if (t->reply_len + n > (int)sizeof t->reply) return;
  memcpy(t->reply + t->reply_len, s, (size_t)n);
  t->reply_len += n;
}
static int fmt_int(char *o, int v) { /* decimal, v >= 0 */
  char tmp[12]; int n = 0, k = 0;
  do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 11);
  while (n) o[k++] = tmp[--n];
  return k;
}
/* reply "ESC [ <pre> a ; b ... <final>" */
static void reply_csi(vt *t, const char *pre, const int *v, int nv, const char *fin) {
  char b[64]; int n = 0;
  b[n++] = 27; b[n++] = '[';
  while (*pre) b[n++] = *pre++;
  for (int i = 0; i < nv; i++) { if (i) b[n++] = ';'; n += fmt_int(b + n, v[i]); }
  while (*fin) b[n++] = *fin++;
  reply(t, b, n);
}

static int mode_get(vt *t, int m) { /* DEC private mode state, for DECRQM: 1 set, 2 reset, 0 unknown */
  switch (m) {
  case 1: return t->appcursor ? 1 : 2;
  case 6: return t->origin ? 1 : 2;
  case 7: return t->autowrap ? 1 : 2;
  case 25: return t->cursor_visible ? 1 : 2;
  case 47: case 1047: case 1049: return t->alt_active ? 1 : 2;
  case 1000: case 1002: case 1003: return t->mouse == m ? 1 : 2;
  case 1004: return t->focus_events ? 1 : 2;
  case 1006: return t->mouse_sgr ? 1 : 2;
  case 2004: return t->bracketed_paste ? 1 : 2;
  case 12: return 2;
  }
  return 0;
}
static void dec_mode(vt *t, int m, int on) {
  switch (m) {
  case 1: t->appcursor = on; break;
  case 6: t->origin = on; move_to(t, 0, 0); break;
  case 7: t->autowrap = on; if (!on) t->pending = 0; break;
  case 9: t->mouse = on ? VT_MOUSE_X10 : 0; break;
  case 12: break; /* cursor blink: ignored */
  case 25: t->cursor_visible = on; t->dirty[t->y] = 1; break;
  case 47: set_alt(t, on, 0); break;
  case 1047:
    if (on) set_alt(t, 1, 1);
    else { if (t->alt_active) clear_screen(t, t->alt); set_alt(t, 0, 0); }
    break;
  case 1048: if (on) save_cursor(t); else restore_cursor(t); break;
  case 1049:
    if (on) { if (!t->alt_active) { save_cursor(t); set_alt(t, 1, 1); } }
    else if (t->alt_active) { set_alt(t, 0, 0); restore_cursor(t); }
    break;
  case 1000: case 1002: case 1003: t->mouse = on ? m : 0; break;
  case 1004: t->focus_events = on; break;
  case 1006: t->mouse_sgr = on; break;
  case 2004: t->bracketed_paste = on; break;
  }
}

/* ---------- SGR ---------- */

static int P(const vt *t, int i, int def) { /* parameter i, def if missing or 0 */
  return (i < t->np && t->params[i] > 0) ? t->params[i] : def;
}
static int P0(const vt *t, int i) { return (i < t->np && t->params[i] > 0) ? t->params[i] : 0; }

/* Extended colour at params[i] (38, 48 or 58). Stores the colour in *c (or leaves it
 * if malformed) and returns the index of the last parameter consumed. */
static int ext_color(const vt *t, int i, uint32_t *c, int *ok) {
  *ok = 0;
  if (i + 1 < t->np && t->sub[i + 1]) { /* 38:5:n  38:2:r:g:b  38:2:cs:r:g:b */
    int a[8], n = 0, j = i + 1;
    for (; j < t->np && t->sub[j]; j++) if (n < 8) a[n++] = t->params[j] < 0 ? 0 : t->params[j];
    if (a[0] == 5 && n >= 2) { *c = VT_PAL(a[1]); *ok = 1; }
    else if (a[0] == 2 && n >= 5) { *c = VT_RGB(a[2], a[3], a[4]); *ok = 1; }
    else if (a[0] == 2 && n == 4) { *c = VT_RGB(a[1], a[2], a[3]); *ok = 1; }
    return j - 1;
  }
  int mode = i + 1 < t->np ? t->params[i + 1] : -1;
  if (mode == 5) {
    if (i + 2 >= t->np) return t->np;
    *c = VT_PAL(t->params[i + 2] < 0 ? 0 : t->params[i + 2]); *ok = 1;
    return i + 2;
  }
  if (mode == 2) {
    if (i + 4 >= t->np) return t->np;
    int r = P0(t, i + 2), g = P0(t, i + 3), b = P0(t, i + 4);
    *c = VT_RGB(r, g, b); *ok = 1;
    return i + 4;
  }
  return t->np; /* unknown: give up on the rest, like xterm */
}

static void sgr(vt *t) {
  if (t->np == 0) { t->pen.fg = t->pen.bg = VT_DEFAULT; t->pen.attr = 0; return; }
  for (int i = 0; i < t->np; i++) {
    int v = t->params[i] < 0 ? 0 : t->params[i];
    uint16_t *a = &t->pen.attr;
    switch (v) {
    case 0: t->pen.fg = t->pen.bg = VT_DEFAULT; *a = 0; break;
    case 1: *a |= VT_BOLD; break;
    case 2: *a |= VT_DIM; break;
    case 3: *a |= VT_ITALIC; break;
    case 4:
      if (i + 1 < t->np && t->sub[i + 1]) { /* 4:0 none, 4:1..5 styles */
        if (t->params[i + 1] == 0) *a &= ~VT_UNDERLINE; else *a |= VT_UNDERLINE;
        while (i + 1 < t->np && t->sub[i + 1]) i++;
      } else *a |= VT_UNDERLINE;
      break;
    case 5: case 6: *a |= VT_BLINK; break;
    case 7: *a |= VT_INVERSE; break;
    case 8: *a |= VT_HIDDEN; break;
    case 9: *a |= VT_STRIKE; break;
    case 21: *a |= VT_UNDERLINE; break; /* double underline */
    case 22: *a &= ~(VT_BOLD | VT_DIM); break;
    case 23: *a &= ~VT_ITALIC; break;
    case 24: *a &= ~VT_UNDERLINE; break;
    case 25: *a &= ~VT_BLINK; break;
    case 27: *a &= ~VT_INVERSE; break;
    case 28: *a &= ~VT_HIDDEN; break;
    case 29: *a &= ~VT_STRIKE; break;
    case 39: t->pen.fg = VT_DEFAULT; break;
    case 49: t->pen.bg = VT_DEFAULT; break;
    case 38: case 48: case 58: {
      uint32_t c = 0; int ok;
      i = ext_color(t, i, &c, &ok);
      if (ok && v == 38) t->pen.fg = c;
      if (ok && v == 48) t->pen.bg = c;
      break;
    }
    default:
      if (v >= 30 && v <= 37) t->pen.fg = VT_PAL(v - 30);
      else if (v >= 40 && v <= 47) t->pen.bg = VT_PAL(v - 40);
      else if (v >= 90 && v <= 97) t->pen.fg = VT_PAL(v - 90 + 8);
      else if (v >= 100 && v <= 107) t->pen.bg = VT_PAL(v - 100 + 8);
      break; /* 53 overline, 59, 73-75 ...: ignored */
    }
    while (i + 1 < t->np && t->sub[i + 1]) i++; /* stray sub-parameters */
  }
}

/* ---------- dispatch ---------- */

static void tab_forward(vt *t, int n) {
  while (n-- > 0) {
    int x = t->x + 1;
    while (x < t->cols - 1 && !t->tabs[x]) x++;
    t->x = imin(x, t->cols - 1);
  }
  t->pending = 0;
}
static void tab_back(vt *t, int n) {
  while (n-- > 0 && t->x > 0) {
    int x = t->x - 1;
    while (x > 0 && !t->tabs[x]) x--;
    t->x = x;
  }
  t->pending = 0;
}

static void execute(vt *t, uint8_t c) { /* C0 controls */
  switch (c) {
  case 7: t->bell++; break;
  case 8: if (t->x > 0) t->x--; t->pending = 0; break;
  case 9: tab_forward(t, 1); break;
  case 10: case 11: case 12:
    index_down(t);
    if (t->newline) t->x = 0;
    t->pending = 0;
    break;
  case 13: t->x = 0; t->pending = 0; break;
  case 14: t->gl = 1; break; /* SO */
  case 15: t->gl = 0; break; /* SI */
  }
}

static void esc_dispatch(vt *t, uint8_t c) {
  if (t->ninter == 1 && (t->inter[0] == '(' || t->inter[0] == ')' || t->inter[0] == '*' || t->inter[0] == '+')) {
    t->cs[t->inter[0] == '(' ? 0 : t->inter[0] == ')' ? 1 : t->inter[0] == '*' ? 2 : 3] = c == '0' ? '0' : 'B';
    return;
  }
  if (t->ninter == 1 && t->inter[0] == '#') {
    if (c == '8') { /* DECALN: fill with E */
      vt_cell e = plain; e.ch = 'E';
      for (int y = 0; y < t->rows; y++) fill(t->scr[y], t->cols, e);
      t->top = 0; t->bot = t->rows - 1;
      move_to(t, 0, 0);
      t->origin = 0;
      t->all_dirty = 1;
    }
    return;
  }
  if (t->ninter) return; /* ESC % G, ESC SP F ...: ignored */
  switch (c) {
  case 'D': index_down(t); t->pending = 0; break;
  case 'E': index_down(t); t->x = 0; t->pending = 0; break;
  case 'M': reverse_index(t); t->pending = 0; break;
  case 'H': t->tabs[t->x] = 1; break;
  case '7': save_cursor(t); break;
  case '8': restore_cursor(t); break;
  case 'c': vt_reset(t); break;
  case '=': t->appkeypad = 1; break;
  case '>': t->appkeypad = 0; break;
  default: break; /* ESC \ (ST) and others */
  }
}

static void csi_dispatch(vt *t, uint8_t c) {
  char pre = t->prefix;
  char in = t->ninter ? t->inter[0] : 0;
  if (pre == '?') {
    if (in == 0 && (c == 'h' || c == 'l')) {
      for (int i = 0; i < t->np; i++) if (t->params[i] >= 0) dec_mode(t, t->params[i], c == 'h');
    } else if (in == '$' && c == 'p') { /* DECRQM */
      int m = P0(t, 0), v[2] = {m, mode_get(t, m)};
      reply_csi(t, "?", v, 2, "$y");
    } else if (in == 0 && c == 'n' && P0(t, 0) == 6) { /* DECXCPR */
      int v[2] = {t->y + 1 - (t->origin ? t->top : 0), t->x + 1};
      reply_csi(t, "?", v, 2, "R");
    } else if (in == 0 && c == 'J') erase_display(t, P0(t, 0));
    else if (in == 0 && c == 'K') erase_line(t, P0(t, 0));
    return;
  }
  if (pre == '>') {
    if (c == 'c' && P0(t, 0) == 0) { int v[3] = {1, 10, 0}; reply_csi(t, ">", v, 3, "c"); }
    return; /* XTMODKEYS etc.: ignored */
  }
  if (pre) return; /* '<' '=' private sequences: ignored */
  if (in == '!' && c == 'p') { soft_reset(t); return; } /* DECSTR */
  if (in == ' ' && c == 'q') { t->cursor_style = P0(t, 0); t->dirty[t->y] = 1; return; } /* DECSCUSR */
  if (in) return;
  int n = P(t, 0, 1);
  switch (c) {
  case '@': insert_chars(t, n); break;
  case 'A': move_rel(t, 0, -n); break;
  case 'B': case 'e': move_rel(t, 0, n); break;
  case 'C': case 'a': move_rel(t, n, 0); break;
  case 'D': move_rel(t, -n, 0); break;
  case 'E': move_rel(t, 0, n); t->x = 0; break;
  case 'F': move_rel(t, 0, -n); t->x = 0; break;
  case 'G': case '`': t->x = clamp(n - 1, 0, t->cols - 1); t->pending = 0; break;
  case 'H': case 'f': move_to(t, P(t, 1, 1) - 1, n - 1); break;
  case 'I': tab_forward(t, n); break;
  case 'J': erase_display(t, P0(t, 0)); break;
  case 'K': erase_line(t, P0(t, 0)); break;
  case 'L': insert_lines(t, n); break;
  case 'M': delete_lines(t, n); break;
  case 'P': delete_chars(t, n); break;
  case 'S': scroll_up(t, t->top, t->bot, n); break;
  case 'T': if (t->np <= 1) scroll_down(t, t->top, t->bot, n); break;
  case 'X': erase(t, t->y, t->x, t->x + n); t->pending = 0; break;
  case 'Z': tab_back(t, n); break;
  case 'b':
    if (t->last_ch) for (int i = 0; i < imin(n, 65535); i++) put_char(t, t->last_ch);
    break;
  case 'c': if (P0(t, 0) == 0) reply(t, "\x1b[?62;1;2;6;7;8;9c", 18); break;
  case 'd': move_to(t, t->x, n - 1); break;
  case 'g':
    if (P0(t, 0) == 0) t->tabs[t->x] = 0;
    else if (P0(t, 0) == 3) memset(t->tabs, 0, (size_t)t->cols);
    break;
  case 'h': case 'l':
    for (int i = 0; i < t->np; i++) {
      if (t->params[i] == 4) t->insert = c == 'h';
      if (t->params[i] == 20) t->newline = c == 'h';
    }
    break;
  case 'm': sgr(t); break;
  case 'n':
    if (P0(t, 0) == 5) reply(t, "\x1b[0n", 4);
    else if (P0(t, 0) == 6) { int v[2] = {t->y + 1 - (t->origin ? t->top : 0), t->x + 1}; reply_csi(t, "", v, 2, "R"); }
    break;
  case 'r': {
    int top = P(t, 0, 1) - 1, bot = P(t, 1, t->rows) - 1;
    bot = imin(bot, t->rows - 1);
    if (top < bot) { t->top = top; t->bot = bot; move_to(t, 0, 0); }
    break;
  }
  case 's': save_cursor(t); break;
  case 'u': restore_cursor(t); break;
  case 't':
    if (P0(t, 0) == 18) { int v[3] = {8, t->rows, t->cols}; reply_csi(t, "", v, 3, "t"); }
    break;
  default: break;
  }
}

static void osc_dispatch(vt *t) {
  int n = 0, i = 0;
  while (i < t->osc_len && t->osc[i] >= '0' && t->osc[i] <= '9' && n < 100000) n = n * 10 + (t->osc[i++] - '0');
  if (i == 0 || i >= t->osc_len || t->osc[i] != ';') return;
  i++;
  if (n == 0 || n == 2) {
    int len = imin(t->osc_len - i, (int)sizeof t->title - 1);
    memcpy(t->title, t->osc + i, (size_t)len);
    t->title[len] = 0;
    t->title_changed = 1;
  }
}

/* ---------- the state machine ---------- */

static void clear_seq(vt *t) {
  t->np = 0; t->prefix = 0; t->ninter = 0;
}
static void param_byte(vt *t, uint8_t c) {
  if (t->np == 0) { t->np = 1; t->params[0] = -1; t->sub[0] = 0; }
  if (c == ';' || c == ':') {
    if (t->np < VT_MAXP) { t->params[t->np] = -1; t->sub[t->np] = c == ':'; t->np++; }
    return; /* extra parameters are dropped */
  }
  int *p = &t->params[t->np - 1];
  if (*p < 0) *p = 0;
  if (*p < 100000) *p = *p * 10 + (c - '0');
}
static void collect(vt *t, uint8_t c) {
  if (t->ninter < 2) t->inter[t->ninter++] = (char)c;
}

static void ground_char(vt *t, uint32_t cp) {
  if (cp >= 0x80 && cp < 0xa0) return; /* C1 controls as UTF-8: ignored */
  put_char(t, cp);
}

/* one byte that is not part of a UTF-8 sequence */
static void byte(vt *t, uint8_t c) {
  /* transitions from anywhere */
  if (c == 0x18 || c == 0x1a) { /* CAN, SUB */
    if (t->state == S_OSC) osc_dispatch(t);
    t->state = S_GROUND;
    return;
  }
  if (c == 0x1b) {
    if (t->state == S_OSC) osc_dispatch(t);
    t->state = S_ESC; clear_seq(t);
    return;
  }
  switch (t->state) {
  case S_GROUND:
    if (c < 0x20) execute(t, c);
    else if (c < 0x7f) put_char(t, c);
    break; /* DEL ignored */
  case S_ESC:
    if (c < 0x20) execute(t, c);
    else if (c < 0x30) { collect(t, c); t->state = S_ESC_INT; }
    else if (c == '[') { clear_seq(t); t->state = S_CSI_ENTRY; }
    else if (c == ']') { t->osc_len = 0; t->state = S_OSC; }
    else if (c == 'P' || c == 'X' || c == '^' || c == '_') t->state = S_STRING_IGNORE;
    else if (c < 0x7f) { esc_dispatch(t, c); t->state = S_GROUND; }
    break;
  case S_ESC_INT:
    if (c < 0x20) execute(t, c);
    else if (c < 0x30) collect(t, c);
    else if (c < 0x7f) { esc_dispatch(t, c); t->state = S_GROUND; }
    break;
  case S_CSI_ENTRY:
  case S_CSI_PARAM:
    if (c < 0x20) execute(t, c);
    else if (c < 0x30) { collect(t, c); t->state = S_CSI_INT; }
    else if (c <= 0x3b) { param_byte(t, c); t->state = S_CSI_PARAM; }
    else if (c <= 0x3f) {
      if (t->state == S_CSI_ENTRY) { t->prefix = (char)c; t->state = S_CSI_PARAM; }
      else t->state = S_CSI_IGNORE;
    } else if (c < 0x7f) { csi_dispatch(t, c); t->state = S_GROUND; }
    break;
  case S_CSI_INT:
    if (c < 0x20) execute(t, c);
    else if (c < 0x30) collect(t, c);
    else if (c < 0x40) t->state = S_CSI_IGNORE;
    else if (c < 0x7f) { csi_dispatch(t, c); t->state = S_GROUND; }
    break;
  case S_CSI_IGNORE:
    if (c < 0x20) execute(t, c);
    else if (c >= 0x40 && c < 0x7f) t->state = S_GROUND;
    break;
  case S_OSC:
    if (c == 7) { osc_dispatch(t); t->state = S_GROUND; }
    else if (c >= 0x20 && t->osc_len < VT_OSC_MAX) t->osc[t->osc_len++] = (char)c;
    break;
  case S_STRING_IGNORE:
    break;
  }
}

static void utf8_fail(vt *t) { t->u_need = 0; ground_char(t, 0xfffd); }

void vt_feed(vt *t, const void *data, size_t len) {
  const uint8_t *s = data;
  for (size_t i = 0; i < len; i++) {
    uint8_t c = s[i];
    if (t->u_need) { /* inside a UTF-8 sequence */
      if (c >= t->u_lo && c <= t->u_hi) {
        t->u_cp = (t->u_cp << 6) | (c & 0x3f);
        t->u_lo = 0x80; t->u_hi = 0xbf;
        if (--t->u_need == 0) ground_char(t, t->u_cp);
        continue;
      }
      utf8_fail(t); /* then handle c afresh */
    }
    if (c < 0x80 || (t->state != S_GROUND)) {
      if (c >= 0x80) { /* bytes >= 0x80 inside sequences: kept in OSC (UTF-8 titles), dropped elsewhere */
        if (t->state == S_OSC && t->osc_len < VT_OSC_MAX) t->osc[t->osc_len++] = (char)c;
        continue;
      }
      if (t->state == S_GROUND && c >= 0x20 && c < 0x7f) {
        /* fast path: a run of printable ASCII that stays on this row */
        if (!t->pending && !t->insert && t->cs[t->gl] != '0') {
          vt_cell *row = t->scr[t->y], v = t->pen;
          size_t j = i;
          while (j < len && t->x < t->cols - 1 && s[j] >= 0x20 && s[j] < 0x7f) { v.ch = s[j++]; row[t->x++] = v; }
          if (j > i) { t->dirty[t->y] = 1; t->last_ch = s[j - 1]; i = j - 1; continue; }
        }
        put_char(t, c);
        continue;
      }
      byte(t, c);
      continue;
    }
    /* a lead byte in the ground state */
    t->u_lo = 0x80; t->u_hi = 0xbf;
    if (c >= 0xc2 && c <= 0xdf) { t->u_need = 1; t->u_cp = c & 0x1f; }
    else if (c >= 0xe0 && c <= 0xef) {
      t->u_need = 2; t->u_cp = c & 0x0f;
      if (c == 0xe0) t->u_lo = 0xa0;
      if (c == 0xed) t->u_hi = 0x9f;
    } else if (c >= 0xf0 && c <= 0xf4) {
      t->u_need = 3; t->u_cp = c & 0x07;
      if (c == 0xf0) t->u_lo = 0x90;
      if (c == 0xf4) t->u_hi = 0x8f;
    } else ground_char(t, 0xfffd); /* stray continuation, C0/C1 lead, F5..FF */
  }
}

/* ---------- life cycle and resize ---------- */

static vt_cell **screen_new(int cols, int rows) {
  vt_cell **s = calloc((size_t)rows, sizeof *s);
  if (!s) return NULL;
  for (int y = 0; y < rows; y++) if (!(s[y] = row_new(cols, plain))) return NULL;
  return s;
}

vt *vt_new(int cols, int rows, int sb_lines) {
  if (cols < 1) cols = 1;
  if (rows < 1) rows = 1;
  vt *t = calloc(1, sizeof *t);
  if (!t) return NULL;
  t->cols = cols; t->rows = rows;
  t->main = screen_new(cols, rows);
  t->alt = screen_new(cols, rows);
  t->dirty = calloc((size_t)rows, 1);
  t->tabs = calloc((size_t)cols, 1);
  t->sb_cap = sb_lines > 0 ? sb_lines : 0;
  if (t->sb_cap) { t->sb = calloc((size_t)t->sb_cap, sizeof *t->sb); t->sb_w = calloc((size_t)t->sb_cap, sizeof(int)); }
  if (!t->main || !t->alt || !t->dirty || !t->tabs || (t->sb_cap && (!t->sb || !t->sb_w))) { vt_free(t); return NULL; }
  t->scr = t->main;
  vt_reset(t);
  return t;
}

static void screen_free(vt_cell **s, int rows) {
  if (!s) return;
  for (int y = 0; y < rows; y++) free(s[y]);
  free(s);
}
void vt_free(vt *t) {
  if (!t) return;
  screen_free(t->main, t->rows);
  screen_free(t->alt, t->rows);
  if (t->sb) sb_clear(t);
  free(t->sb); free(t->sb_w); free(t->dirty); free(t->tabs);
  free(t);
}

/* Give every row of s the new width. */
static int widen(vt_cell **s, int rows, int ocols, int cols) {
  for (int y = 0; y < rows; y++) {
    vt_cell *r = realloc(s[y], sizeof(vt_cell) * (size_t)cols);
    if (!r) return -1;
    if (cols > ocols) fill(r + ocols, cols - ocols, plain);
    s[y] = r;
  }
  return 0;
}

void vt_resize(vt *t, int cols, int rows) {
  if (cols < 1) cols = 1;
  if (rows < 1) rows = 1;
  if (cols == t->cols && rows == t->rows) return;
  int ocols = t->cols, orows = t->rows;
  uint8_t *tabs = realloc(t->tabs, (size_t)cols);
  uint8_t *dirty = realloc(t->dirty, (size_t)rows);
  if (!tabs || !dirty) { if (tabs) t->tabs = tabs; if (dirty) t->dirty = dirty; return; }
  t->tabs = tabs; t->dirty = dirty;
  if (widen(t->main, orows, ocols, cols) || widen(t->alt, orows, ocols, cols)) return;
  t->cols = cols;
  if (cols > ocols) reset_tabs(t, ocols);

  vt_cell **nm = calloc((size_t)rows, sizeof *nm), **na = calloc((size_t)rows, sizeof *na);
  if (!nm || !na) { free(nm); free(na); return; }
  /* main screen: keep the bottom lines with text; when shrinking, lines above the
   * cursor go to the scrollback, blank lines below the cursor are dropped first */
  int cy = t->alt_active ? t->saved[0].y : t->y;
  int used = orows;
  while (used > cy + 1 && used > rows) { /* drop trailing blank rows */
    vt_cell *r = t->main[used - 1];
    int blank = 1;
    for (int x = 0; x < cols && blank; x++) blank = r[x].ch == ' ' && r[x].bg == VT_DEFAULT && !(r[x].attr & VT_INVERSE);
    if (!blank) break;
    free(r); used--;
  }
  int shift = used > rows ? used - rows : 0; /* rows that leave the top */
  for (int y = 0; y < shift; y++) {
    vt_cell *r = t->main[y];
    vt_cell *back = t->sb_cap ? sb_push(t, r) : r;
    if (back != r || !t->sb_cap) free(back);
  }
  int pull = 0; /* growing: bring lines back from the scrollback */
  if (rows > used && !t->alt_active) pull = imin(rows - used, t->sb_len);
  for (int y = 0; y < pull; y++) {
    int idx = (t->sb_head + t->sb_len - pull + y) % t->sb_cap;
    vt_cell *r = t->sb[idx];
    if (t->sb_w[idx] != cols) {
      vt_cell *n = realloc(r, sizeof(vt_cell) * (size_t)cols);
      if (!n) { pull = y; break; }
      if (cols > t->sb_w[idx]) fill(n + t->sb_w[idx], cols - t->sb_w[idx], plain);
      r = n;
    }
    nm[y] = r;
  }
  t->sb_len -= pull;
  int kept = used - shift;
  for (int y = 0; y < kept && pull + y < rows; y++) nm[pull + y] = t->main[shift + y];
  for (int y = pull + kept; y < rows; y++) nm[y] = row_new(cols, plain);
  /* alt screen: anchored at the top */
  for (int y = 0; y < rows; y++) na[y] = y < orows ? t->alt[y] : row_new(cols, plain);
  for (int y = rows; y < orows; y++) free(t->alt[y]);
  for (int y = 0; y < rows; y++) if (!nm[y] || !na[y]) { /* out of memory: unrecoverable rows */
    if (!nm[y]) nm[y] = row_new(cols, plain);
    if (!na[y]) na[y] = row_new(cols, plain);
  }
  free(t->main); free(t->alt);
  t->main = nm; t->alt = na;
  t->scr = t->alt_active ? t->alt : t->main;
  t->rows = rows;
  int dy = pull - shift;
  if (t->alt_active) {
    t->saved[0].y = clamp(t->saved[0].y + dy, 0, rows - 1);
  } else {
    t->y += dy;
    t->saved[0].y = clamp(t->saved[0].y + dy, 0, rows - 1);
  }
  t->x = clamp(t->x, 0, cols - 1);
  t->y = clamp(t->y, 0, rows - 1);
  t->saved[0].x = clamp(t->saved[0].x, 0, cols - 1);
  t->saved[1].x = clamp(t->saved[1].x, 0, cols - 1);
  t->saved[1].y = clamp(t->saved[1].y, 0, rows - 1);
  t->pending = 0;
  t->top = 0; t->bot = rows - 1;
  t->view = 0;
  t->all_dirty = 1;
  memset(t->dirty, 1, (size_t)rows);
}

/* ---------- view ---------- */

const vt_cell *vt_view_row(const vt *t, int y, int *w) {
  int line = y - t->view;
  if (line >= 0 || t->alt_active) { *w = t->cols; return t->scr[clamp(line, 0, t->rows - 1)]; }
  int idx = t->sb_len + line;
  if (idx < 0) { *w = 0; return NULL; }
  idx = (t->sb_head + idx) % t->sb_cap;
  *w = t->sb_w[idx];
  return t->sb[idx];
}
void vt_scroll_view(vt *t, int delta) {
  if (t->alt_active) return;
  int v = clamp(t->view + delta, 0, t->sb_len);
  if (v != t->view) { t->view = v; t->all_dirty = 1; }
}
void vt_view_bottom(vt *t) { if (t->view) { t->view = 0; t->all_dirty = 1; } }
int vt_cursor_in_view(const vt *t) { return t->cursor_visible && t->view == 0; }

/* ---------- helpers ---------- */

int vt_utf8_encode(uint32_t c, char *o) {
  if (c < 0x80) { o[0] = (char)c; return 1; }
  if (c < 0x800) { o[0] = (char)(0xc0 | c >> 6); o[1] = (char)(0x80 | (c & 63)); return 2; }
  if (c < 0x10000) { o[0] = (char)(0xe0 | c >> 12); o[1] = (char)(0x80 | ((c >> 6) & 63)); o[2] = (char)(0x80 | (c & 63)); return 3; }
  o[0] = (char)(0xf0 | c >> 18); o[1] = (char)(0x80 | ((c >> 12) & 63));
  o[2] = (char)(0x80 | ((c >> 6) & 63)); o[3] = (char)(0x80 | (c & 63));
  return 4;
}
int vt_row_text(const vt *t, int y, char *buf, int cap) {
  int w, n = 0, end = 0;
  const vt_cell *r = vt_view_row(t, y, &w);
  for (int x = 0; r && x < w && n + 5 < cap; x++) {
    n += vt_utf8_encode(r[x].ch, buf + n);
    if (r[x].ch != ' ') end = n;
  }
  buf[end] = 0;
  return end;
}
void vt_take_reply(vt *t, char *out, int *len) {
  memcpy(out, t->reply, (size_t)t->reply_len);
  *len = t->reply_len;
  t->reply_len = 0;
}
