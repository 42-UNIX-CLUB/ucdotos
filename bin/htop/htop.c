// SPDX-License-Identifier: GPL-2.0-or-later
/* htop: an interactive process monitor in htop's style.
 * Meters at the top (CPU, memory, kernel heap, syscall rate), then one row per process with
 * CPU% (from cpu_us), memory, syscalls per second (the activity column, with a bar) and run time.
 * Keys: arrows/jk move, PgUp/PgDn, Home/End, F6 or > < cycle the sort column, P M N T S pick one,
 * I inverts, k or F9 kills the selected process (asks first), h or F1 help, q or F10 quits.
 * Not on a terminal: prints one snapshot as a plain table and exits. */
#include "term.h"
#include "info.h"

#ifdef __wasm__
#define MAXP 64
#else
#define MAXP 4096
#endif

struct row {
  struct pinfo p;
  double cpu, rate; /* CPU percent, syscalls per second over the last interval */
};
static struct row rows_[MAXP];
static int nrows;
static struct { int pid; unsigned long long sys, cpu; } prevs[MAXP];
static int nprev;
static double prev_t;
static struct sys_stats st;
static double total_rate, total_cpu, peak_rate = 1;

static void sample(void) {
  static struct pinfo tmp[MAXP];
  double t = now_ms();
  int n = info_procs(tmp, MAXP);
  info_stats(&st);
  double dt = prev_t ? (t - prev_t) / 1000.0 : 0;
  total_rate = total_cpu = 0;
  for (int i = 0; i < n; i++) {
    struct row *r = &rows_[i];
    r->p = tmp[i];
    r->cpu = r->rate = 0;
    for (int k = 0; k < nprev && dt > 0; k++)
      if (prevs[k].pid == tmp[i].pid) {
        if (tmp[i].syscalls >= prevs[k].sys) r->rate = (tmp[i].syscalls - prevs[k].sys) / dt;
        if (tmp[i].cpu_us >= prevs[k].cpu) r->cpu = (tmp[i].cpu_us - prevs[k].cpu) / (dt * 1e4);
        break;
      }
    total_rate += r->rate;
    total_cpu += r->cpu;
  }
  nrows = n;
  for (int i = 0; i < n; i++) { prevs[i].pid = tmp[i].pid; prevs[i].sys = tmp[i].syscalls; prevs[i].cpu = tmp[i].cpu_us; }
  nprev = n;
  prev_t = t;
}

/* ---------- sorting ---------- */
enum { S_PID, S_PPID, S_STATE, S_CPU, S_MEM, S_RATE, S_TIME, S_NAME, S_N };
static const char *sort_names[S_N] = {"PID", "PPID", "S", "CPU%", "MEM", "SYSC/s", "TIME+", "Command"};
static int sort_col = S_CPU, sort_inv;
static int cmp_rows(const void *a, const void *b) {
  const struct row *x = a, *y = b;
  double d = 0;
  switch (sort_col) {
  case S_PID: d = x->p.pid - y->p.pid; break;
  case S_PPID: d = x->p.ppid - y->p.ppid; break;
  case S_STATE: d = x->p.state - y->p.state; break;
  case S_CPU: d = y->cpu - x->cpu; if (d == 0) d = y->rate - x->rate; break;
  case S_MEM: d = y->p.mem - x->p.mem; break;
  case S_RATE: d = y->rate - x->rate; if (d == 0) d = y->cpu - x->cpu; break;
  case S_TIME: d = (double)y->p.cpu_us - (double)x->p.cpu_us; break;
  case S_NAME: d = strcmp(x->p.name, y->p.name); break;
  }
  if (d == 0) d = x->p.pid - y->p.pid;
  if (sort_inv) d = -d;
  return d < 0 ? -1 : d > 0 ? 1 : 0;
}

/* ---------- formatting ---------- */
static void fmt_mem(char *o, size_t cap, double b) {
  if (b >= 1024.0 * 1048576) snprintf(o, cap, "%.1fG", b / (1024.0 * 1048576));
  else if (b >= 1048576) snprintf(o, cap, "%.1fM", b / 1048576);
  else snprintf(o, cap, "%.0fK", b / 1024);
}
static void fmt_rate(char *o, size_t cap, double r) {
  if (r >= 1e6) snprintf(o, cap, "%.1fM", r / 1e6);
  else if (r >= 1e4) snprintf(o, cap, "%.1fk", r / 1e3);
  else snprintf(o, cap, "%.0f", r);
}
static void fmt_time(char *o, size_t cap, unsigned long long us) {
  unsigned long long cs = us / 10000;
  snprintf(o, cap, "%llu:%02llu.%02llu", cs / 6000, cs / 100 % 60, cs % 100);
}

/* ---------- plain snapshot ---------- */
static void snapshot(void) {
  sample();
  usleep(250000);
  sample();
  qsort(rows_, nrows, sizeof rows_[0], cmp_rows);
  char a[32], b[32];
  fmt_mem(a, sizeof a, st.used); fmt_mem(b, sizeof b, st.total);
  printf("Tasks: %d   Uptime: %02ld:%02ld:%02ld   Mem: %s/%s   ", nrows, st.uptime / 3600, st.uptime / 60 % 60, st.uptime % 60, a, b);
  fmt_mem(a, sizeof a, st.kheap); fmt_rate(b, sizeof b, total_rate);
  printf("Kernel heap: %s   Syscalls/s: %s   CPU: %.1f%%\n", a, b, total_cpu);
  printf("%6s %6s %s %6s %7s %8s %10s  %s\n", "PID", "PPID", "S", "CPU%", "MEM", "SYSC/s", "TIME+", "Command");
  for (int i = 0; i < nrows; i++) {
    struct row *r = &rows_[i];
    fmt_mem(a, sizeof a, r->p.mem); fmt_rate(b, sizeof b, r->rate);
    char t[32];
    fmt_time(t, sizeof t, r->p.cpu_us);
    printf("%6d %6d %c %6.1f %7s %8s %10s  %s\n", r->p.pid, r->p.ppid, r->p.state, r->cpu, a, b, t, r->p.name);
  }
}

/* ---------- screen, redrawn line by line: only changed lines are written ---------- */
static int R = 24, C = 80;
static char **prev_line;
static int lw; /* visible width emitted on the current line */
static unsigned cur_bg;
static void txt(const char *s, int w, int right) { ob_padded(s, w, right); lw += w; }
static void txtf(int w, int right, const char *fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  txt(buf, w, right);
}
static void raw(const char *s) { ob_puts(s); lw += u8_width(s); }
static void bg(unsigned c) { cur_bg = c; ob_bg(c); }
static size_t line_start;
static void line_begin(int r) { line_start = ob_len; ob_goto(r, 0); lw = 0; bg(th->bg); ob_fg(th->fg); }
static void line_end(int r) {
  bg(th->bg);
  while (lw < C) { ob_put(" ", 1); lw++; }
  ob_puts("\033[0m");
  size_t n = ob_len - line_start;
  if (prev_line[r] && strlen(prev_line[r]) == n && !memcmp(prev_line[r], ob + line_start, n)) { ob_len = line_start; return; }
  free(prev_line[r]);
  prev_line[r] = malloc(n + 1);
  memcpy(prev_line[r], ob + line_start, n);
  prev_line[r][n] = 0;
}

/* an htop meter: Label[|||||||      text] with up to three coloured segments */
static void meter(const char *label, int w, const double *part, const unsigned *col, int nparts, const char *text) {
  ob_puts("\033[1m"); ob_fg(th->blue); txt(label, 5, 0);
  ob_fg(th->fg); raw("[");
  ob_puts("\033[22m");
  int inner = w - 7;
  if (inner < 4) inner = 4;
  int tl = u8_width(text), filled = 0;
  char cells[512];
  int cellcol[512];
  for (int i = 0; i < inner; i++) { cells[i] = ' '; cellcol[i] = -1; }
  for (int k = 0; k < nparts; k++) {
    int n = (int)(part[k] * inner + 0.5);
    for (int i = 0; i < n && filled < inner; i++, filled++) { cells[filled] = '|'; cellcol[filled] = k; }
  }
  for (int i = 0; i < tl && i < inner; i++) { cells[inner - tl + i] = text[i]; cellcol[inner - tl + i] = cellcol[inner - tl + i] >= 0 && text[i] == ' ' ? cellcol[inner - tl + i] : -2; }
  int last = -3;
  for (int i = 0; i < inner; i++) {
    int c = cellcol[i];
    if (c != last) {
      if (c >= 0) ob_fg(col[c]); else if (c == -2) ob_fg(th->grey); else ob_fg(th->fg);
      last = c;
    }
    char ch[2] = {cells[i], 0};
    raw(ch);
  }
  ob_puts("\033[1m"); ob_fg(th->fg); raw("]"); ob_puts("\033[22m");
}

static int sel, top;           /* selected index and first visible row */
static int sel_pid = -1;
static int confirm_kill;       /* 1 while asking "kill?" */
static int help;
static char msg[128];
static double msg_time;

static unsigned level_color(double f) {
  return f < 0.25 ? th->green : f < 0.5 ? th->yellow : f < 0.75 ? th->orange : th->red;
}

static void draw(void) {
  int half = C / 2;
  char a[32], b[32], t[64];
  /* meters */
  double cpuf = total_cpu / 100.0;
  if (cpuf > 1) cpuf = 1;
  double user_mem = 0;
  for (int i = 0; i < nrows; i++) user_mem += rows_[i].p.mem;
  double tot = st.total > 0 ? st.total : 1;
  if (user_mem > st.used) user_mem = st.used;
  double kmem = st.used - user_mem;
  if (kmem < 0) kmem = 0;
  if (total_rate > peak_rate) peak_rate = total_rate;

  line_begin(0);
  raw(" ");
  { double p[1] = {cpuf}; unsigned c[1] = {level_color(cpuf)}; snprintf(t, sizeof t, "%.1f%%", total_cpu); meter("CPU", half - 2, p, c, 1, t); }
  raw("   ");
  int running = 0, zombies = 0;
  for (int i = 0; i < nrows; i++) { if (rows_[i].p.state == 'R') running++; if (rows_[i].p.state == 'Z') zombies++; }
  ob_fg(th->blue); raw("Tasks: "); ob_puts("\033[1m"); ob_fg(th->fg); snprintf(t, sizeof t, "%d", nrows); raw(t);
  ob_puts("\033[22m"); ob_fg(th->blue); raw(", "); ob_puts("\033[1m"); ob_fg(th->green); snprintf(t, sizeof t, "%d", running); raw(t);
  ob_puts("\033[22m"); ob_fg(th->blue); raw(" running");
  if (zombies) { raw(", "); ob_fg(th->red); snprintf(t, sizeof t, "%d", zombies); raw(t); ob_fg(th->blue); raw(" zombie"); }
  line_end(0);

  line_begin(1);
  raw(" ");
  { double p[2] = {user_mem / tot, kmem / tot}; unsigned c[2] = {th->green, th->orange};
    fmt_mem(a, sizeof a, st.used); fmt_mem(b, sizeof b, st.total); snprintf(t, sizeof t, "%s/%s", a, b);
    meter("Mem", half - 2, p, c, 2, t); }
  raw("   ");
  ob_fg(th->blue); raw("Uptime: "); ob_puts("\033[1m"); ob_fg(th->fg);
  snprintf(t, sizeof t, "%02ld:%02ld:%02ld", st.uptime / 3600, st.uptime / 60 % 60, st.uptime % 60); raw(t); ob_puts("\033[22m");
  line_end(1);

  line_begin(2);
  raw(" ");
  { double kh = st.kheap / tot; double p[1] = {kh}; unsigned c[1] = {th->yellow};
    fmt_mem(a, sizeof a, st.kheap); snprintf(t, sizeof t, "%s", a); meter("Heap", half - 2, p, c, 1, t); }
  raw("   ");
  ob_fg(th->blue); raw("Syscalls/s: "); ob_puts("\033[1m"); ob_fg(th->fg); fmt_rate(a, sizeof a, total_rate); raw(a); ob_puts("\033[22m");
  ob_fg(th->grey); fmt_rate(b, sizeof b, peak_rate); snprintf(t, sizeof t, "  (peak %s)", b); raw(t);
  line_end(2);

  line_begin(3);
  raw(" ");
  { double f = total_rate / peak_rate; double p[1] = {f}; unsigned c[1] = {th->aqua}; fmt_rate(a, sizeof a, total_rate); snprintf(t, sizeof t, "%s/s", a); meter("Sys", half - 2, p, c, 1, t); }
  line_end(3);

  line_begin(4); line_end(4);

  /* column header */
  static const int cw[S_N] = {7, 8, 2, 7, 8, 9, 11, 0};
  const int barw = 12;
  line_begin(5);
  bg(th->aqua); ob_fg(th->bg); ob_puts("\033[1m");
  for (int k = 0; k < S_N; k++) {
    if (k == S_NAME) { raw(" "); txtf(barw, 0, "ACTIVITY"); raw(" "); }
    if (k == sort_col) bg(th->orange);
    if (k == S_NAME) txt(sort_names[k], 7, 0);
    else if (k == S_STATE) txt(" S", 2, 0);
    else txt(sort_names[k], cw[k], 1);
    if (k == sort_col) bg(th->aqua);
  }
  while (lw < C) raw(" ");
  ob_puts("\033[22m");
  line_end(5);

  /* process rows */
  int list_top = 6, list_h = R - list_top - 1;
  if (list_h < 1) list_h = 1;
  if (sel >= nrows) sel = nrows - 1;
  if (sel < 0) sel = 0;
  if (sel < top) top = sel;
  if (sel >= top + list_h) top = sel - list_h + 1;
  if (top > nrows - list_h) top = nrows - list_h > 0 ? nrows - list_h : 0;
  double maxrate = 1;
  for (int i = 0; i < nrows; i++) if (rows_[i].rate > maxrate) maxrate = rows_[i].rate;
  for (int y = 0; y < list_h; y++) {
    int i = top + y;
    line_begin(list_top + y);
    if (i < nrows) {
      struct row *r = &rows_[i];
      int s = i == sel;
      unsigned fg = s ? th->bg : th->fg, dim = s ? th->bg : th->grey;
      if (s) bg(th->blue);
      ob_fg(fg); txtf(7, 1, "%d", r->p.pid);
      ob_fg(dim); txtf(8, 1, "%d", r->p.ppid);
      ob_fg(s ? fg : r->p.state == 'R' ? th->green : r->p.state == 'Z' ? th->red : th->grey); txtf(2, 1, "%c", r->p.state);
      ob_fg(s ? fg : r->cpu >= 50 ? th->red : r->cpu >= 10 ? th->yellow : th->fg); txtf(7, 1, "%.1f", r->cpu);
      fmt_mem(a, sizeof a, r->p.mem); ob_fg(fg); txt(a, 8, 1);
      fmt_rate(a, sizeof a, r->rate); ob_fg(s ? fg : r->rate > 0 ? th->aqua : th->grey); txt(a, 9, 1);
      fmt_time(t, sizeof t, r->p.cpu_us); ob_fg(dim); txt(t, 11, 1);
      raw(" ");
      /* activity bar in eighths */
      double f = r->rate / maxrate;
      int eighths = (int)(f * barw * 8 + 0.5);
      if (r->rate > 0 && eighths == 0) eighths = 1;
      ob_fg(s ? th->bg : level_color(f));
      if (!s) bg(th->bg1);
      static const char *blk[9] = {" ", "\xe2\x96\x8f", "\xe2\x96\x8e", "\xe2\x96\x8d", "\xe2\x96\x8c", "\xe2\x96\x8b", "\xe2\x96\x8a", "\xe2\x96\x89", "\xe2\x96\x88"};
      for (int k = 0; k < barw; k++) {
        int e = eighths - k * 8;
        raw(blk[e >= 8 ? 8 : e > 0 ? e : 0]);
      }
      bg(s ? th->blue : th->bg);
      raw(" ");
      if (!s) ob_puts("\033[1m");
      ob_fg(s ? fg : th->fg);
      txt(r->p.name, C - lw > 0 ? C - lw : 0, 0);
      ob_puts("\033[22m");
      if (s) { while (lw < C) raw(" "); }
    }
    line_end(list_top + y);
  }

  /* footer: htop's function key bar, or the kill prompt */
  line_begin(R - 1);
  if (confirm_kill && sel < nrows) {
    bg(th->red); ob_fg(th->bg); ob_puts("\033[1m");
    txtf(C, 0, " Kill %d (%s) with SIGTERM? [y/N] ", rows_[sel].p.pid, rows_[sel].p.name);
    ob_puts("\033[22m");
  } else if (msg[0]) {
    ob_fg(th->yellow); txt(msg, C, 0);
  } else {
    static const char *keys[][2] = {{"F1", "Help"}, {"F6", "SortBy"}, {"I", "Invert"}, {"F9", "Kill"}, {"F10", "Quit"}};
    for (int k = 0; k < 5 && lw < C - 10; k++) {
      bg(th->bg); ob_fg(th->fg); ob_puts("\033[1m"); raw(keys[k][0]); ob_puts("\033[22m");
      bg(th->aqua); ob_fg(th->bg); txtf(8, 0, "%s", keys[k][1]);
    }
    bg(th->bg); ob_fg(th->grey);
    snprintf(t, sizeof t, " sort: %s%s", sort_names[sort_col], sort_inv ? " (inv)" : "");
    if (lw + u8_width(t) <= C) raw(t);
  }
  line_end(R - 1);

  if (help) { /* a box over the list */
    static const char *h[] = {"htop for UC.OS: keys", "", "Up/Down      move the selection", "PgUp/PgDn    page",
                              "Home/End     first / last", "F6 > <       next / previous sort column",
                              "P M N T S    sort by CPU, MEM, PID, TIME, SYSC/s", "I            invert the order",
                              "k F9         kill the selected process (asks first)", "q F10        quit", "",
                              "any key closes this help"};
    int n = sizeof h / sizeof h[0], w = 56, x0 = (C - w) / 2, y0 = 6;
    if (x0 < 0) x0 = 0;
    for (int i = 0; i < n + 2 && y0 + i < R - 1; i++) {
      ob_goto(y0 + i, x0); ob_bg(th->bg1); ob_fg(i == 1 ? th->orange : th->fg);
      const char *s = i == 0 || i == n + 1 ? "" : h[i - 1];
      ob_puts("  "); ob_padded(s, w - 4, 0); ob_puts("  ");
      ob_puts("\033[0m");
      free(prev_line[y0 + i]); prev_line[y0 + i] = 0; /* repaint it when help closes */
    }
  }
  ob_flush();
}

static void select_pid(void) { /* keep the selection on the same process after re-sorting */
  if (sel_pid < 0) return;
  for (int i = 0; i < nrows; i++) if (rows_[i].p.pid == sel_pid) { sel = i; return; }
}
static void resort(void) {
  qsort(rows_, nrows, sizeof rows_[0], cmp_rows);
  select_pid();
}
static void cleanup(void) {
  ob_puts("\033[0m\033[?25h\033[?1049l");
  ob_flush();
  tty_restore();
}
static void full_redraw(void) {
  for (int i = 0; i < R; i++) { free(prev_line[i]); prev_line[i] = 0; }
  ob_puts("\033[0m"); ob_bg(th->bg); ob_puts("\033[2J");
}

int main(int argc, char **argv) {
  theme_init();
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-s") && i + 1 < argc) {
      for (int k = 0; k < S_N; k++) if (!strcasecmp(argv[i + 1], sort_names[k])) sort_col = k;
      i++;
    } else { fprintf(stderr, "usage: htop [-s PID|CPU%%|MEM|SYSC/s|TIME+|Command]\n"); return 2; }
  }
  if (!tty_check(0) || !tty_check(1)) { snapshot(); return 0; }
  tty_size(1, &R, &C);
  if (R < 8) R = 8;
  if (C > 500) C = 500;
  prev_line = calloc(R, sizeof *prev_line);
  tty_raw(0);
  ob_puts("\033[?1049h\033[?25l");
  full_redraw();
  sample();
  resort();
  draw();
  usleep(200000); /* a first rate measurement without waiting a whole second */
  sample();
  resort();
  draw();
  double next = now_ms() + 1000;
  for (;;) {
    int wait = (int)(next - now_ms());
    if (wait < 0) wait = 0;
    int k = key_read(wait);
    if (k == -2) break;
    if (k == -1) {
      sample(); resort();
      if (now_ms() - msg_time > 3000) msg[0] = 0;
      int r, c;
      if (tty_size(1, &r, &c) == 0 && (r != R || c != C)) {
        for (int i = 0; i < R; i++) free(prev_line[i]);
        free(prev_line);
        R = r < 8 ? 8 : r; C = c > 500 ? 500 : c;
        prev_line = calloc(R, sizeof *prev_line);
        full_redraw();
      }
      draw();
      next = now_ms() + 1000;
      continue;
    }
    int list_h = R - 7;
    if (help) { help = 0; draw(); continue; }
    if (confirm_kill) {
      confirm_kill = 0;
      if ((k == 'y' || k == 'Y') && sel < nrows) {
        int pid = rows_[sel].p.pid;
        if (info_kill(pid, 15) == 0) snprintf(msg, sizeof msg, " sent SIGTERM to %d", pid);
        else snprintf(msg, sizeof msg, " could not kill %d", pid);
        msg_time = now_ms();
        usleep(50000);
        sample(); resort();
      }
      draw();
      continue;
    }
    if (k == 'q' || k == K_F10 || k == 3) { cleanup(); return 0; }
    switch (k) {
    case K_UP: sel--; break;
    case K_DOWN: sel++; break;
    case K_PGUP: sel -= list_h; break;
    case K_PGDN: sel += list_h; break;
    case K_HOME: case 'g': sel = 0; break;
    case K_END: case 'G': sel = nrows - 1; break;
    case K_F6: case '>': case '.': sort_col = (sort_col + 1) % S_N; break;
    case '<': case ',': sort_col = (sort_col + S_N - 1) % S_N; break;
    case 'P': sort_col = S_CPU; break;
    case 'M': sort_col = S_MEM; break;
    case 'N': sort_col = S_PID; break;
    case 'T': sort_col = S_TIME; break;
    case 'S': sort_col = S_RATE; break;
    case 'I': sort_inv = !sort_inv; break;
    case 'k': case K_F9: confirm_kill = 1; break;
    case 'h': case '?': case K_F1: help = 1; break;
    case 12: full_redraw(); break; /* Ctrl-L */
    }
    if (k == 0x10) sel--; /* Ctrl-P */
    if (k == 0x0e) sel++; /* Ctrl-N */
    if (sel >= nrows) sel = nrows - 1;
    if (sel < 0) sel = 0;
    if (k == K_F6 || k == '>' || k == '<' || k == '.' || k == ',' || k == 'P' || k == 'M' || k == 'N' || k == 'T' || k == 'S' || k == 'I') {
      qsort(rows_, nrows, sizeof rows_[0], cmp_rows);
      select_pid();
    }
    if (nrows) sel_pid = rows_[sel].p.pid;
    msg[0] = 0;
    draw();
  }
  cleanup();
  return 0;
}
