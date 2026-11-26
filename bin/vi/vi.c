// SPDX-License-Identifier: GPL-2.0-or-later
/* vi: a small modal editor in the spirit of vi.
 * Modes: normal, insert, visual-line and command-line. Motions hjkl w b e W B E 0 ^ $ gg G
 * f t F T ; , % H M L, Ctrl-F/B/D/U; operators d c y > < with counts; x X D C s S J r ~ p P
 * u Ctrl-R and . (repeat); / ? n N * #; :w :q :q! :wq :x :e :N :set nu/nonu :s and :%s.
 * Undo keeps whole-buffer snapshots that share unchanged lines (copy on write), so every
 * change costs one pointer per line. Not on a terminal, vi reads ex commands from stdin
 * (one per line, ":normal keys" runs normal-mode keys), which makes it scriptable. */
#include "term.h"
#include "vi.h"
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>

#define BIG 0x3fffffff
#define UNDO_MAX 1000

/* ---------- lines: refcounted so undo snapshots can share them ---------- */
typedef struct { int refs, len, cap; char *s; } Line;
static Line **L;
static int nl, lcap;
static int buf_empty = 1; /* "--No lines in buffer--": written as 0 bytes */

static Line *lnew(const char *s, int n) {
  Line *l = malloc(sizeof *l);
  l->refs = 1; l->len = n; l->cap = n + 16;
  l->s = malloc(l->cap);
  if (n) memcpy(l->s, s, n);
  return l;
}
static void lunref(Line *l) { if (--l->refs == 0) { free(l->s); free(l); } }
static void lreserve(int n) { if (n > lcap) { lcap = n * 2 + 16; L = realloc(L, lcap * sizeof *L); } }
static int LEN(int y) { return L[y]->len; }
static char *S(int y) { return L[y]->s; }

/* ---------- undo: snapshots of the line array ---------- */
typedef struct { Line **v; int n, cy, cx, empty; } Snap;
static Snap und[UNDO_MAX], red[UNDO_MAX];
static int nund, nred;
static int chg_open, chg_dirty, modified;
static int cy, cx, want, top, leftcol; /* cursor (byte column), wanted display column, scroll */
static int cmd_cy, cmd_cx;             /* cursor when the current command started */

static Snap snap_take(int y, int x) {
  Snap s = {malloc((nl ? nl : 1) * sizeof(Line *)), nl, y, x, buf_empty};
  for (int i = 0; i < nl; i++) { s.v[i] = L[i]; L[i]->refs++; }
  return s;
}
static void snap_free(Snap *s) { for (int i = 0; i < s->n; i++) lunref(s->v[i]); free(s->v); }
static void snap_restore(Snap *s) { /* the snapshot's references move into the buffer */
  for (int i = 0; i < nl; i++) lunref(L[i]);
  free(L);
  L = s->v; nl = lcap = s->n; buf_empty = s->empty;
  cy = s->cy; cx = s->cx;
}
static void begin_change(void) {
  if (chg_open) return;
  while (nred) snap_free(&red[--nred]);
  if (nund == UNDO_MAX) { snap_free(&und[0]); memmove(und, und + 1, (UNDO_MAX - 1) * sizeof *und); nund--; }
  und[nund++] = snap_take(cmd_cy, cmd_cx);
  chg_open = 1; chg_dirty = 0;
}
static void end_change(void) {
  if (chg_open && !chg_dirty) snap_free(&und[--nund]); /* nothing changed after all */
  chg_open = 0;
}
static void mark_dirty(void) { begin_change(); chg_dirty = 1; modified = 1; }

/* ---------- buffer edits (all go through here) ---------- */
static Line *lmut(int y) {
  mark_dirty();
  Line *l = L[y];
  if (l->refs > 1) { Line *c = lnew(l->s, l->len); l->refs--; L[y] = c; l = c; }
  return l;
}
static void ins_text(int y, int x, const char *s, int n) {
  Line *l = lmut(y);
  if (l->len + n + 1 > l->cap) { l->cap = (l->len + n) * 2 + 16; l->s = realloc(l->s, l->cap); }
  memmove(l->s + x + n, l->s + x, l->len - x);
  memcpy(l->s + x, s, n);
  l->len += n;
  buf_empty = 0;
}
static void del_text(int y, int x, int n) {
  Line *l = lmut(y);
  if (x + n > l->len) n = l->len - x;
  if (n <= 0) return;
  memmove(l->s + x, l->s + x + n, l->len - x - n);
  l->len -= n;
}
static void set_line(int y, const char *s, int n) { Line *l = lmut(y); l->len = 0; ins_text(y, 0, s, n); (void)l; }
static void ins_line(int at, const char *s, int n) {
  mark_dirty();
  lreserve(nl + 1);
  memmove(L + at + 1, L + at, (nl - at) * sizeof *L);
  L[at] = lnew(s, n);
  nl++;
  buf_empty = 0;
}
static void del_lines(int y, int n) {
  mark_dirty();
  if (y + n > nl) n = nl - y;
  for (int i = 0; i < n; i++) lunref(L[y + i]);
  memmove(L + y, L + y + n, (nl - y - n) * sizeof *L);
  nl -= n;
  if (nl == 0) { lreserve(1); L[0] = lnew("", 0); nl = 1; buf_empty = 1; }
}
static void split_line(int y, int x) {
  Line *l = L[y];
  ins_line(y + 1, l->s + x, l->len - x); /* l stays valid: only the array moved */
  lmut(y)->len = x;
}
/* insert text that may contain newlines at (y, x); the end position goes to (*ey, *ex) */
static void put_text(int y, int x, const char *s, int n, int *ey, int *ex) {
  const char *nlp = memchr(s, '\n', n);
  if (!nlp) { ins_text(y, x, s, n); *ey = y; *ex = x + n; return; }
  split_line(y, x);
  ins_text(y, x, s, (int)(nlp - s));
  const char *p = nlp + 1, *end = s + n;
  int yy = y + 1;
  for (const char *q; (q = memchr(p, '\n', end - p)); p = q + 1) ins_line(yy++, p, (int)(q - p));
  ins_text(yy, 0, p, (int)(end - p));
  *ey = yy; *ex = (int)(end - p);
}

/* ---------- character helpers ---------- */
static int nextc(int y, int x) {
  int n = LEN(y);
  if (x >= n) return n;
  x += u8_len((unsigned char)S(y)[x]);
  return x > n ? n : x;
}
static int prevc(int y, int x) {
  if (x <= 0) return 0;
  x--;
  while (x > 0 && u8_cont((unsigned char)S(y)[x])) x--;
  return x;
}
static int lastc(int y) { return LEN(y) ? prevc(y, LEN(y)) : 0; }
static int is_blank(int c) { return c == ' ' || c == '\t'; }
static int first_nb(int y) {
  int x = 0;
  while (x < LEN(y) && is_blank(S(y)[x])) x++;
  return x < LEN(y) ? x : lastc(y);
}
static int char_w(const char *s, int x, int col) { /* display width of the char at s[x] when at column col */
  unsigned char c = s[x];
  if (c == '\t') return 8 - col % 8;
  if (c < 32 || c == 127) return 2;
  return 1;
}
static int dcol(int y, int x) {
  int col = 0;
  for (int i = 0; i < x && i < LEN(y); i = nextc(y, i)) col += char_w(S(y), i, col);
  return col;
}
static int x_at_dcol(int y, int w, int past_end) {
  int col = 0, i = 0;
  while (i < LEN(y)) {
    int cw = char_w(S(y), i, col);
    if (col + cw > w) return i;
    col += cw;
    int n = nextc(y, i);
    if (n >= LEN(y) && !past_end) return i;
    i = n;
  }
  return past_end ? LEN(y) : lastc(y);
}

/* ---------- editor state ---------- */
enum { M_NORMAL, M_INSERT, M_VISUAL, M_CMD };
static int mode, number, quit_flag;
static int R = 24, C = 80;
static char filename[512];
static char msg[512];
static int msg_err;
static char cmdline[512];
static int cmd_len, cmd_active;
static char last_search[512];
static int last_dir = 1;
static int vis_y; /* visual anchor line */
static int mark_lt = -1, mark_gt = -1;
static char *reg;
static int reg_len, reg_line;
static int interactive, no_term; /* no_term: :normal is running, never read the terminal */

static void set_msg(int err, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  msg_err = err;
  if (!interactive && msg[0]) fprintf(err ? stderr : stdout, "%s\n", msg);
}
static void clamp(void) {
  if (cy >= nl) cy = nl - 1;
  if (cy < 0) cy = 0;
  int maxx = mode == M_INSERT ? LEN(cy) : lastc(cy);
  if (cx > maxx) cx = maxx;
  if (cx < 0) cx = 0;
  while (cx > 0 && cx < LEN(cy) && u8_cont((unsigned char)S(cy)[cx])) cx--;
}
static void set_want(void) { want = dcol(cy, cx); }
static int text_rows(void) { return R - 2 > 1 ? R - 2 : 1; }

/* ---------- files ---------- */
static void free_buffer(void) {
  for (int i = 0; i < nl; i++) lunref(L[i]);
  nl = 0;
  while (nund) snap_free(&und[--nund]);
  while (nred) snap_free(&red[--nred]);
}
static int load_file(const char *path) { /* 0 loaded, 1 new file, -1 error */
  free_buffer();
  cy = cx = want = top = leftcol = 0;
  modified = 0;
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    lreserve(1); L[0] = lnew("", 0); nl = 1; buf_empty = 1;
    return errno == ENOENT ? 1 : -1;
  }
  size_t cap = 65536, len = 0;
  char *buf = malloc(cap);
  for (;;) {
    if (len == cap) buf = realloc(buf, cap *= 2);
    ssize_t r = read(fd, buf + len, cap - len);
    if (r <= 0) break;
    len += r;
  }
  close(fd);
  size_t start = 0;
  for (size_t i = 0; i < len; i++)
    if (buf[i] == '\n') { lreserve(nl + 1); L[nl++] = lnew(buf + start, (int)(i - start)); start = i + 1; }
  if (start < len) { lreserve(nl + 1); L[nl++] = lnew(buf + start, (int)(len - start)); }
  free(buf);
  buf_empty = nl == 0;
  if (!nl) { lreserve(1); L[0] = lnew("", 0); nl = 1; }
  return 0;
}
static long buffer_bytes(void) {
  if (buf_empty && nl == 1 && !LEN(0)) return 0;
  long b = 0;
  for (int i = 0; i < nl; i++) b += LEN(i) + 1;
  return b;
}
static int write_file(const char *path) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) { set_msg(1, "E212: Can't open file for writing: %s", path); return -1; }
  long bytes = buffer_bytes();
  int ok = 1;
  if (bytes) {
    char *out = malloc(bytes), *p = out;
    for (int i = 0; i < nl; i++) { memcpy(p, S(i), LEN(i)); p += LEN(i); *p++ = '\n'; }
    ok = write(fd, out, bytes) == bytes;
    free(out);
  }
  close(fd);
  if (!ok) { set_msg(1, "E514: Write error: %s", path); return -1; }
  set_msg(0, "\"%s\" %dL, %ldB written", path, bytes ? nl : 0, bytes);
  if (!strcmp(path, filename)) modified = 0;
  return 0;
}
static void open_file(const char *path) {
  snprintf(filename, sizeof filename, "%s", path);
  int r = load_file(path);
  if (r == 1) set_msg(0, "\"%s\" [New]", path);
  else if (r < 0) set_msg(1, "\"%s\" cannot be read", path);
  else set_msg(0, "\"%s\" %dL, %ldB", path, buf_empty ? 0 : nl, buffer_bytes());
}

/* ---------- syntax colouring ---------- */
enum { H_TEXT, H_KEYWORD, H_TYPE, H_STRING, H_COMMENT, H_NUMBER, H_PREPROC, H_HEADING, H_CODE, H_LIST, H_CTRL };
enum { FT_NONE, FT_C, FT_MD };
static int filetype(void) {
  const char *dot = strrchr(filename, '.');
  if (!dot) return FT_NONE;
  if (!strcmp(dot, ".c") || !strcmp(dot, ".h")) return FT_C;
  if (!strcmp(dot, ".md") || !strcmp(dot, ".markdown")) return FT_MD;
  return FT_NONE;
}
static const char *c_kw[] = {"if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue", "return",
                             "goto", "sizeof", "typedef", "struct", "union", "enum", "static", "extern", "const", "volatile",
                             "inline", "register", "restrict", 0};
static const char *c_ty[] = {"int", "char", "void", "long", "short", "unsigned", "signed", "float", "double", "bool",
                             "size_t", "ssize_t", "pid_t", "off_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t",
                             "int16_t", "int32_t", "int64_t", "u8", "u16", "u32", "u64", "FILE", "NULL", "true", "false", 0};
static int in_list(const char **list, const char *s, int n) {
  for (int i = 0; list[i]; i++) if ((int)strlen(list[i]) == n && !memcmp(list[i], s, n)) return 1;
  return 0;
}
static int is_word(int c) { return isalnum(c) || c == '_' || c >= 0x80; }
/* colour each byte of line y into hl; *incomment carries a C block comment across lines */
static void highlight(int y, unsigned char *hl, int ft, int *incomment) {
  const char *s = S(y);
  int n = LEN(y);
  memset(hl, H_TEXT, n);
  if (ft == FT_MD) {
    int i = 0;
    while (i < n && s[i] == ' ') i++;
    if (i < n && s[i] == '#') { memset(hl, H_HEADING, n); return; }
    if (i + 1 < n && (s[i] == '-' || s[i] == '*' || s[i] == '+') && s[i + 1] == ' ') hl[i] = H_LIST;
    for (int k = 0; k < n; k++)
      if (s[k] == '`') {
        int e = k + 1;
        while (e < n && s[e] != '`') e++;
        if (e < n) { memset(hl + k, H_CODE, e - k + 1); k = e; }
      }
    return;
  }
  if (ft != FT_C) return;
  int i = 0;
  int fnb = 0;
  while (fnb < n && is_blank(s[fnb])) fnb++;
  if (!*incomment && fnb < n && s[fnb] == '#') { /* preprocessor line, but still colour comments */
    for (i = fnb; i < n; i++) {
      if (i + 1 < n && s[i] == '/' && (s[i + 1] == '/' || s[i + 1] == '*')) break;
      hl[i] = H_PREPROC;
    }
  }
  while (i < n) {
    if (*incomment) {
      int st = i;
      while (i < n && !(s[i] == '*' && i + 1 < n && s[i + 1] == '/')) i++;
      if (i < n) { i += 2; *incomment = 0; }
      memset(hl + st, H_COMMENT, i - st);
      continue;
    }
    char c = s[i];
    if (c == '/' && i + 1 < n && s[i + 1] == '/') { memset(hl + i, H_COMMENT, n - i); return; }
    if (c == '/' && i + 1 < n && s[i + 1] == '*') { *incomment = 1; hl[i] = hl[i + 1] = H_COMMENT; i += 2; continue; }
    if (c == '"' || c == '\'') {
      int st = i++;
      while (i < n && s[i] != c) i += s[i] == '\\' ? 2 : 1;
      if (i < n) i++;
      if (i > n) i = n;
      memset(hl + st, H_STRING, i - st);
      continue;
    }
    if (isdigit((unsigned char)c) && (i == 0 || !is_word((unsigned char)s[i - 1]))) {
      int st = i;
      while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '.')) i++;
      memset(hl + st, H_NUMBER, i - st);
      continue;
    }
    if (is_word((unsigned char)c)) {
      int st = i;
      while (i < n && is_word((unsigned char)s[i])) i++;
      if (hl[st] != H_PREPROC) {
        if (in_list(c_kw, s + st, i - st)) memset(hl + st, H_KEYWORD, i - st);
        else if (in_list(c_ty, s + st, i - st)) memset(hl + st, H_TYPE, i - st);
      }
      continue;
    }
    i++;
  }
}
static unsigned hl_color(int h) {
  switch (h) {
  case H_KEYWORD: return th->red;
  case H_TYPE: return th->yellow;
  case H_STRING: return th->green;
  case H_COMMENT: return th->grey;
  case H_NUMBER: return th->purple;
  case H_PREPROC: return th->aqua;
  case H_HEADING: return th->orange;
  case H_CODE: return th->aqua;
  case H_LIST: return th->yellow;
  case H_CTRL: return th->blue;
  }
  return th->fg;
}

/* ---------- screen: each row is rebuilt, but only rows that changed are written ---------- */
static char **prev_row;
static int prev_R, prev_C;
static size_t row_start;
static int gutter(void) {
  if (!number) return 0;
  int w = 1;
  for (int n = nl; n >= 10; n /= 10) w++;
  return (w < 3 ? 3 : w) + 1;
}
static void row_begin(int r) { row_start = ob_len; ob_goto(r, 0); }
static void row_end(int r) {
  ob_puts("\033[0m");
  size_t n = ob_len - row_start;
  if (prev_row[r] && strlen(prev_row[r]) == n && !memcmp(prev_row[r], ob + row_start, n)) { ob_len = row_start; return; }
  free(prev_row[r]);
  prev_row[r] = malloc(n + 1);
  memcpy(prev_row[r], ob + row_start, n);
  prev_row[r][n] = 0;
}
/* Soft wrap (on by default, :set nowrap turns it off): a line is split into screen segments,
 * breaking after a blank when the word would not fit, so words are not cut in half.
 * seg_x[k] is the first byte of segment k and seg_c[k] its display column. */
static int wrap_on = 1;
static int segments(int y, int tw, int *seg_x, int *seg_c, int max) {
  int n = 1, col = 0, brk_x = -1, brk_c = 0;
  seg_x[0] = 0; seg_c[0] = 0;
  if (!wrap_on || tw < 1) return 1;
  const char *s = S(y);
  for (int i = 0; i < LEN(y); i = nextc(y, i)) {
    int w = char_w(s, i, col);
    if (col + w - seg_c[n - 1] > tw && n < max) {
      if (brk_x > seg_x[n - 1]) { seg_x[n] = brk_x; seg_c[n] = brk_c; }
      else { seg_x[n] = i; seg_c[n] = col; }
      n++;
      brk_x = -1;
    }
    col += w;
    if (is_blank(s[i])) { brk_x = nextc(y, i); brk_c = col; }
  }
  return n;
}
#define MAXSEG 256
static int seg_of(int y, int x, int tw, int *col_in_seg) { /* segment holding byte x, and the column in it */
  int sx[MAXSEG], sc[MAXSEG], n = segments(y, tw, sx, sc, MAXSEG), k = n - 1;
  while (k > 0 && sx[k] > x) k--;
  if (col_in_seg) *col_in_seg = dcol(y, x) - sc[k];
  return k;
}
static int line_rows(int y, int tw) { int sx[MAXSEG], sc[MAXSEG]; return segments(y, tw, sx, sc, MAXSEG); }
static void scroll_to_cursor(void) {
  int th_ = text_rows();
  if (cy < top) top = cy - top < -th_ / 2 ? cy - th_ / 2 : cy;
  else if (cy >= top + th_) {
    top = cy - (top + th_) >= th_ / 2 ? cy - th_ / 2 : cy - th_ + 1;
    if (top > nl - th_) top = nl - th_ > 0 ? nl - th_ : 0; /* a far jump down shows the end at the bottom */
  }
  if (top < 0) top = 0;
  if (top > cy) top = cy;
  int tw = C - gutter();
  if (tw < 1) tw = 1;
  if (wrap_on) { /* wrapped lines take several rows: scroll until the cursor's row is on screen */
    leftcol = 0;
    for (;;) {
      int rows = seg_of(cy, cx, tw, 0) + 1;
      for (int y = top; y < cy; y++) rows += line_rows(y, tw);
      if (rows <= th_ || top >= cy) break;
      top++;
    }
    return;
  }
  int col = dcol(cy, cx);
  if (col < leftcol) leftcol = col;
  if (col >= leftcol + tw) leftcol = col - tw + 1;
}
/* screen row r shows line y from byte x0 to x1, display column c0 at the left edge */
static void draw_text_row(int r, int y, int x0, int x1, int c0, int first, unsigned char *hl, int vis1, int vis2) {
  int gw = gutter(), tw = C - gw;
  unsigned bgc = (mode == M_VISUAL && y >= vis1 && y <= vis2) ? th->bg2 : th->bg;
  row_begin(r);
  if (y >= nl) {
    ob_bg(th->bg); ob_fg(th->bg2); ob_puts("~");
    for (int i = 1; i < C; i++) ob_put(" ", 1);
    row_end(r);
    return;
  }
  if (gw) {
    ob_bg(th->bg);
    if (y == cy) { ob_puts("\033[1m"); ob_fg(th->yellow); } else ob_fg(th->grey);
    if (first) ob_printf("%*d ", gw - 1, y + 1); else ob_printf("%*s ", gw - 1, "");
    ob_puts("\033[22m");
  }
  ob_bg(bgc);
  const char *s = S(y);
  int col = dcol(y, x0), i = x0, out = 0, cur = -1;
  while (i < x1 && out < tw) {
    int w = char_w(s, i, col), next = nextc(y, i);
    int h = hl[i];
    unsigned char c = s[i];
    if (c < 32 || c == 127) h = H_CTRL;
    for (int k = 0; k < w; k++) {
      int vc = col + k;
      if (vc < c0 || out >= tw) continue;
      if (h != cur) {
        ob_fg(hl_color(h));
        if (h == H_HEADING) ob_puts("\033[1m"); else if (cur == H_HEADING) ob_puts("\033[22m");
        cur = h;
      }
      if (c == '\t') ob_put(" ", 1);
      else if (c < 32 || c == 127) ob_put(k == 0 ? "^" : (c == 127 ? "?" : (char[]){(char)(c + 64), 0}), 1);
      else if (vc == col) ob_put(s + i, next - i);
      else ob_put(" ", 1);
      out++;
    }
    col += w;
    i = next;
  }
  ob_puts("\033[22m");
  for (; out < tw; out++) ob_put(" ", 1);
  row_end(r);
}
static void draw_status(int r) {
  static const char *names[] = {"NORMAL", "INSERT", "V-LINE", "COMMAND"};
  unsigned mbg[] = {th->bg2, th->orange, th->purple, th->aqua};
  unsigned mfg[] = {th->fg, th->bg, th->bg, th->bg};
  char mode_s[32], pos[48], path[600];
  snprintf(mode_s, sizeof mode_s, " -- %s -- ", names[mode]);
  snprintf(pos, sizeof pos, " %d:%d ", cy + 1, dcol(cy, cx) + 1);
  snprintf(path, sizeof path, "%s%s", filename[0] ? filename : "[No Name]", modified ? " [+]" : "");
  int lw = 6 /* the club mark: chevron, 42, cursor bar */, mw = u8_width(mode_s), pw = u8_width(pos);
  int mid = C - mw - pw - lw;
  if (mid < 0) { lw = 0; mid = C - mw - pw; }
  row_begin(r);
  ob_bg(mbg[mode]); ob_fg(mfg[mode]); ob_puts("\033[1m");
  if (mid < 0) { ob_padded(mode_s, C, 0); row_end(r); return; }
  ob_puts(mode_s);
  ob_puts("\033[22m");
  ob_bg(th->bg1); ob_fg(th->grey);
  int plen = u8_width(path);
  if (plen > mid) plen = mid;
  int left = (mid - plen) / 2;
  for (int i = 0; i < left; i++) ob_put(" ", 1);
  ob_padded(path, plen, 0);
  for (int i = left + plen; i < mid; i++) ob_put(" ", 1);
  ob_bg(th->yellow); ob_fg(th->bg); ob_puts("\033[1m"); ob_puts(pos);
  if (lw) {
    ob_bg(th->bg); ob_puts(" "); ob_fg(th->aqua); ob_puts("\xe2\x9d\xaf"); ob_fg(th->fg); ob_puts("42");
    ob_fg(th->orange); ob_puts("\xe2\x96\x8c ");
  }
  ob_puts("\033[22m");
  row_end(r);
}
static void render(void) {
  int r, c;
  if (tty_size(1, &r, &c) == 0) { R = r < 3 ? 3 : r; C = c < 20 ? 20 : c; }
  static double theme_checked;
  int theme_changed = 0;
  if (now_ms() - theme_checked > 1000) { theme_checked = now_ms(); theme_changed = theme_reload(); } /* Alt+t */
  if (R != prev_R || C != prev_C || theme_changed) {
    for (int i = 0; i < prev_R; i++) free(prev_row[i]);
    free(prev_row);
    prev_row = calloc(R, sizeof *prev_row);
    prev_R = R; prev_C = C;
    ob_puts("\033[0m\033[2J");
  }
  scroll_to_cursor();
  ob_puts("\033[?25l");
  int ft = filetype(), incomment = 0, th_ = text_rows();
  int maxlen = 1;
  for (int y = top; y < top + th_ && y < nl; y++) if (LEN(y) > maxlen) maxlen = LEN(y);
  unsigned char *hl = malloc(maxlen + 1);
  if (ft == FT_C) { /* block comment state at the top of the screen */
    unsigned char *tmp = 0;
    int cap = 0;
    for (int y = 0; y < top && y < nl; y++) {
      if (LEN(y) + 1 > cap) { cap = LEN(y) + 1; tmp = realloc(tmp, cap); }
      highlight(y, tmp, ft, &incomment);
    }
    free(tmp);
  }
  int v1 = vis_y < cy ? vis_y : cy, v2 = vis_y < cy ? cy : vis_y;
  int tw = C - gutter(), sr = 0, cur_row = 0, cur_col = 0;
  for (int y = top; sr < th_; y++) {
    if (y >= nl) { draw_text_row(sr++, y, 0, 0, 0, 1, hl, v1, v2); continue; }
    if (LEN(y) + 1 > maxlen) { maxlen = LEN(y) + 1; hl = realloc(hl, maxlen); }
    highlight(y, hl, ft, &incomment);
    int sx[MAXSEG], sc[MAXSEG], n = segments(y, tw, sx, sc, MAXSEG);
    for (int k = 0; k < n && sr < th_; k++, sr++) {
      int x1 = k + 1 < n ? sx[k + 1] : LEN(y);
      if (wrap_on) draw_text_row(sr, y, sx[k], x1, sc[k], k == 0, hl, v1, v2);
      else draw_text_row(sr, y, 0, LEN(y), leftcol, 1, hl, v1, v2);
    }
    if (y == cy) { int c; int k = seg_of(cy, cx, tw, &c); cur_row = sr - n + k; cur_col = wrap_on ? c : dcol(cy, cx) - leftcol; }
  }
  free(hl);
  draw_status(R - 2);
  row_begin(R - 1);
  ob_bg(th->bg);
  if (cmd_active) { ob_fg(th->fg); cmdline[cmd_len] = 0; ob_padded(cmdline, C, 0); }
  else if (msg[0]) { ob_fg(msg_err ? th->red : th->fg); if (msg_err) ob_puts("\033[1m"); ob_padded(msg, C, 0); }
  else ob_padded("", C, 0);
  row_end(R - 1);
  /* cursor */
  if (cmd_active) {
    cmdline[cmd_len] = 0;
    int w = u8_width(cmdline);
    ob_goto(R - 1, w < C ? w : C - 1);
  } else {
    int col = cur_col;
    if (mode != M_INSERT && cx < LEN(cy) && S(cy)[cx] == '\t') col += char_w(S(cy), cx, dcol(cy, cx)) - 1;
    int sc = gutter() + col;
    ob_goto(cur_row, sc < C ? sc : C - 1);
  }
  ob_puts("\033[?25h");
  ob_flush();
}

/* ---------- keys, recording for "." ---------- */
static int Q[16384], qn;
static int rec[8192], rec_n, recording;
static int dot[8192], dot_n, dot_count, forced_count;
static void q_push_front(const int *k, int n) {
  if (qn + n > (int)(sizeof Q / sizeof Q[0])) return;
  memmove(Q + n, Q, qn * sizeof *Q);
  memcpy(Q, k, n * sizeof *Q);
  qn += n;
}
static void quit_now(int code);
static int getkey(void) {
  int k;
  if (qn) { k = Q[0]; memmove(Q, Q + 1, --qn * sizeof *Q); }
  else if (!interactive || no_term) k = K_ESC;
  else {
    if (!key_wait(0)) render();
    k = key_read(-1);
    if (k == -2) quit_now(1);
    if (k == K_UNKNOWN) k = 0;
    msg[0] = 0;
  }
  if (recording && rec_n < (int)(sizeof rec / sizeof rec[0])) rec[rec_n++] = k;
  return k;
}

/* ---------- motions ---------- */
enum { EXCL, INCL, LINEW };
static int my, mx; /* motion cursor */
static int cls(int big) {
  if (mx >= LEN(my)) return 0;
  unsigned char c = S(my)[mx];
  if (is_blank(c)) return 0;
  if (big) return 1;
  return is_word(c) ? 2 : 1;
}
static int inc(void) { /* vim's inc(): 0 moved, 2 moved onto the end of line, 1 next line, -1 at end */
  if (mx < LEN(my)) { mx = nextc(my, mx); return mx < LEN(my) ? 0 : 2; }
  if (my < nl - 1) { my++; mx = 0; return 1; }
  return -1;
}
static int dec(void) {
  if (mx > 0) { mx = prevc(my, mx); return 0; }
  if (my > 0) { my--; mx = LEN(my); return 1; }
  return -1;
}
static int fwd_word(int count, int big, int eol) {
  while (--count >= 0) {
    int last_line = my == nl - 1, sc = cls(big), i = inc();
    if (i == -1 || (i >= 1 && last_line)) return 0;
    if (i >= 1 && eol && count == 0) return 1;
    if (sc) while (cls(big) == sc) { i = inc(); if (i == -1 || (i >= 1 && eol && count == 0)) return 1; }
    while (cls(big) == 0) {
      if (mx == 0 && LEN(my) == 0) break;
      i = inc();
      if (i == -1 || (i >= 1 && eol && count == 0)) return 1;
    }
  }
  return 1;
}
static int skip_fwd(int c, int big) { while (cls(big) == c) if (inc() == -1) return 1; return 0; }
static int skip_back(int c, int big) { while (cls(big) == c) if (dec() == -1) return 1; return 0; }
static int end_word(int count, int big, int stop) {
  while (--count >= 0) {
    int sc = cls(big);
    if (inc() == -1) return 0;
    if (cls(big) == sc && sc) { if (skip_fwd(sc, big)) return 0; }
    else if (!stop || sc == 0) {
      while (cls(big) == 0) if (inc() == -1) return 0;
      if (skip_fwd(cls(big), big)) return 0;
    }
    dec();
    stop = 0;
  }
  return 1;
}
static int bck_word(int count, int big) {
  while (--count >= 0) {
    if (dec() == -1) return 0;
    int empty_line = 0;
    while (cls(big) == 0) { /* skip white space before the word; an empty line is a word */
      if (mx == 0 && LEN(my) == 0) { empty_line = 1; break; }
      if (dec() == -1) return 1;
    }
    if (empty_line) continue;
    if (skip_back(cls(big), big)) return 1;
    inc();
  }
  return 1;
}
static int find_c, find_kind; /* last f/t/F/T */
static int find_char(int kind, int c, int count) {
  int x = mx;
  for (int n = 0; n < count; n++) {
    if (kind == 'f' || kind == 't') {
      int i = nextc(my, x);
      while (i < LEN(my) && (unsigned char)S(my)[i] != c) i = nextc(my, i);
      if (i >= LEN(my)) return 0;
      x = i;
    } else {
      int i = x;
      if (i == 0) return 0;
      i = prevc(my, i);
      while (i > 0 && (unsigned char)S(my)[i] != c) i = prevc(my, i);
      if ((unsigned char)S(my)[i] != c) return 0;
      x = i;
    }
  }
  if (kind == 't') x = prevc(my, x);
  if (kind == 'T') x = nextc(my, x);
  mx = x;
  return 1;
}
/* search for last_search from (my, mx) in direction dir; moves (my, mx) to the match */
static int search(int dir, int quiet) {
  const char *pat = last_search;
  if (!pat[0]) { if (!quiet) set_msg(1, "E35: No previous regular expression"); return 0; }
  int ms, me, wrapped = 0;
  if (dir > 0) {
    int from = mx < LEN(my) ? nextc(my, mx) : LEN(my) + 1;
    for (int k = 0; k <= nl; k++) {
      int y = (my + k) % nl;
      if (k > 0 && y == 0) wrapped = 1;
      int f = k == 0 ? from : 0;
      if (k == nl) f = 0;
      if (f <= LEN(y) && re_search(pat, S(y), LEN(y), f, &ms, &me)) {
        if (k == nl && ms >= mx) break;
        if (ms == LEN(y) && LEN(y) > 0 && me == ms && pat[0] != '$') { /* empty match at end: skip */ }
        my = y; mx = ms;
        if (wrapped && !quiet) set_msg(1, "search hit BOTTOM, continuing at TOP");
        return 1;
      }
    }
  } else {
    for (int k = 0; k <= nl; k++) {
      int y = ((my - k) % nl + nl) % nl;
      if (k > 0 && y == nl - 1) wrapped = 1;
      int best = -1, pos = 0;
      while (pos <= LEN(y) && re_search(pat, S(y), LEN(y), pos, &ms, &me)) {
        if (k == 0 && ms >= mx) break;
        if (k == nl && ms <= mx) { pos = ms + 1; continue; }
        best = ms;
        pos = ms + 1;
      }
      if (best >= 0) {
        my = y; mx = best;
        if (wrapped && !quiet) set_msg(1, "search hit TOP, continuing at BOTTOM");
        return 1;
      }
    }
  }
  if (!quiet) set_msg(1, "E486: Pattern not found: %s", pat);
  return 0;
}
static int read_cmdline(int prompt);
static int match_pair(void) {
  static const char *pairs = "()[]{}";
  int x = mx;
  while (x < LEN(my) && !strchr(pairs, S(my)[x])) x++;
  if (x >= LEN(my)) return 0;
  char c = S(my)[x];
  int idx = (int)(strchr(pairs, c) - pairs), dir = idx % 2 == 0 ? 1 : -1;
  char open = pairs[idx], close = pairs[idx + dir];
  int depth = 0, y = my;
  for (;;) {
    char ch = S(y)[x];
    if (ch == open) depth++;
    else if (ch == close && --depth == 0) { my = y; mx = x; return 1; }
    if (dir > 0) {
      x++;
      while (x >= LEN(y)) { if (++y >= nl) return 0; x = 0; if (LEN(y)) break; }
    } else {
      x--;
      while (x < 0) { if (--y < 0) return 0; x = LEN(y) - 1; }
    }
  }
}
static void word_under_cursor(char *out, int cap) {
  int x = cx;
  out[0] = 0;
  while (x < LEN(cy) && !is_word((unsigned char)S(cy)[x])) x++;
  if (x >= LEN(cy)) return;
  int s = x;
  while (s > 0 && is_word((unsigned char)S(cy)[s - 1])) s--;
  int e = x;
  while (e < LEN(cy) && is_word((unsigned char)S(cy)[e])) e++;
  int n = 0;
  for (int i = s; i < e && n < cap - 2; i++) {
    char ch = S(cy)[i];
    if (strchr(".*[]^$\\/", ch)) out[n++] = '\\';
    out[n++] = ch;
  }
  out[n] = 0;
  mx = s; /* * and # search from the start of the word */
}

/* parse a motion key (count already known); sets my, mx, *type. op: the pending operator or 0 */
static int motion(int k, int count, int has_count, int op, int *type) {
  int n = count < 1 ? 1 : count, th_ = text_rows();
  my = cy; mx = cx;
  *type = EXCL;
  switch (k) {
  case 'h': case K_LEFT: case K_BS: case 8:
    if (mx == 0) return 0;
    for (int i = 0; i < n && mx > 0; i++) mx = prevc(my, mx);
    return 1;
  case 'l': case K_RIGHT: case ' ':
    if (op) { for (int i = 0; i < n && mx < LEN(my); i++) mx = nextc(my, mx); return LEN(my) > 0; }
    if (nextc(my, mx) >= LEN(my)) return 0;
    for (int i = 0; i < n && nextc(my, mx) < LEN(my); i++) mx = nextc(my, mx);
    return 1;
  case '0': case K_HOME: mx = 0; return 1;
  case '^': mx = first_nb(my); return 1;
  case '$': case K_END:
    *type = INCL;
    my += n - 1;
    if (my >= nl) return 0;
    mx = op ? (LEN(my) ? lastc(my) : 0) : lastc(my);
    want = BIG;
    if (op && !LEN(my)) { *type = EXCL; mx = 0; }
    return 1;
  case '|': mx = x_at_dcol(my, n - 1, 0); return 1;
  case 'j': case K_DOWN: case 14: case 10: /* Ctrl-N, Ctrl-J */
    *type = LINEW;
    if (my + n >= nl) { if (my == nl - 1) return 0; n = nl - 1 - my; }
    my += n;
    return 1;
  case 'k': case K_UP: case 16:
    *type = LINEW;
    if (my == 0) return 0;
    my -= n < my ? n : my;
    return 1;
  case '+': case '\r': case '-':
    *type = LINEW;
    if (k == '-') { if (my - n < 0) return 0; my -= n; }
    else { if (my + n >= nl) return 0; my += n; }
    mx = first_nb(my);
    return 1;
  case '_': *type = LINEW; my += n - 1; if (my >= nl) my = nl - 1; mx = first_nb(my); return 1;
  case 'G': *type = LINEW; my = has_count ? (count > nl ? nl : count) - 1 : nl - 1; mx = first_nb(my); return 1;
  case 'g': {
    int k2 = getkey();
    if (k2 == 'g') { *type = LINEW; my = has_count ? (count > nl ? nl : count) - 1 : 0; mx = first_nb(my); return 1; }
    if (k2 == 'e' || k2 == 'E') { /* back to the end of the previous word */
      *type = INCL;
      for (int i = 0; i < n; i++) {
        int sc = cls(k2 == 'E');
        while (cls(k2 == 'E') == sc && sc) if (dec() == -1) return 1;
        while (cls(k2 == 'E') == 0) { if (mx == 0 && LEN(my) == 0) break; if (dec() == -1) return 1; }
      }
      return 1;
    }
    if (k2 == '_') { *type = INCL; my += n - 1; if (my >= nl) my = nl - 1; mx = LEN(my); while (mx > 0 && is_blank(S(my)[mx - 1])) mx--; mx = prevc(my, mx); return 1; }
    return 0;
  }
  case 'w': case 'W': {
    int big = k == 'W';
    if (op == 'c' && cls(big) != 0) { /* cw changes to the end of the word, like ce */
      *type = INCL;
      end_word(n, big, 1);
      if (mx >= LEN(my) && LEN(my)) mx = lastc(my);
      return 1;
    }
    int ok = fwd_word(n, big, op != 0);
    if (!op) { if (mx >= LEN(my)) mx = lastc(my); return ok || my != cy || mx != cx; }
    if (mx >= LEN(my) && mx > 0 && !(my > cy && mx == 0)) { *type = EXCL; mx = LEN(my); }
    return 1;
  }
  case 'b': case 'B': if (!bck_word(n, k == 'B') && my == cy && mx == cx) return 0; return 1;
  case 'e': case 'E':
    *type = INCL;
    end_word(n, k == 'E', 0);
    if (mx >= LEN(my) && LEN(my)) mx = lastc(my);
    return my != cy || mx != cx || op;
  case 'f': case 't': case 'F': case 'T': {
    int c = getkey();
    if (c == K_ESC) return 0;
    find_kind = k; find_c = c;
    if (k == 'f' || k == 't') *type = INCL;
    return find_char(k, c, n);
  }
  case ';': case ',': {
    if (!find_kind) return 0;
    int kind = find_kind;
    if (k == ',') kind = kind == 'f' ? 'F' : kind == 'F' ? 'f' : kind == 't' ? 'T' : 't';
    if (kind == 'f' || kind == 't') *type = INCL;
    if ((kind == 't' || kind == 'T') && count <= 1) { /* ; after t must skip the char it stopped before */
      int save = mx;
      mx = kind == 't' ? nextc(my, mx) : prevc(my, mx);
      if (!find_char(kind, find_c, n)) { mx = save; return 0; }
      return 1;
    }
    return find_char(kind, find_c, n);
  }
  case '%': *type = INCL; return match_pair();
  case 'H': case 'M': case 'L': {
    *type = LINEW;
    int last = top + th_ - 1 < nl - 1 ? top + th_ - 1 : nl - 1;
    if (k == 'H') my = top + n - 1 < last ? top + n - 1 : last;
    else if (k == 'L') my = last - (n - 1) > top ? last - (n - 1) : top;
    else my = top + (last - top) / 2;
    mx = first_nb(my);
    return 1;
  }
  case '/': case '?': {
    if (!read_cmdline(k)) return 0;
    if (cmdline[1]) snprintf(last_search, sizeof last_search, "%s", cmdline + 1);
    last_dir = k == '/' ? 1 : -1;
    for (int i = 0; i < n; i++) if (!search(last_dir, 0)) return 0;
    return 1;
  }
  case 'n': case 'N':
    for (int i = 0; i < n; i++) if (!search(k == 'n' ? last_dir : -last_dir, 0)) return 0;
    return 1;
  case '*': case '#': {
    char w[256];
    word_under_cursor(w, sizeof w - 8);
    if (!w[0]) { set_msg(1, "E348: No string under cursor"); return 0; }
    snprintf(last_search, sizeof last_search, "%s", w);
    last_dir = k == '*' ? 1 : -1;
    for (int i = 0; i < n; i++) if (!search(last_dir, 0)) return 0;
    return 1;
  }
  }
  return 0;
}

/* ---------- registers and operators ---------- */
static void yank(const char *s, int n, int linewise) {
  free(reg);
  reg = malloc(n + 1);
  memcpy(reg, s, n);
  reg[n] = 0;
  reg_len = n; reg_line = linewise;
}
static void yank_lines(int y1, int y2) {
  int n = 0;
  for (int y = y1; y <= y2; y++) n += LEN(y) + 1;
  char *b = malloc(n + 1), *p = b;
  for (int y = y1; y <= y2; y++) { memcpy(p, S(y), LEN(y)); p += LEN(y); *p++ = '\n'; }
  yank(b, n, 1);
  free(b);
}
static void yank_chars(int y1, int x1, int y2, int x2) { /* [start, end) */
  int n = 0;
  for (int y = y1; y <= y2; y++) n += (y == y2 ? x2 : LEN(y)) - (y == y1 ? x1 : 0) + (y < y2);
  char *b = malloc(n + 1), *p = b;
  for (int y = y1; y <= y2; y++) {
    int a = y == y1 ? x1 : 0, e = y == y2 ? x2 : LEN(y);
    memcpy(p, S(y) + a, e - a); p += e - a;
    if (y < y2) *p++ = '\n';
  }
  yank(b, n, 0);
  free(b);
}
static void delete_chars(int y1, int x1, int y2, int x2) {
  if (y1 == y2) { del_text(y1, x1, x2 - x1); return; }
  Line *last = L[y2];
  last->refs++; /* keep the tail alive while lines are deleted */
  del_text(y1, x1, LEN(y1) - x1);
  ins_text(y1, x1, last->s + x2, last->len - x2);
  lunref(last);
  del_lines(y1 + 1, y2 - y1);
}
static void shift_lines(int y1, int y2, int dir, int times) {
  for (int y = y1; y <= y2; y++) {
    if (!LEN(y)) continue;
    int col = 0, x = 0;
    while (x < LEN(y) && is_blank(S(y)[x])) { col = S(y)[x] == '\t' ? (col / 8 + 1) * 8 : col + 1; x++; }
    int nc = col + dir * 8 * times;
    if (nc < 0) nc = 0;
    char ind[1024];
    int k = 0;
    while (nc >= 8 && k < 1000) { ind[k++] = '\t'; nc -= 8; }
    while (nc-- > 0 && k < 1020) ind[k++] = ' ';
    del_text(y, 0, x);
    ins_text(y, 0, ind, k);
  }
  cy = y1; cx = first_nb(y1);
}
static int inindent(int y, int x) { for (int i = 0; i < x; i++) if (!is_blank(S(y)[i])) return 0; return 1; }
static void insert_mode(int how, int count);
static void join_lines(int y, int count, int spaces);

/* apply operator op over the motion from the cursor to (my, mx) */
static void apply_op(int op, int type) {
  int y1 = cy, x1 = cx, y2 = my, x2 = mx;
  if (y2 < y1 || (y2 == y1 && x2 < x1)) { int t = y1; y1 = y2; y2 = t; t = x1; x1 = x2; x2 = t; }
  if (type == INCL) x2 = nextc(y2, x2);
  if (type == EXCL && x2 == 0 && y2 > y1) { /* vim: an exclusive motion ending in column 0 */
    y2--;
    if (inindent(y1, x1)) type = LINEW;
    else x2 = LEN(y2);
  }
  if (type == LINEW) {
    if (op == 'y') { yank_lines(y1, y2); cy = y1 < cy ? y1 : cy; if (my < cy) cy = my; return; }
    if (op == '>' || op == '<') { shift_lines(y1, y2, op == '>' ? 1 : -1, 1); return; }
    if (op == '~' || op == 'u' || op == 'U') return;
    yank_lines(y1, y2);
    if (op == 'c') {
      del_lines(y1 + 1, y2 - y1);
      del_text(y1, 0, LEN(y1));
      cy = y1; cx = 0;
      insert_mode('c', 1);
      return;
    }
    del_lines(y1, y2 - y1 + 1);
    cy = y1 < nl ? y1 : nl - 1;
    cx = first_nb(cy);
    return;
  }
  if (op == '>' || op == '<') { shift_lines(y1, y2, op == '>' ? 1 : -1, 1); return; }
  if (op == 'y') { yank_chars(y1, x1, y2, x2); cy = y1; cx = x1; return; }
  yank_chars(y1, x1, y2, x2);
  delete_chars(y1, x1, y2, x2);
  cy = y1; cx = x1;
  if (op == 'c') { insert_mode('c', 1); return; }
  clamp();
}

/* ---------- insert mode ---------- */
static char *ins_buf;
static int ins_len, ins_cap;
static void ins_add(const char *s, int n) {
  if (ins_len + n > ins_cap) { ins_cap = (ins_len + n) * 2 + 64; ins_buf = realloc(ins_buf, ins_cap); }
  memcpy(ins_buf + ins_len, s, n);
  ins_len += n;
}
static void insert_mode(int how, int count) {
  mode = M_INSERT;
  ins_len = 0;
  for (;;) {
    int k = getkey();
    if (k == K_ESC || k == 3) break;
    if (k == '\r' || k == '\n') { split_line(cy, cx); cy++; cx = 0; ins_add("\n", 1); continue; }
    if (k == K_BS || k == 8) {
      if (cx > 0) { int p = prevc(cy, cx); del_text(cy, p, cx - p); cx = p; }
      else if (cy > 0) join_lines(cy - 1, 2, 0);
      if (ins_len) { ins_len--; while (ins_len > 0 && u8_cont((unsigned char)ins_buf[ins_len])) ins_len--; }
      continue;
    }
    if (k == K_DEL) {
      if (cx < LEN(cy)) del_text(cy, cx, nextc(cy, cx) - cx);
      else if (cy + 1 < nl) join_lines(cy, 2, 0);
      continue;
    }
    if (k == 23) { /* Ctrl-W: delete the word before the cursor */
      int x = cx;
      while (x > 0 && is_blank(S(cy)[x - 1])) x--;
      if (x > 0 && is_word((unsigned char)S(cy)[x - 1])) while (x > 0 && is_word((unsigned char)S(cy)[x - 1])) x--;
      else if (x > 0 && x == cx) x--;
      del_text(cy, x, cx - x); cx = x; ins_len = 0;
      continue;
    }
    if (k == 21) { del_text(cy, 0, cx); cx = 0; ins_len = 0; continue; } /* Ctrl-U */
    if (k == K_LEFT) { if (cx > 0) cx = prevc(cy, cx); ins_len = 0; continue; }
    if (k == K_RIGHT) { if (cx < LEN(cy)) cx = nextc(cy, cx); ins_len = 0; continue; }
    if (k == K_UP || k == K_DOWN) {
      int w = dcol(cy, cx);
      cy += k == K_UP ? -1 : 1;
      if (cy < 0) cy = 0;
      if (cy >= nl) cy = nl - 1;
      cx = x_at_dcol(cy, w, 1);
      ins_len = 0;
      continue;
    }
    if (k == K_HOME) { cx = 0; ins_len = 0; continue; }
    if (k == K_END) { cx = LEN(cy); ins_len = 0; continue; }
    if (k == '\t' || (k >= 32 && k < 256 && k != 127)) {
      char b[4] = {(char)k};
      int n = 1;
      if (k >= 0xc0) { int want_n = u8_len(k); while (n < want_n) { int c = getkey(); if (c < 0x80 || c > 0xbf) break; b[n++] = (char)c; } }
      ins_text(cy, cx, b, n);
      cx += n;
      ins_add(b, n);
    }
  }
  for (int i = 1; i < count; i++) { /* 3ifoo<Esc> */
    if (how == 'o' || how == 'O') { ins_line(cy + 1, "", 0); cy++; cx = 0; }
    int ey, ex;
    put_text(cy, cx, ins_buf ? ins_buf : "", ins_len, &ey, &ex);
    cy = ey; cx = ex;
  }
  mode = M_NORMAL;
  if (cx > 0) cx = prevc(cy, cx);
  clamp();
  set_want();
}
static void join_lines(int y, int count, int spaces) { /* J: count lines starting at y */
  if (count < 2) count = 2;
  for (int i = 1; i < count && y + 1 < nl; i++) {
    int l1 = LEN(y);
    const char *n = S(y + 1);
    int skip = 0, nlen = LEN(y + 1);
    if (spaces) while (skip < nlen && is_blank(n[skip])) skip++;
    int add_space = spaces && skip < nlen && l1 > 0 && !is_blank(S(y)[l1 - 1]) && n[skip] != ')';
    Line *next = L[y + 1];
    next->refs++;
    if (add_space) ins_text(y, l1, " ", 1);
    int at = LEN(y);
    ins_text(y, at, next->s + skip, next->len - skip);
    lunref(next);
    del_lines(y + 1, 1);
    cx = spaces ? (add_space ? l1 : (l1 > 0 && skip < nlen ? at : (at > 0 ? prevc(y, at) : 0))) : l1;
    if (spaces && !add_space && skip >= nlen) cx = l1 > 0 ? prevc(y, l1) : 0;
  }
  cy = y;
}

/* ---------- command line ---------- */
static int read_cmdline(int prompt) { /* returns 1 with the text in cmdline (prompt char first) */
  int prev_mode = mode;
  cmd_active = 1;
  mode = M_CMD;
  if (cmd_len == 0 || cmdline[0] != prompt) { cmdline[0] = (char)prompt; cmd_len = 1; }
  int ok = 0;
  for (;;) {
    int k = getkey();
    if (k == K_ESC || k == 3) break;
    if (k == '\r' || k == '\n') { ok = 1; break; }
    if (k == K_BS || k == 8) {
      if (cmd_len <= 1) break;
      cmd_len--;
      while (cmd_len > 1 && u8_cont((unsigned char)cmdline[cmd_len])) cmd_len--;
      continue;
    }
    if (k == 21) { cmd_len = 1; continue; }
    if (k == 23) {
      while (cmd_len > 1 && cmdline[cmd_len - 1] == ' ') cmd_len--;
      while (cmd_len > 1 && cmdline[cmd_len - 1] != ' ') cmd_len--;
      continue;
    }
    if ((k >= 32 || k == '\t') && k < 256 && cmd_len < (int)sizeof cmdline - 2) cmdline[cmd_len++] = (char)k;
  }
  cmdline[cmd_len] = 0;
  cmd_active = 0;
  cmd_len = 0;
  mode = prev_mode;
  return ok;
}

static void run_normal(void);
static int parse_addr(const char **pp, int *out) {
  const char *p = *pp;
  while (*p == ' ') p++;
  int v;
  if (isdigit((unsigned char)*p)) { v = 0; while (isdigit((unsigned char)*p)) v = v * 10 + *p++ - '0'; v--; }
  else if (*p == '.') { v = cy; p++; }
  else if (*p == '$') { v = nl - 1; p++; }
  else if (*p == '\'' && (p[1] == '<' || p[1] == '>')) {
    v = p[1] == '<' ? mark_lt : mark_gt;
    if (v < 0) return -1;
    p += 2;
  } else if (*p == '+' || *p == '-') v = cy;
  else return 0;
  while (*p == '+' || *p == '-') {
    int sign = *p++ == '+' ? 1 : -1, d = 0, any = 0;
    while (isdigit((unsigned char)*p)) { d = d * 10 + *p++ - '0'; any = 1; }
    v += sign * (any ? d : 1);
  }
  *pp = p;
  *out = v;
  return 1;
}
static int substitute(int y1, int y2, const char *args) {
  char delim = *args;
  if (!delim || isalnum((unsigned char)delim) || delim == ' ') { set_msg(1, "E146: Regular expressions can't be delimited by letters"); return 0; }
  char pat[256], rep[512];
  int pn = 0, rn = 0;
  const char *p = args + 1;
  while (*p && *p != delim && pn < 250) {
    if (*p == '\\' && p[1] == delim) { pat[pn++] = delim; p += 2; continue; }
    if (*p == '\\' && p[1]) pat[pn++] = *p++;
    pat[pn++] = *p++;
  }
  pat[pn] = 0;
  if (*p == delim) p++;
  while (*p && *p != delim && rn < 500) {
    if (*p == '\\' && p[1]) { rep[rn++] = *p++; }
    rep[rn++] = *p++;
  }
  rep[rn] = 0;
  if (*p == delim) p++;
  int global = 0;
  for (; *p; p++) if (*p == 'g') global = 1;
  if (pn) snprintf(last_search, sizeof last_search, "%s", pat);
  const char *re = last_search;
  if (!re[0]) { set_msg(1, "E35: No previous regular expression"); return 0; }
  int count = 0, lines = 0, last_line = -1;
  for (int y = y1; y <= y2 && y < nl; y++) {
    const char *s = S(y);
    int len = LEN(y), pos = 0, ms, me, changed = 0;
    char *out = 0;
    int on = 0, ocap = 0;
#define OUT(src, n) do { int _n = (n); if (on + _n + 1 > ocap) { ocap = (on + _n) * 2 + 64; out = realloc(out, ocap); } memcpy(out + on, (src), _n); on += _n; } while (0)
    while (pos <= len && re_search(re, s, len, pos, &ms, &me)) {
      OUT(s + pos, ms - pos);
      for (int i = 0; i < rn; i++) {
        if (rep[i] == '&') OUT(s + ms, me - ms);
        else if (rep[i] == '\\' && i + 1 < rn) {
          i++;
          if (rep[i] == 't') OUT("\t", 1);
          else if (rep[i] == 'n' || rep[i] == 'r') OUT("\n", 1);
          else OUT(rep + i, 1);
        } else OUT(rep + i, 1);
      }
      count++;
      changed = 1;
      if (me == ms) { if (ms < len) { int nx = nextc(y, ms); OUT(s + ms, nx - ms); pos = nx; } else { pos = len + 1; } }
      else pos = me;
      if (!global) break;
    }
    if (changed) {
      if (pos < len) OUT(s + pos, len - pos);
      if (!out) out = malloc(1);
      char *nlp = memchr(out, '\n', on);
      if (nlp) { /* \r in the replacement splits the line */
        set_line(y, "", 0);
        int ey, ex;
        put_text(y, 0, out, on, &ey, &ex);
        y2 += ey - y; y = ey;
      } else set_line(y, out, on);
      lines++;
      last_line = y;
    }
    free(out);
#undef OUT
  }
  if (!count) { set_msg(1, "E486: Pattern not found: %s", re); return 0; }
  cy = last_line; cx = first_nb(cy); set_want();
  if (lines > 1 || count > 1) set_msg(0, "%d substitution%s on %d line%s", count, count > 1 ? "s" : "", lines, lines > 1 ? "s" : "");
  return 1;
}
static void ex_command(const char *line) {
  const char *p = line;
  while (*p == ' ' || *p == ':') p++;
  if (!*p) return;
  int y1 = cy, y2 = cy, naddr = 0, a;
  if (*p == '%') { y1 = 0; y2 = nl - 1; naddr = 2; p++; }
  else {
    int r = parse_addr(&p, &a);
    if (r < 0) { set_msg(1, "E20: Mark not set"); return; }
    if (r) {
      y1 = y2 = a; naddr = 1;
      while (*p == ' ') p++;
      if (*p == ',' || *p == ';') {
        p++;
        if (parse_addr(&p, &a) <= 0) { set_msg(1, "E14: Invalid address"); return; }
        y2 = a; naddr = 2;
      }
    }
  }
  while (*p == ' ') p++;
  char name[32];
  int n = 0;
  while (isalpha((unsigned char)*p) && n < 30) name[n++] = *p++;
  name[n] = 0;
  if (!n && (*p == '>' || *p == '<' || *p == '&' || *p == '=')) { name[n++] = *p++; name[n] = 0; }
  int bang = 0;
  if (*p == '!') { bang = 1; p++; }
  while (*p == ' ') p++;
  const char *arg = p;
  if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
  if (naddr && (y1 < 0 || y2 >= nl)) {
    if (!n) { cy = y2 < 0 ? 0 : y2 >= nl ? nl - 1 : y2; cx = first_nb(cy); set_want(); return; }
    set_msg(1, "E16: Invalid range"); return;
  }
  if (!n) { cy = y2; cx = first_nb(cy); set_want(); return; } /* :N goes to line N */
#define IS(full, min) (n >= (min) && n <= (int)strlen(full) && !strncmp(name, full, n))
  if (IS("write", 1) || IS("wq", 2) || IS("xit", 1) || IS("exit", 3) || IS("wall", 2)) {
    int quit = name[0] == 'x' || name[0] == 'e' || (name[0] == 'w' && name[1] == 'q');
    const char *path = *arg ? arg : filename;
    if (!*path) { set_msg(1, "E32: No file name"); return; }
    if (!filename[0]) snprintf(filename, sizeof filename, "%s", path);
    if ((name[0] == 'x' || name[0] == 'e') && !modified && *arg == 0) { quit_flag = 1; return; }
    if (write_file(path) == 0 && quit) quit_flag = 1;
    return;
  }
  if (IS("quit", 1) || IS("qall", 2)) {
    if (modified && !bang) { set_msg(1, "E37: No write since last change (add ! to override)"); return; }
    quit_flag = 1;
    return;
  }
  if (IS("edit", 1)) {
    if (modified && !bang) { set_msg(1, "E37: No write since last change (add ! to override)"); return; }
    if (!*arg && !filename[0]) { set_msg(1, "E32: No file name"); return; }
    char path[512];
    snprintf(path, sizeof path, "%s", *arg ? arg : filename);
    open_file(path);
    return;
  }
  if (IS("set", 2)) {
    while (*arg) {
      char opt[64];
      int k = 0;
      while (*arg && *arg != ' ' && k < 62) opt[k++] = *arg++;
      opt[k] = 0;
      while (*arg == ' ') arg++;
      if (!strcmp(opt, "nu") || !strcmp(opt, "number")) number = 1;
      else if (!strcmp(opt, "nonu") || !strcmp(opt, "nonumber")) number = 0;
      else if (!strcmp(opt, "nu!") || !strcmp(opt, "invnumber") || !strcmp(opt, "invnu")) number = !number;
      else if (!strcmp(opt, "wrap")) wrap_on = 1;
      else if (!strcmp(opt, "nowrap")) wrap_on = 0;
      else { set_msg(1, "E518: Unknown option: %s", opt); return; }
    }
    return;
  }
  if (IS("substitute", 1) || !strcmp(name, "&")) { if (name[0] == '&') arg = "//"; substitute(y1, y2, arg); return; }
  if (IS("delete", 1)) { yank_lines(y1, y2); del_lines(y1, y2 - y1 + 1); cy = y1 < nl ? y1 : nl - 1; cx = first_nb(cy); return; }
  if (IS("yank", 1)) { yank_lines(y1, y2); return; }
  if (IS("join", 1)) { join_lines(y1, naddr == 2 ? y2 - y1 + 1 : 2, !bang); return; }
  if (!strcmp(name, ">") || !strcmp(name, "<")) { shift_lines(y1, y2, name[0] == '>' ? 1 : -1, 1); return; }
  if (IS("print", 1) || IS("number", 2) || !strcmp(name, "=")) {
    if (name[0] == '=') { set_msg(0, "%d", naddr ? y2 + 1 : nl); return; }
    for (int y = y1; y <= y2; y++) {
      if (interactive) { set_msg(0, "%.*s", LEN(y), S(y)); continue; }
      if (name[0] == 'n') printf("%6d  ", y + 1);
      fwrite(S(y), 1, LEN(y), stdout);
      putchar('\n');
    }
    cy = y2;
    return;
  }
  if (IS("normal", 4)) {
    static int keys[4096];
    int nk = 0;
    for (const unsigned char *s = (const unsigned char *)arg; *s && nk < 4096; s++) keys[nk++] = *s;
    int save = no_term;
    no_term = 1;
    q_push_front(keys, nk);
    while (qn && !quit_flag) run_normal();
    no_term = save;
    return;
  }
  if (IS("nohlsearch", 3)) return;
  set_msg(1, "E492: Not an editor command: %s", line);
#undef IS
}

/* ---------- normal mode ---------- */
static int cmd_count;
static void put_reg(int after, int count) {
  if (!reg) { set_msg(1, "E353: Nothing in register \""); return; }
  if (reg_line) {
    int at = after ? cy + 1 : cy;
    int nlines = 0;
    for (int c = 0; c < count; c++) {
      const char *p = reg, *end = reg + reg_len;
      for (const char *q; p < end && (q = memchr(p, '\n', end - p)); p = q + 1) { ins_line(at + nlines, p, (int)(q - p)); nlines++; }
    }
    cy = at; cx = first_nb(cy);
    return;
  }
  int x = cx;
  if (after && LEN(cy)) x = nextc(cy, cx);
  int ey = cy, ex = x;
  int sy = cy, sx = x;
  for (int c = 0; c < count; c++) put_text(ey, ex, reg, reg_len, &ey, &ex);
  if (memchr(reg, '\n', reg_len)) { cy = sy; cx = sx; }
  else { cy = ey; cx = ex > 0 ? prevc(ey, ex) : 0; }
}
static void visual_mode(void) {
  mode = M_VISUAL;
  vis_y = cy;
  for (;;) {
    int k = getkey(), count = 0, has = 0;
    while ((k >= '1' && k <= '9') || (k == '0' && count)) { count = count * 10 + k - '0'; has = 1; k = getkey(); }
    if (k == K_ESC || k == 'V' || k == 3) break;
    int y1 = vis_y < cy ? vis_y : cy, y2 = vis_y < cy ? cy : vis_y, type;
    if (k == 'o') { int t = vis_y; vis_y = cy; cy = t; cx = first_nb(cy); continue; }
    if (strchr("dxXDycsSCR><J~uU:", k) == 0 || k > 255 || k == 0) {
      if (motion(k, count, has, 0, &type)) { cy = my; cx = mx; clamp(); if (type == LINEW) cx = x_at_dcol(cy, want, 0); else set_want(); }
      continue;
    }
    mark_lt = y1; mark_gt = y2;
    int nlines = y2 - y1 + 1;
    rec_n = 0;
    if (k == ':') {
      mode = M_NORMAL;
      cmd_len = 0;
      strcpy(cmdline, ":'<,'>");
      cmd_len = 6;
      if (read_cmdline(':')) ex_command(cmdline + 1);
      return;
    }
    if (k == 'y' || k == 'Y') { yank_lines(y1, y2); cy = y1; cx = first_nb(cy); break; }
    if (k == '>' || k == '<') {
      shift_lines(y1, y2, k == '>' ? 1 : -1, count ? count : 1);
      rec[rec_n++] = k; rec[rec_n++] = k; cmd_count = nlines; break;
    }
    if (k == 'J') {
      join_lines(y1, nlines < 2 ? 2 : nlines, 1);
      rec[rec_n++] = 'J'; cmd_count = nlines; break;
    }
    if (k == '~' || k == 'u' || k == 'U') {
      for (int y = y1; y <= y2; y++) {
        Line *l = lmut(y);
        for (int i = 0; i < l->len; i++) {
          unsigned char c = l->s[i];
          if (k == 'u') l->s[i] = tolower(c); else if (k == 'U') l->s[i] = toupper(c);
          else l->s[i] = islower(c) ? toupper(c) : isupper(c) ? tolower(c) : c;
        }
      }
      cy = y1; cx = 0;
      break;
    }
    if (k == 'c' || k == 's' || k == 'S' || k == 'C' || k == 'R') {
      mode = M_NORMAL;
      rec[rec_n++] = 'c'; rec[rec_n++] = 'c'; cmd_count = nlines;
      yank_lines(y1, y2);
      del_lines(y1 + 1, y2 - y1);
      del_text(y1, 0, LEN(y1));
      cy = y1; cx = 0;
      insert_mode('c', 1);
      return;
    }
    /* d x X D */
    yank_lines(y1, y2);
    del_lines(y1, nlines);
    cy = y1 < nl ? y1 : nl - 1;
    cx = first_nb(cy);
    rec[rec_n++] = 'd'; rec[rec_n++] = 'd'; cmd_count = nlines;
    break;
  }
  mode = M_NORMAL;
  clamp();
}
static int is_change_cmd;
static void run_normal(void) {
  cmd_cy = cy; cmd_cx = cx;
  int count = 0, has = 0;
  rec_n = 0; recording = 1; is_change_cmd = 0;
  int k = getkey();
  while ((k >= '1' && k <= '9') || (k == '0' && count)) { count = count * 10 + k - '0'; has = 1; k = getkey(); }
  if (forced_count) { count = forced_count; has = 1; forced_count = 0; }
  rec_n = 0;
  rec[rec_n++] = k;
  cmd_count = count;
  int n = count ? count : 1, type;
  switch (k) {
  case K_ESC: case 0: break;
  case 'i': case K_INS: is_change_cmd = 1; insert_mode('i', n); break;
  case 'a': is_change_cmd = 1; if (LEN(cy)) cx = nextc(cy, cx); insert_mode('a', n); break;
  case 'I': is_change_cmd = 1; cx = 0; while (cx < LEN(cy) && is_blank(S(cy)[cx])) cx++; insert_mode('I', n); break;
  case 'A': is_change_cmd = 1; cx = LEN(cy); insert_mode('A', n); break;
  case 'o': is_change_cmd = 1; ins_line(cy + 1, "", 0); cy++; cx = 0; insert_mode('o', n); break;
  case 'O': is_change_cmd = 1; ins_line(cy, "", 0); cx = 0; insert_mode('O', n); break;
  case 'x': case K_DEL: case 'X': case 's':
    is_change_cmd = 1;
    if (!LEN(cy)) { if (k == 's') insert_mode('i', 1); break; }
    if (k == 'X') {
      if (cx == 0) break;
      int x = cx;
      for (int i = 0; i < n && x > 0; i++) x = prevc(cy, x);
      yank_chars(cy, x, cy, cx); del_text(cy, x, cx - x); cx = x;
    } else {
      int e = cx;
      for (int i = 0; i < n && e < LEN(cy); i++) e = nextc(cy, e);
      yank_chars(cy, cx, cy, e); del_text(cy, cx, e - cx);
      if (k == 's') { insert_mode('i', 1); break; }
    }
    clamp(); set_want();
    break;
  case 'D': case 'C':
    is_change_cmd = 1;
    my = cy + n - 1 < nl ? cy + n - 1 : nl - 1;
    mx = LEN(my);
    if (my == cy && cx >= LEN(cy)) { if (k == 'C') { cx = LEN(cy); insert_mode('A', 1); } break; }
    apply_op(k == 'D' ? 'd' : 'c', EXCL);
    if (k == 'D') { clamp(); set_want(); }
    break;
  case 'S': is_change_cmd = 1; my = cy + n - 1 < nl ? cy + n - 1 : nl - 1; apply_op('c', LINEW); break;
  case 'Y': my = cy + n - 1 < nl ? cy + n - 1 : nl - 1; yank_lines(cy, my); break;
  case 'p': case 'P': is_change_cmd = 1; put_reg(k == 'p', n); set_want(); break;
  case 'J': is_change_cmd = 1; if (cy + 1 >= nl) break; join_lines(cy, n, 1); set_want(); break;
  case 'r': {
    is_change_cmd = 1;
    int c = getkey();
    if (c == K_ESC || !LEN(cy)) break;
    char b[4] = {(char)c};
    int bl = 1;
    if (c >= 0xc0) { int w = u8_len(c); while (bl < w) b[bl++] = (char)getkey(); }
    int e = cx, cnt = 0;
    while (cnt < n && e < LEN(cy)) { e = nextc(cy, e); cnt++; }
    if (cnt < n) break;
    if (c == '\r') { del_text(cy, cx, e - cx); split_line(cy, cx); cy++; cx = 0; break; }
    del_text(cy, cx, e - cx);
    for (int i = 0; i < n; i++) ins_text(cy, cx + i * bl, b, bl);
    cx += (n - 1) * bl;
    set_want();
    break;
  }
  case '~': {
    is_change_cmd = 1;
    if (!LEN(cy)) break;
    for (int i = 0; i < n && cx < LEN(cy); i++) {
      unsigned char c = S(cy)[cx];
      if (isalpha(c)) { char b = islower(c) ? toupper(c) : tolower(c); Line *l = lmut(cy); l->s[cx] = b; }
      if (nextc(cy, cx) >= LEN(cy)) break;
      cx = nextc(cy, cx);
    }
    set_want();
    break;
  }
  case 'u':
    for (int i = 0; i < n; i++) {
      if (!nund) { set_msg(1, "Already at oldest change"); break; }
      red[nred++] = snap_take(cy, cx);
      Snap s = und[--nund];
      snap_restore(&s);
      modified = 1;
    }
    clamp(); set_want();
    break;
  case 18: /* Ctrl-R */
    for (int i = 0; i < n; i++) {
      if (!nred) { set_msg(1, "Already at newest change"); break; }
      und[nund++] = snap_take(cy, cx);
      Snap s = red[--nred];
      int ry = s.cy, rx = s.cx;
      snap_restore(&s);
      cy = ry; cx = rx;
      modified = 1;
    }
    clamp(); set_want();
    break;
  case '.':
    if (!dot_n) break;
    forced_count = count ? count : dot_count;
    q_push_front(dot, dot_n);
    recording = 0;
    run_normal();
    return;
  case 'V': visual_mode(); if (rec_n && rec[0] != 'V') is_change_cmd = 1; count = cmd_count; break;
  case ':':
    if (read_cmdline(':')) {
      if (has) { /* 3: means .,.+2 */
        char tmp[600];
        snprintf(tmp, sizeof tmp, ".,.+%d%s", count - 1, cmdline + 1);
        ex_command(tmp);
      } else ex_command(cmdline + 1);
    }
    clamp();
    break;
  case 'Z': {
    int c = getkey();
    if (c == 'Z') ex_command("x");
    else if (c == 'Q') ex_command("q!");
    break;
  }
  case 6: case 2: { /* Ctrl-F, Ctrl-B: a page minus two lines */
    int th_ = text_rows(), step = th_ > 2 ? th_ - 2 : 1;
    for (int i = 0; i < n; i++) {
      if (k == 6) { if (top + step >= nl) { top = nl - 1 > top ? nl - 1 : top; break; } top += step; }
      else { if (top == 0) break; top = top - step < 0 ? 0 : top - step; }
    }
    if (k == 2 && n > 0) cy = top + th_ - 1; /* Ctrl-B leaves the cursor on the bottom line */
    if (cy < top) cy = top;
    if (cy >= top + th_) cy = top + th_ - 1;
    if (cy >= nl) cy = nl - 1;
    cx = first_nb(cy); set_want();
    break;
  }
  case 4: case 21: { /* Ctrl-D, Ctrl-U: half a page */
    int th_ = text_rows(), amount = count ? count : th_ / 2;
    if (k == 4) {
      if (cy == nl - 1) break;
      int maxtop = nl - th_ > 0 ? nl - th_ : 0;
      top = top + amount > maxtop ? (top > maxtop ? top : maxtop) : top + amount;
      cy = cy + amount < nl ? cy + amount : nl - 1;
    } else {
      if (cy == 0) break;
      top = top - amount > 0 ? top - amount : 0;
      cy = cy - amount > 0 ? cy - amount : 0;
    }
    if (cy < top) cy = top;
    cx = first_nb(cy); set_want();
    break;
  }
  case 7: { /* Ctrl-G */
    set_msg(0, "\"%s\"%s %d line%s --%d%%--", filename[0] ? filename : "[No Name]", modified ? " [Modified]" : "", nl, nl == 1 ? "" : "s", (cy + 1) * 100 / nl);
    break;
  }
  case 12: /* Ctrl-L: full redraw, picking up the desktop theme */
    theme_reload();
    for (int i = 0; i < prev_R; i++) { free(prev_row[i]); prev_row[i] = 0; }
    ob_puts("\033[2J");
    break;
  case 'd': case 'c': case 'y': case '>': case '<': {
    int op = k;
    if (op != 'y') is_change_cmd = 1;
    int k2 = getkey(), c2 = 0, has2 = 0;
    while ((k2 >= '1' && k2 <= '9') || (k2 == '0' && c2)) { c2 = c2 * 10 + k2 - '0'; has2 = 1; k2 = getkey(); }
    int total = (count ? count : 1) * (c2 ? c2 : 1);
    int hasc = has || has2;
    if (k2 == op) { /* dd cc yy >> << */
      my = cy + total - 1 < nl ? cy + total - 1 : nl - 1;
      mx = 0;
      if (op == 'y') { yank_lines(cy, my); break; }
      apply_op(op, LINEW);
      break;
    }
    if (k2 == K_ESC) break;
    if (!motion(k2, hasc ? total : 0, hasc, op, &type)) break;
    if (type == LINEW && (op == '>' || op == '<')) { int y1 = cy < my ? cy : my, y2 = cy < my ? my : cy; shift_lines(y1, y2, op == '>' ? 1 : -1, 1); break; }
    apply_op(op, type);
    if (op != 'c') { clamp(); set_want(); }
    break;
  }
  default:
    if (motion(k, count, has, 0, &type)) {
      cy = my; cx = mx;
      if (type == LINEW && k != 'G' && k != 'g' && k != '+' && k != '-' && k != '\r' && k != '_' && k != 'H' && k != 'M' && k != 'L') {
        clamp();
        cx = x_at_dcol(cy, want, 0);
      } else {
        clamp();
        if (k != '$' && k != K_END) set_want();
      }
    }
    break;
  }
  recording = 0;
  if (is_change_cmd && rec_n) {
    memcpy(dot, rec, rec_n * sizeof *rec);
    dot_n = rec_n;
    dot_count = cmd_count;
  }
  end_change();
  clamp();
}

/* ---------- start and stop ---------- */
static void quit_now(int code) {
  if (interactive) {
    ob_puts("\033[0m\033[?25h\033[?1049l");
    ob_flush();
    tty_restore();
  }
  exit(code);
}
static void batch_mode(void) { /* not a terminal: one ex command per input line */
  char line[4096];
  while (!quit_flag && fgets(line, sizeof line, stdin)) {
    line[strcspn(line, "\r\n")] = 0;
    cmd_cy = cy; cmd_cx = cx;
    ex_command(line);
    end_change();
    clamp();
    if (msg[0] && msg_err) msg[0] = 0;
  }
  if (!quit_flag && modified) fprintf(stderr, "vi: input ended with unsaved changes\n");
}
int main(int argc, char **argv) {
  theme_init();
  const char *file = 0;
  for (int i = 1; i < argc; i++) {
    if (argv[i][0] == '+' && isdigit((unsigned char)argv[i][1])) { /* +N: start on line N */ }
    else if (!file) file = argv[i];
  }
  interactive = tty_check(0) && tty_check(1); /* before open_file: its message must not go to stdout */
  if (file) open_file(file);
  else { lreserve(1); L[0] = lnew("", 0); nl = 1; }
  cx = first_nb(0);
  for (int i = 1; i < argc; i++)
    if (argv[i][0] == '+' && isdigit((unsigned char)argv[i][1])) { cy = atoi(argv[i] + 1) - 1; clamp(); cx = first_nb(cy); }
  if (!interactive) {
    fprintf(stderr, "vi: not a terminal: reading ex commands from standard input\n");
    batch_mode();
    return 0;
  }
  tty_size(1, &R, &C);
  tty_raw(0);
  ob_puts("\033[?1049h");
  while (!quit_flag) run_normal();
  quit_now(0);
  return 0;
}
