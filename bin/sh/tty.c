// SPDX-License-Identifier: GPL-2.0-or-later
/* Interactive front end of the line editor: raw mode, drawing, completion.
 * Used only when stdin is a terminal (TCGETS succeeds). */
#include "sh.h"
#include "lineedit.h"
#include <dirent.h>
#include <termios.h>

static struct termios orig;
static int have_tty, raw_on, cur_row;

int tty_init(void) {
  have_tty = __syscall3(SYS_ioctl, 0, TCGETS, &orig) == 0;
  return have_tty;
}
static void raw_mode(void) {
  struct termios t;
  if (__syscall3(SYS_ioctl, 0, TCGETS, &t) == 0) orig = t;
  /* commands run in cooked mode with ISIG, even if a program left the tty raw */
  orig.c_lflag |= ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN;
  orig.c_iflag |= ICRNL;
  orig.c_oflag |= OPOST | ONLCR;
  orig.c_cc[VINTR] = orig.c_cc[VINTR] ? orig.c_cc[VINTR] : 3;
  /* job control: Ctrl-Z stops the foreground job (the kernel leaves VSUSP off by default) */
  if (job_control_active() && !orig.c_cc[VSUSP]) orig.c_cc[VSUSP] = 26;
  t = orig;
  t.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
  t.c_iflag &= ~ICRNL;
  t.c_cc[VMIN] = 1;
  t.c_cc[VTIME] = 0;
  if (__syscall3(SYS_ioctl, 0, TCSETSW, &t) == 0) raw_on = 1;
}
void tty_restore(void) {
  if (!raw_on) return;
  __syscall3(SYS_ioctl, 0, TCSETSW, &orig);
  raw_on = 0;
}
static int term_width(void) {
  struct winsize ws;
  if (__syscall3(SYS_ioctl, 1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
  return 80;
}

/* ---- prompt ---- */
char *expand_prompt(const char *ps) {
  size_t cap = 256, n = 0;
  char *r = malloc(cap);
#define ADD(s) do { const char *_s = (s); size_t _l = strlen(_s); while (n + _l + 1 > cap) r = realloc(r, cap *= 2); memcpy(r + n, _s, _l); n += _l; } while (0)
  for (; *ps; ps++) {
    if (*ps != '\\' || !ps[1]) { char c[2] = {*ps, 0}; ADD(c); continue; }
    ps++;
    char tmp[512];
    switch (*ps) {
    case 'u': { const char *u = getvar("USER"); ADD(u ? u : "user"); break; }
    case 'h': case 'H': {
      struct utsname un;
      if (__syscall1(SYS_uname, &un) < 0) strcpy(un.nodename, "localhost");
      if (*ps == 'h') { char *d = strchr(un.nodename, '.'); if (d) *d = 0; }
      ADD(un.nodename);
      break;
    }
    case 'w': case 'W': {
      if (!getcwd(tmp, sizeof tmp)) strcpy(tmp, "?");
      const char *home = getvar("HOME");
      size_t hl = home ? strlen(home) : 0;
      if (*ps == 'W') { char *b = strrchr(tmp, '/'); ADD(home && !strcmp(tmp, home) ? "~" : (b && b[1]) ? b + 1 : tmp); break; }
      if (hl > 1 && !strncmp(tmp, home, hl) && (tmp[hl] == '/' || !tmp[hl])) { ADD("~"); ADD(tmp + hl); }
      else ADD(tmp);
      break;
    }
    case '$': ADD(getuid() == 0 ? "#" : "$"); break;
    case 'n': ADD("\n"); break;
    case 'e': ADD("\033"); break;
    case 'a': ADD("\a"); break;
    case '\\': ADD("\\"); break;
    case '[': case ']': break;
    case '0': case '1': case '2': case '3': {
      int v = 0, k = 0;
      ps--;
      while (k < 3 && ps[1] >= '0' && ps[1] <= '7') { ps++; v = v * 8 + (*ps - '0'); k++; }
      char c[2] = {(char)v, 0};
      ADD(c);
      break;
    }
    default: { char c[3] = {'\\', *ps, 0}; ADD(c); }
    }
  }
  r[n] = 0;
  return r;
}
/* visible width of a prompt line: escape sequences take no room */
static int prompt_width(const char *s) {
  int w = 0;
  while (*s) {
    if (*s == 27 && s[1] == '[') { s += 2; while (*s && !(*s >= 0x40 && *s <= 0x7e)) s++; if (*s) s++; continue; }
    if (*s == 27 && s[1] == ']') { s += 2; while (*s && *s != 7 && !(*s == 27 && s[1] == '\\')) s++; if (*s == 7) s++; else if (*s) s += 2; continue; }
    if (((unsigned char)*s & 0xc0) != 0x80 && (unsigned char)*s >= 32) w++;
    s++;
  }
  return w;
}

/* ---- drawing ---- */
static void refresh(struct le *e, const char *pline, int pw) {
  int W = term_width();
  int total = pw + le_cols(e->buf, e->len), curc = pw + le_cols(e->buf, e->pos);
  size_t cap = e->len + strlen(pline) + 64;
  char *o = malloc(cap);
  int n = 0;
  if (cur_row > 0) n += sprintf(o + n, "\033[%dA", cur_row);
  o[n++] = '\r';
  memcpy(o + n, pline, strlen(pline)); n += strlen(pline);
  memcpy(o + n, e->buf, e->len); n += e->len;
  n += sprintf(o + n, "\033[J");
  if (total > 0 && total % W == 0) n += sprintf(o + n, "\r\n");
  int end_row = total / W, row = curc / W, col = curc % W;
  if (end_row > row) n += sprintf(o + n, "\033[%dA", end_row - row);
  o[n++] = '\r';
  if (col) n += sprintf(o + n, "\033[%dC", col);
  write(1, o, n);
  free(o);
  cur_row = row;
}

/* ---- completion ---- */
static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static int add_cand(char **out, int n, int max, const char *s) {
  if (n >= max) return n;
  for (int i = 0; i < n; i++) if (!strcmp(out[i], s)) return n;
  out[n] = strdup(s);
  return n + 1;
}
static int complete(void *user, const char *word, int is_cmd, char **out, int max) {
  int n = 0;
  size_t wl = strlen(word);
  if (is_cmd && !strchr(word, '/')) {
    const char *b;
    for (int i = 0; (b = builtin_name(i)); i++) if (!strncmp(b, word, wl)) n = add_cand(out, n, max, b);
    const char *path = getvar("PATH");
    if (!path) path = "/bin";
    char *pc = strdup(path), *save, *dir;
    for (dir = strtok_r(pc, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
      DIR *d = opendir(dir);
      if (!d) continue;
      struct dirent *de;
      while ((de = readdir(d))) if (de->d_name[0] != '.' && !strncmp(de->d_name, word, wl)) n = add_cand(out, n, max, de->d_name);
      closedir(d);
    }
    free(pc);
  } else {
    const char *slash = strrchr(word, '/');
    char dirpart[1024], dirpath[1024];
    size_t dl = slash ? (size_t)(slash - word + 1) : 0;
    if (dl >= sizeof dirpart) return 0;
    memcpy(dirpart, word, dl);
    dirpart[dl] = 0;
    if (dirpart[0] == '~' && (dirpart[1] == '/' || !dirpart[1])) {
      const char *h = getvar("HOME");
      snprintf(dirpath, sizeof dirpath, "%s%s", h ? h : "", dirpart + 1);
    } else strcpy(dirpath, dl ? dirpart : ".");
    const char *base = word + dl;
    size_t bl = strlen(base);
    DIR *d = opendir(dirpath);
    if (d) {
      struct dirent *de;
      while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (de->d_name[0] == '.' && base[0] != '.') continue;
        if (strncmp(de->d_name, base, bl)) continue;
        char c[1200], full[1200];
        snprintf(full, sizeof full, "%s/%s", dirpath, de->d_name);
        struct stat st;
        int isdir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        snprintf(c, sizeof c, "%s%s%s", dirpart, de->d_name, isdir ? "/" : "");
        n = add_cand(out, n, max, c);
      }
      closedir(d);
    }
  }
  qsort(out, n, sizeof *out, cmpstr);
  return n;
}
static void list_cands(struct le *e) {
  int W = term_width(), maxw = 0;
  for (int i = 0; i < e->ncands; i++) {
    const char *s = e->cands[i], *sl = strrchr(s, '/');
    if (sl && sl[1]) s = sl + 1; else if (sl) { const char *p = sl; while (p > e->cands[i] && p[-1] != '/') p--; s = p; }
    int w = le_cols(s, strlen(s));
    if (w > maxw) maxw = w;
  }
  int colw = maxw + 2, cols = W / colw;
  if (cols < 1) cols = 1;
  int rows = (e->ncands + cols - 1) / cols;
  outs(1, "\r\n");
  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < cols; c++) {
      int i = c * rows + r;
      if (i >= e->ncands) break;
      const char *s = e->cands[i], *sl = strrchr(s, '/');
      if (sl && sl[1]) s = sl + 1; else if (sl) { const char *p = sl; while (p > e->cands[i] && p[-1] != '/') p--; s = p; }
      int w = le_cols(s, strlen(s));
      outs(1, s);
      if (c + 1 < cols && i + rows < e->ncands) for (int k = w; k < colw; k++) outs(1, " ");
    }
    outs(1, "\r\n");
  }
}

char *tty_readline(const char *prompt) {
  static struct le e;
  e.complete = complete;
  le_reset(&e, (const char *const *)hist, nhist);
  const char *pline = strrchr(prompt, '\n');
  pline = pline ? pline + 1 : prompt;
  int pw = prompt_width(pline);
  raw_mode();
  sh_write(1, prompt, pline - prompt);
  cur_row = 0;
  refresh(&e, pline, pw);
  for (;;) {
    unsigned char c;
    if (read(0, &c, 1) != 1) { tty_restore(); return NULL; }
    int r = le_feed(&e, c);
    switch (r) {
    case LE_REDRAW: refresh(&e, pline, pw); break;
    case LE_ACCEPT:
      e.pos = e.len;
      refresh(&e, pline, pw);
      outs(1, "\r\n");
      tty_restore();
      return strdup(e.buf);
    case LE_EOF: outs(1, "\r\n"); tty_restore(); return NULL;
    case LE_INTR:
      outs(1, "^C\r\n");
      last_status = 130;
      sh_write(1, prompt, pline - prompt);
      cur_row = 0;
      refresh(&e, pline, pw);
      break;
    case LE_CLEAR:
      outs(1, "\033[H\033[2J");
      sh_write(1, prompt, pline - prompt);
      cur_row = 0;
      refresh(&e, pline, pw);
      break;
    case LE_LIST:
      { int save = e.pos; e.pos = e.len; refresh(&e, pline, pw); e.pos = save; }
      list_cands(&e);
      le_free_cands(&e);
      sh_write(1, prompt, pline - prompt);
      cur_row = 0;
      refresh(&e, pline, pw);
      break;
    case LE_BELL: outs(1, "\a"); break;
    }
  }
}
