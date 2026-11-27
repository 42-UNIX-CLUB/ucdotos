// SPDX-License-Identifier: GPL-2.0-or-later
/* bar: the top bar. Workspaces and the focused window on the left, date and time in
 * the centre, live system numbers on the right. It wakes once a second (poll timeout)
 * and whenever the window manager pushes new state; otherwise it sleeps. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/syscall.h>
#include "ui.h"
#include "theme.h"
#include "gfx.h"
#include "uconf.h"

static ui_window *win;
static ui_theme th;
static ttf_font font, bold;
static unsigned char *font_data, *bold_data;
static ttf_cache *reg, *bld;
static ttf_metrics met;
static ui_wm_info info;
static char clock_str[48], stats_str[96];
static int cell_w, ws_x0;  /* workspace cells, for clicks */

/* ~/.config/bar/bar.uconf (.uconf, see libuconf/uconf.h). Read at start and on every theme
 * change (the window manager resends the size then), except the height, read at start.
 *   [bar]         height (pixels; 0 = from the font size)
 *   [workspaces]  show      [title] show
 *   [clock]       show, format (strftime; empty = the built-in "Thu 26 Nov 10:35:54")
 *   [stats]       show = mem, procs, sys   (any of them, in any order; empty for none) */
static struct { int height, workspaces, title, clock; char format[64], stats[64]; } conf;
static void load_conf(void) {
  const char *home = getenv("HOME");
  char path[256];
  snprintf(path, sizeof path, "%s/.config/bar/bar.uconf", home && *home ? home : "/home/user");
  uconf c;
  uconf_load(&c, path); /* no file: the defaults */
  conf.height = uconf_int(&c, "bar.height", 0, 0, 96);
  conf.workspaces = uconf_bool(&c, "workspaces.show", 1);
  conf.title = uconf_bool(&c, "title.show", 1);
  conf.clock = uconf_bool(&c, "clock.show", 1);
  strlcpy(conf.format, uconf_str(&c, "clock.format", ""), sizeof conf.format);
  strlcpy(conf.stats, uconf_str(&c, "stats.show", "mem, procs, sys"), sizeof conf.stats);
  uconf_warn_unused(&c);
  uconf_free(&c);
}

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void load_theme(void) {
  ui_theme_load(&th, NULL);
  if (reg) ttf_cache_free(reg);
  if (bld) ttf_cache_free(bld);
  free(font_data); free(bold_data);
  reg = bld = NULL;
  unsigned n;
  font_data = ui_read_file(th.font, &n);
  if (font_data && ttf_open(&font, font_data, n) == 0) {
    reg = ttf_cache_new(&font, th.font_size - 1, 128);
    ttf_metrics_px(&font, th.font_size - 1, &met);
  }
  bold_data = ui_read_file(th.font_bold, &n);
  if (bold_data && ttf_open(&bold, bold_data, n) == 0) bld = ttf_cache_new(&bold, th.font_size - 1, 64);
  if (!bld) bld = reg ? ttf_cache_new(&font, th.font_size - 1, 64) : NULL;
}

/* Civil date from days since 1970-01-01 (Howard Hinnant's algorithm). Times are UTC:
 * the system has no time zone database yet. */
static void format_clock(char *out, int cap) {
  static const char *wd[] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};
  static const char *mo[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  long long t = ts.tv_sec, days = t / 86400, sod = t % 86400;
  long long z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = (unsigned)(z - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
  unsigned d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
  if (conf.format[0]) { /* the user's format; times are UTC */
    time_t tt = (time_t)t;
    struct tm tm;
    gmtime_r(&tt, &tm);
    if (!strftime(out, (size_t)cap, conf.format, &tm)) out[0] = 0;
    return;
  }
  snprintf(out, cap, "%s %u %s %02d:%02d:%02d", wd[days % 7], d, mo[m - 1], (int)(sod / 3600), (int)(sod / 60 % 60), (int)(sod % 60));
}

/* Memory in use, process count, and system calls per second summed over every process. */
static void format_stats(char *out, int cap) {
  struct sample { int pid; unsigned long long n; };
  static struct sample prev[64];
  static int nprev;
  static double prev_t;
  struct sysinfo si;
  memset(&si, 0, sizeof si);
  __syscall1(SYS_sysinfo, &si);
  unsigned long used = si.totalram > si.freeram ? si.totalram - si.freeram : si.totalram;
  struct procinfo pi;
  unsigned long long delta = 0;
  int n = 0;
  struct sample cur[64];
  for (int i = 0; n < 64 && __syscall2(SYS_procinfo, i, &pi) == 0; i++) {
    unsigned long long before = 0;
    for (int j = 0; j < nprev; j++) if (prev[j].pid == pi.pid) before = prev[j].n;
    delta += pi.syscalls >= before ? pi.syscalls - before : pi.syscalls;
    cur[n].pid = pi.pid; cur[n].n = pi.syscalls; n++;
  }
  double t = now_ms(), dt = prev_t ? (t - prev_t) / 1000.0 : 0;
  double rate = dt > 0.05 ? delta / dt : 0;
  memcpy(prev, cur, sizeof cur); nprev = n; prev_t = t;
  char r[16];
  if (rate >= 1e6) snprintf(r, sizeof r, "%.1fM", rate / 1e6);
  else if (rate >= 1e3) snprintf(r, sizeof r, "%.1fk", rate / 1e3);
  else snprintf(r, sizeof r, "%d", (int)rate);
  /* the parts bar.uconf asks for, in its order */
  char want[64], *part[3];
  strlcpy(want, conf.stats, sizeof want);
  int nparts = *want ? uconf_split(want, part, 3) : 0, len = 0;
  out[0] = 0;
  for (int i = 0; i < nparts && len < cap; i++) {
    const char *sep = len ? "|" : "";
    if (!strcmp(part[i], "mem")) len += snprintf(out + len, (size_t)(cap - len), "%smem %luM", sep, used >> 20);
    else if (!strcmp(part[i], "procs")) len += snprintf(out + len, (size_t)(cap - len), "%sprocs %d", sep, si.procs);
    else if (!strcmp(part[i], "sys")) len += snprintf(out + len, (size_t)(cap - len), "%ssys %s/s", sep, r);
  }
}

/* Draw "label value" pairs separated by '|', labels dim and values bright. Returns the width. */
static int draw_stats(gfx_surface *s, int x, int base, const char *str, int measure) {
  int x0 = x;
  char buf[96];
  strlcpy(buf, str, sizeof buf);
  for (char *part = buf, *next; part; part = next) {
    next = strchr(part, '|');
    if (next) *next++ = 0;
    char *sp = strchr(part, ' ');
    if (sp) *sp = 0;
    if (measure) x += gfx_text_width(reg, part, -1) + met.advance + (sp ? gfx_text_width(bld, sp + 1, -1) : 0);
    else {
      x = gfx_text(s, reg, x, base, part, -1, th.dim) + met.advance;
      if (sp) x = gfx_text(s, bld, x, base, sp + 1, -1, th.bar_fg);
    }
    if (next) x += met.advance * 2;
  }
  return x - x0;
}

/* Paint the whole bar into the buffer; `what` says which parts to send (1 left, 2 centre, 4 right). */
static void draw(int what) {
  int w = ui_width(win), h = ui_height(win);
  if (!reg || !bld) return;
  gfx_surface s;
  gfx_init(&s, ui_pixels(win), w, h, w);
  gfx_clear(&s, th.bar_bg);
  int base = (h + met.ascent - met.descent) / 2, pad = 10;
  /* left: workspaces, current one in an accent block, occupied ones bright */
  cell_w = met.advance * 2 + 6;
  ws_x0 = pad;
  int x = ws_x0, count = !conf.workspaces ? 0 : info.workspace_count > 0 ? info.workspace_count : 4;
  if (!conf.workspaces) cell_w = 0; /* no cells to click */
  for (int i = 0; i < count; i++) {
    char num[4];
    snprintf(num, sizeof num, "%d", i + 1);
    int tw = gfx_text_width(bld, num, -1), tx = x + (cell_w - tw) / 2;
    if (i + 1 == info.workspace) {
      gfx_round_rect_fill(&s, x, 4, cell_w, h - 8, 3, th.accent);
      gfx_text(&s, bld, tx, base, num, -1, th.bar_bg);
    } else
      gfx_text(&s, (info.occupied_mask >> i) & 1 ? bld : reg, tx, base, num, -1, (info.occupied_mask >> i) & 1 ? th.bar_fg : th.dim);
    x += cell_w + 2;
  }
  int title_x = count ? x + 14 : pad;
  /* centre: date and time */
  int cw = conf.clock ? gfx_text_width(reg, clock_str, -1) : 0, cx = (w - cw) / 2;
  if (conf.clock) gfx_text(&s, reg, cx, base, clock_str, -1, th.bar_fg);
  /* right: system numbers */
  int sw = draw_stats(&s, 0, base, stats_str, 1), sx = w - pad - sw;
  draw_stats(&s, sx, base, stats_str, 0);
  /* the focused window title, cut to fit before the clock */
  int room = cx - 24 - title_x;
  if (conf.title && info.focused_title[0] && room > met.advance * 4) {
    char t[128];
    strlcpy(t, info.focused_title, sizeof t);
    int len = strlen(t);
    while (len > 0 && gfx_text_width(reg, t, len) > room) len--;
    gfx_text(&s, reg, title_x, base, t, len, th.dim);
  }
  if (what == 7) ui_present(win, 0, 0, w, h);
  else {
    if (what & 1) ui_present(win, 0, 0, cx - 8, h);
    if (what & 2) ui_present(win, cx - 8, 0, cw + 16, h);
    if (what & 4) ui_present(win, sx - 120 > cx + cw + 8 ? sx - 120 : cx + cw + 8, 0, w, h);
  }
}

int main(void) {
  load_conf();
  load_theme();
  int h = conf.height ? conf.height : th.font_size + 13;
  win = ui_open("bar", "bar", 800, h, UI_BAR);
  if (!win) { fprintf(stderr, "bar: no window manager\n"); return 1; }
  ui_wm_state(win, &info);
  format_clock(clock_str, sizeof clock_str);
  format_stats(stats_str, sizeof stats_str);
  ui_event ev;
  for (;;) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int wait = 1000 - (int)(ts.tv_nsec / 1000000) + 2; /* wake just after the next second starts */
    int r = ui_wait_event(win, &ev, wait);
    if (r < 0) break;
    if (r == 0) {
      char c[48], st[96];
      format_clock(c, sizeof c);
      format_stats(st, sizeof st);
      int what = (strcmp(c, clock_str) ? 2 : 0) | (strcmp(st, stats_str) ? 4 : 0);
      if (strlen(c) != strlen(clock_str) || strlen(st) != strlen(stats_str)) what = 7; /* widths moved: send it all */
      strcpy(clock_str, c); strcpy(stats_str, st);
      if (what) draw(what);
      continue;
    }
    switch (ev.type) {
    case UI_EV_RESIZE: /* also a theme change: bar.uconf may have changed too */
      load_conf(); load_theme();
      format_clock(clock_str, sizeof clock_str); format_stats(stats_str, sizeof stats_str);
      draw(7);
      break;
    case UI_EV_WM_STATE: ui_wm_state(win, &info); draw(1); break;
    case UI_EV_CLOSE: ui_close(win); return 0;
    case UI_EV_MOUSE_DOWN:
      if (ev.button == 0 && ev.x >= ws_x0 && cell_w > 0) {
        int i = (ev.x - ws_x0) / (cell_w + 2);
        if (i < info.workspace_count) {
          char cmd[24];
          snprintf(cmd, sizeof cmd, "workspace %d", i + 1);
          ui_wm_command(win, cmd);
        }
      }
      break;
    case UI_EV_WHEEL: { /* scroll over the bar to walk through workspaces */
      int n = info.workspace + (ev.wheel > 0 ? -1 : 1);
      if (n >= 1 && n <= info.workspace_count) {
        char cmd[24];
        snprintf(cmd, sizeof cmd, "workspace %d", n);
        ui_wm_command(win, cmd);
      }
      break;
    }
    }
  }
  return 0;
}
