// SPDX-License-Identifier: GPL-2.0-or-later
/* bar: the top bar. Workspaces and the focused window on the left, date and time in
 * the centre, live system numbers on the right. It wakes once a second (poll timeout)
 * and whenever the window manager pushes new state; otherwise it sleeps.
 *
 * With a persistent store (browser storage), the right side also shows "disk N%": how full
 * the browser's quota for this site is (/proc/ucos-store, the same figures as statfs and
 * df). From 80% the number turns yellow, from 95% (or when the browser refused data) red,
 * and each time the level goes up, a notice takes the title's place for ten seconds. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include "ui.h"
#include "theme.h"
#include "gfx.h"

static ui_window *win;
static ui_theme th;
static ttf_font font, bold;
static unsigned char *font_data, *bold_data;
static ttf_cache *reg, *bld;
static ttf_metrics met;
static ui_wm_info info;
static char clock_str[48], stats_str[96];
static int cell_w, ws_x0;  /* workspace cells, for clicks */
static char notice[96];    /* storage warning shown in place of the title until notice_until */
static double notice_until;
static int disk_level;     /* 0 ok, 1 warning, 2 critical, 3 full: the last level seen */

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
  snprintf(out, cap, "%s %u %s %02d:%02d:%02d", wd[days % 7], d, mo[m - 1], (int)(sod / 3600), (int)(sod / 60 % 60), (int)(sod % 60));
}

/* Browser storage from /proc/ucos-store: "" when no store is attached. A value that starts
 * with '!' is drawn in the warning colour, "!!" in the alarm colour (draw_stats). */
static void format_disk(char *out, int cap) {
  char t[1024], state[32] = "", lev[16] = "";
  double pct = 0;
  out[0] = 0;
  int fd = open("/proc/ucos-store", O_RDONLY);
  if (fd < 0) return;
  long n = read(fd, t, sizeof t - 1);
  close(fd);
  if (n <= 0) return;
  t[n] = 0;
  for (char *l = t; l && *l; l = strchr(l, '\n'), l = l ? l + 1 : NULL) {
    if (!strncmp(l, "state: ", 7)) sscanf(l + 7, "%31s", state);
    else if (!strncmp(l, "used_percent: ", 14)) pct = strtod(l + 14, NULL);
    else if (!strncmp(l, "level: ", 7)) sscanf(l + 7, "%15s", lev);
  }
  if (!strcmp(state, "off") || !state[0]) return;
  int level = !strcmp(lev, "full") ? 3 : !strcmp(lev, "critical") ? 2 : !strcmp(lev, "warning") ? 1 : 0;
  if (level > disk_level) { /* once per crossing: the level must fall back (the kernel adds a margin) before it warns again */
    if (level == 3) snprintf(notice, sizeof notice, "browser storage is full: delete files to make room");
    else snprintf(notice, sizeof notice, "browser storage is %.0f%% full", pct);
    notice_until = now_ms() + 10000;
  }
  disk_level = level;
  snprintf(out, cap, "|disk %s%.0f%%", level >= 2 ? "!!" : level == 1 ? "!" : "", pct);
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
  char disk[32];
  format_disk(disk, sizeof disk);
  snprintf(out, cap, "mem %luM|procs %d|sys %s/s%s", used >> 20, si.procs, r, disk);
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
    const char *val = sp ? sp + 1 : NULL;
    unsigned vc = th.bar_fg;
    if (val && val[0] == '!') { val++; vc = th.color[3]; if (val[0] == '!') { val++; vc = th.color[1]; } }
    if (measure) x += gfx_text_width(reg, part, -1) + met.advance + (val ? gfx_text_width(bld, val, -1) : 0);
    else {
      x = gfx_text(s, reg, x, base, part, -1, th.dim) + met.advance;
      if (val) x = gfx_text(s, bld, x, base, val, -1, vc);
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
  int x = ws_x0, count = info.workspace_count > 0 ? info.workspace_count : 4;
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
  int title_x = x + 14;
  /* centre: date and time */
  int cw = gfx_text_width(reg, clock_str, -1), cx = (w - cw) / 2;
  gfx_text(&s, reg, cx, base, clock_str, -1, th.bar_fg);
  /* right: system numbers */
  int sw = draw_stats(&s, 0, base, stats_str, 1), sx = w - pad - sw;
  draw_stats(&s, sx, base, stats_str, 0);
  /* the focused window title, cut to fit before the clock */
  int room = cx - 24 - title_x;
  int show_notice = notice[0] && now_ms() < notice_until;
  if ((show_notice || info.focused_title[0]) && room > met.advance * 4) {
    char t[128];
    strlcpy(t, show_notice ? notice : info.focused_title, sizeof t);
    int len = strlen(t);
    while (len > 0 && gfx_text_width(reg, t, len) > room) len--;
    gfx_text(&s, reg, title_x, base, t, len, show_notice ? (disk_level >= 2 ? th.color[1] : th.color[3]) : th.dim);
  }
  if (what == 7) ui_present(win, 0, 0, w, h);
  else {
    if (what & 1) ui_present(win, 0, 0, cx - 8, h);
    if (what & 2) ui_present(win, cx - 8, 0, cw + 16, h);
    if (what & 4) ui_present(win, sx - 120 > cx + cw + 8 ? sx - 120 : cx + cw + 8, 0, w, h);
  }
}

int main(void) {
  load_theme();
  int h = th.font_size + 13;
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
      static int noticed;
      int now_notice = notice[0] && now_ms() < notice_until;
      int what = (strcmp(c, clock_str) ? 2 : 0) | (strcmp(st, stats_str) ? 4 : 0) | (now_notice != noticed ? 1 : 0);
      noticed = now_notice;
      strcpy(clock_str, c); strcpy(stats_str, st);
      if (what) draw(what);
      continue;
    }
    switch (ev.type) {
    case UI_EV_RESIZE: load_theme(); draw(7); break;
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
