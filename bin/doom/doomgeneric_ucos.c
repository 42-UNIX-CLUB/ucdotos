// SPDX-License-Identifier: GPL-2.0-or-later
/* doomgeneric_ucos.c: the UC.OS platform layer for doomgeneric.
 *
 * Two ways to reach the screen:
 *   window mode       a window from the window manager (libui), when one is running;
 *   framebuffer mode  /dev/fb0 + /dev/input/event0 directly (forced with --fb).
 *
 * The engine draws 320x200 palette indices (built with CMAP256) into
 * DG_ScreenBuffer. DG_DrawFrame converts them to R,G,B,A and scales them with
 * nearest-neighbour tables computed once per output size, centred with black
 * bars, and presents only the rows that changed since the last frame.
 *
 * Timing: DG_GetTicksMs reads CLOCK_MONOTONIC, DG_SleepMs sleeps with
 * nanosleep. The engine waits for the next 35 Hz tic by calling I_Sleep(1) in a
 * loop; DG_SleepMs stretches such a short sleep to the next tic boundary, so a
 * waiting frame costs one sleep instead of about 28.
 *
 * Extra flags (all others go to the engine):
 *   --fb / -fb      use the framebuffer even if a window manager is running
 *   -stats          print frame timing, system call and memory counters to stderr every 5 s
 *   -selftest       report the save game files to stderr when they appear or change
 * Game data still on the web server (kernel/remote.c) is fetched when the engine first
 * opens it (DG_FetchFile), with a progress bar in Doom's window. Fatal errors are shown
 * in the window too when stderr is not a terminal (DG_ErrorBox); -nogui turns that off.
 * Default arguments: if /usr/share/games/doom/doom.args exists, its words are
 * inserted before the command-line arguments. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#include "doomgeneric.h"
#include "doomkeys.h"
#include "doomtype.h"
#include "i_system.h"
#include "i_video.h"
#include "m_argv.h"
#include "ui.h"
#include "theme.h"
#include "gfx.h"

/* libui is optional at link time: these are weak references, null when the
 * library does not provide them (the CFLAGS file forces the linker to look for
 * them with -u, so a real libui is linked as soon as it exists). */
extern ui_window *ui_open(const char *, const char *, int, int, int) __attribute__((weak));
extern void ui_close(ui_window *) __attribute__((weak));
extern void ui_set_title(ui_window *, const char *) __attribute__((weak));
extern unsigned int *ui_pixels(ui_window *) __attribute__((weak));
extern int ui_width(ui_window *) __attribute__((weak));
extern int ui_height(ui_window *) __attribute__((weak));
extern void ui_present(ui_window *, int, int, int, int) __attribute__((weak));
extern int ui_poll_event(ui_window *, ui_event *) __attribute__((weak));
/* for the notices (progress, errors): the theme's font and colours */
extern int ui_theme_load(ui_theme *, const char *) __attribute__((weak));
extern unsigned char *ui_read_file(const char *, unsigned *) __attribute__((weak));
extern int ttf_open(ttf_font *, const unsigned char *, unsigned) __attribute__((weak));
extern ttf_cache *ttf_cache_new(const ttf_font *, float, int) __attribute__((weak));
extern void ttf_cache_free(ttf_cache *) __attribute__((weak));
extern void ttf_metrics_px(const ttf_font *, float, ttf_metrics *) __attribute__((weak));
extern void gfx_init(gfx_surface *, unsigned int *, int, int, int) __attribute__((weak));
extern void gfx_fill_rect(gfx_surface *, int, int, int, int, unsigned int) __attribute__((weak));
extern int gfx_text(gfx_surface *, ttf_cache *, int, int, const char *, int, unsigned int) __attribute__((weak));
extern int gfx_text_width(ttf_cache *, const char *, int) __attribute__((weak));
extern int ui_pointer_lock(ui_window *, int) __attribute__((weak));

#define SRC_W 320
#define SRC_H 200
#define DATA_DIR "/usr/share/games/doom"

/* ---------------------------------------------------------------- counters */

static struct {
  unsigned frames, presented, syscalls, sleeps, clocks, reads, writes;
  unsigned long long sleep_us, work_us, draw_us, present_us;
} st, st_last;
static int opt_stats, opt_selftest;

static long long now_us(void) {
  struct timespec ts;
  st.syscalls++; st.clocks++;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

/* ---------------------------------------------------------------- output */

static ui_window *win;          /* window mode when non-null */
static int fb_fd = -1, ev_fd = -1;
static unsigned *out;           /* the whole output surface, out_w * out_h pixels */
static int out_w, out_h;
static int dst_x, dst_y, dst_w, dst_h; /* where the game picture goes */
static int int_sx;               /* integer horizontal scale, 0 if fractional */
static unsigned short *xmap;    /* dst column -> source column */
static int row_start[SRC_H + 1]; /* source row s covers dst rows [row_start[s], row_start[s+1]) */
static unsigned pal[256];       /* palette as R,G,B,A bytes */
static unsigned char *prev;     /* the last presented 320x200 frame */
static int need_full = 1;

/* Pick the output rectangle: the largest integer scale (sx, sy) that fits,
 * with pixels between square (16:10 picture) and 1.25 times taller than wide
 * (close to the 4:3 picture of the original game). Below 320x200, fall back
 * to a fractional nearest-neighbour fit that keeps 16:10. */
static void compute_layout(void) {
  int best_sx = 0, best_sy = 0;
  for (int sx = 1; sx * SRC_W <= out_w; sx++)
    for (int sy = sx; sy * 4 <= sx * 5 && sy * SRC_H <= out_h; sy++) {
      int a = sx * sy, b = best_sx * best_sy;
      /* prefer more area; on a tie, the aspect closer to 4:3 (sy/sx = 1.2) */
      if (a > b || (a == b && abs(sy * 10 - sx * 12) * best_sx < abs(best_sy * 10 - best_sx * 12) * sx)) { best_sx = sx; best_sy = sy; }
    }
  int_sx = best_sx;
  if (best_sx) {
    dst_w = SRC_W * best_sx; dst_h = SRC_H * best_sy;
  } else {
    dst_w = out_w; dst_h = out_w * SRC_H / SRC_W;
    if (dst_h > out_h) { dst_h = out_h; dst_w = out_h * SRC_W / SRC_H; }
    if (dst_w < 1) dst_w = 1;
    if (dst_h < 1) dst_h = 1;
  }
  dst_x = (out_w - dst_w) / 2; dst_y = (out_h - dst_h) / 2;
  free(xmap);
  xmap = malloc(dst_w * sizeof *xmap);
  for (int x = 0; x < dst_w; x++) xmap[x] = x * SRC_W / dst_w;
  for (int s = 0; s <= SRC_H; s++) row_start[s] = (s * dst_h + SRC_H - 1) / SRC_H;
  memset(out, 0, (size_t)out_w * out_h * 4);
  need_full = 1;
}

static void present_rows(int y0, int y1) { /* output rows [y0, y1), full width */
  if (y1 <= y0) return;
  long long t0 = opt_stats ? now_us() : 0;
  st.presented++;
  if (win) {
    ui_present(win, 0, y0, out_w, y1 - y0);
  } else {
    st.syscalls++; st.writes++;
    __syscall4(SYS_pwrite64, fb_fd, (long)(out + (size_t)y0 * out_w), (long)((size_t)(y1 - y0) * out_w * 4), (long)((size_t)y0 * out_w * 4));
  }
  if (opt_stats) st.present_us += now_us() - t0;
}

static void fb_set_size(int w, int h) {
  out = realloc(out, (size_t)w * h * 4);
  out_w = w; out_h = h;
  compute_layout();
}

static void win_set_size(void) {
  out = ui_pixels(win);
  out_w = ui_width(win); out_h = ui_height(win);
  compute_layout();
}

/* ---------------------------------------------------------------- input */

/* Linux key code -> Doom key, and the character the key types (for cheats and
 * save names, which read the typed character rather than the Doom key: that
 * is how W can move forward and still type a 'w'). */
static const unsigned char keymap[128][2] = {
  [1] = {KEY_ESCAPE, 0},
  [2] = {'1', '1'}, [3] = {'2', '2'}, [4] = {'3', '3'}, [5] = {'4', '4'}, [6] = {'5', '5'},
  [7] = {'6', '6'}, [8] = {'7', '7'}, [9] = {'8', '8'}, [10] = {'9', '9'}, [11] = {'0', '0'},
  [12] = {KEY_MINUS, '-'}, [13] = {KEY_EQUALS, '='}, [14] = {KEY_BACKSPACE, 0}, [15] = {KEY_TAB, 0},
  [16] = {'q', 'q'}, [17] = {KEY_UPARROW, 'w'}, [18] = {'e', 'e'}, [19] = {'r', 'r'}, [20] = {'t', 't'},
  [21] = {'y', 'y'}, [22] = {'u', 'u'}, [23] = {'i', 'i'}, [24] = {'o', 'o'}, [25] = {'p', 'p'},
  [26] = {'[', '['}, [27] = {']', ']'}, [28] = {KEY_ENTER, 0}, [29] = {KEY_FIRE, 0},
  [30] = {KEY_LEFTARROW, 'a'}, [31] = {KEY_DOWNARROW, 's'}, [32] = {KEY_RIGHTARROW, 'd'},
  [33] = {'f', 'f'}, [34] = {'g', 'g'}, [35] = {'h', 'h'}, [36] = {'j', 'j'}, [37] = {'k', 'k'},
  [38] = {'l', 'l'}, [39] = {';', ';'}, [40] = {'\'', '\''}, [41] = {'`', '`'}, [42] = {KEY_RSHIFT, 0},
  [43] = {'\\', '\\'}, [44] = {'z', 'z'}, [45] = {'x', 'x'}, [46] = {'c', 'c'}, [47] = {'v', 'v'},
  [48] = {'b', 'b'}, [49] = {'n', 'n'}, [50] = {'m', 'm'}, [51] = {',', ','}, [52] = {'.', '.'},
  [53] = {'/', '/'}, [54] = {KEY_RSHIFT, 0}, [55] = {KEYP_MULTIPLY, '*'}, [56] = {KEY_RALT, 0},
  [57] = {KEY_USE, ' '}, [58] = {KEY_CAPSLOCK, 0},
  [59] = {KEY_F1, 0}, [60] = {KEY_F2, 0}, [61] = {KEY_F3, 0}, [62] = {KEY_F4, 0}, [63] = {KEY_F5, 0},
  [64] = {KEY_F6, 0}, [65] = {KEY_F7, 0}, [66] = {KEY_F8, 0}, [67] = {KEY_F9, 0}, [68] = {KEY_F10, 0},
  [69] = {KEY_NUMLOCK, 0}, [70] = {KEY_SCRLCK, 0},
  [71] = {KEYP_7, 0}, [72] = {KEYP_8, 0}, [73] = {KEYP_9, 0}, [74] = {KEYP_MINUS, '-'},
  [75] = {KEYP_4, 0}, [76] = {KEYP_5, '5'}, [77] = {KEYP_6, 0}, [78] = {KEYP_PLUS, '+'},
  [79] = {KEYP_1, 0}, [80] = {KEYP_2, 0}, [81] = {KEYP_3, 0}, [82] = {'0', '0'},
  [87] = {KEY_F11, 0}, [88] = {KEY_F12, 0}, [96] = {KEY_ENTER, 0}, [97] = {KEY_FIRE, 0},
  [98] = {KEYP_DIVIDE, '/'}, [100] = {KEY_RALT, 0},
  [102] = {KEY_HOME, 0}, [103] = {KEY_UPARROW, 0}, [104] = {KEY_PGUP, 0}, [105] = {KEY_LEFTARROW, 0},
  [106] = {KEY_RIGHTARROW, 0}, [107] = {KEY_END, 0}, [108] = {KEY_DOWNARROW, 0}, [109] = {KEY_PGDN, 0},
  [110] = {KEY_INS, 0}, [111] = {KEY_DEL, 0}, [119] = {KEY_PAUSE, 0},
};
#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111

/* Set before DG_GetKey returns a key: i_input.c uses it as the typed character. */
unsigned char DG_KeyChar;

static struct { unsigned char pressed, key, ch; } kq[256];
static unsigned kq_head, kq_len;
static unsigned char held[128]; /* Linux code -> currently down (1) */
static unsigned char btn_held[2];

static void kq_push(int pressed, int key, int ch) {
  if (!key || kq_len == 256) return;
  unsigned i = (kq_head + kq_len++) & 255;
  kq[i].pressed = pressed; kq[i].key = key; kq[i].ch = ch;
}

static void linux_key(int code, int value) { /* value: 0 release, 1 press, 2 repeat */
  if (code == BTN_LEFT || code == BTN_RIGHT) {
    int b = code - BTN_LEFT, k = b ? KEY_USE : KEY_FIRE;
    if (value == 1 && !btn_held[b]) { btn_held[b] = 1; kq_push(1, k, 0); }
    else if (value == 0 && btn_held[b]) { btn_held[b] = 0; kq_push(0, k, 0); }
    return;
  }
  if (code < 0 || code >= 128 || !keymap[code][0]) return;
  if (value == 0) {
    if (!held[code]) return;
    held[code] = 0;
    kq_push(0, keymap[code][0], keymap[code][1]);
  } else {
    if (value == 2 && !held[code]) return;
    held[code] = 1;
    kq_push(1, keymap[code][0], keymap[code][1]);
  }
}

static void release_all(void) { /* focus lost: nothing stays pressed */
  for (int c = 0; c < 128; c++) if (held[c]) linux_key(c, 0);
  if (btn_held[0]) linux_key(BTN_LEFT, 0);
  if (btn_held[1]) linux_key(BTN_RIGHT, 0);
}

struct input_event { long long sec, usec; unsigned short type, code; int value; };

static void pump_fb(void) {
  struct input_event e[64];
  for (;;) {
    st.syscalls++; st.reads++;
    long n = read(ev_fd, e, sizeof e);
    if (n <= 0) return;
    for (long i = 0; i < n / (long)sizeof e[0]; i++) {
      if (e[i].type == 1) linux_key(e[i].code, e[i].value);
      else if (e[i].type == 4 && e[i].code == 0x11) fb_set_size((unsigned)e[i].value >> 16, e[i].value & 0xffff);
    }
    if (n < (long)sizeof e) return;
  }
}

static void pump_win(void) {
  ui_event e;
  while (ui_poll_event(win, &e) > 0) {
    switch (e.type) {
    case UI_EV_KEY_DOWN: linux_key(e.key, e.repeat ? 2 : 1); break;
    case UI_EV_KEY_UP: linux_key(e.key, 0); break;
    case UI_EV_MOUSE_DOWN: if (e.button == 0) linux_key(BTN_LEFT, 1); else if (e.button == 2) linux_key(BTN_RIGHT, 1); break;
    case UI_EV_MOUSE_UP: if (e.button == 0) linux_key(BTN_LEFT, 0); else if (e.button == 2) linux_key(BTN_RIGHT, 0); break;
    case UI_EV_MOUSE_REL: break;               /* captured mouse: buttons only, it does not turn the view */
    case UI_EV_RESIZE: win_set_size(); break;
    case UI_EV_BLUR: release_all(); break;
    case UI_EV_CLOSE: I_Quit(); break;
    default: break;
    }
  }
}

int DG_GetKey(int *pressed, unsigned char *key) {
  static int drained;
  if (!kq_len && !drained) {
    if (win) pump_win(); else pump_fb();
    drained = 1;
  }
  if (!kq_len) { drained = 0; return 0; }
  *pressed = kq[kq_head].pressed;
  *key = kq[kq_head].key;
  DG_KeyChar = kq[kq_head].ch;
  kq_head = (kq_head + 1) & 255; kq_len--;
  return 1;
}

/* ---------------------------------------------------------------- frames */

void DG_DrawFrame(void) {
  long long t0 = opt_stats ? now_us() : 0; /* timing only with -stats */
  const unsigned char *src = (const unsigned char *)DG_ScreenBuffer;
  st.frames++;
  if (palette_changed) {
    for (int i = 0; i < 256; i++) pal[i] = colors[i].r | colors[i].g << 8 | colors[i].b << 16 | 0xffu << 24;
    palette_changed = false;
    need_full = 1;
  }
  int y0 = 0, y1 = SRC_H - 1;
  if (need_full) {
    memcpy(prev, src, SRC_W * SRC_H);
  } else { /* only the source rows that changed */
    while (y0 < SRC_H && !memcmp(src + y0 * SRC_W, prev + y0 * SRC_W, SRC_W)) y0++;
    if (y0 == SRC_H) { if (opt_stats) st.draw_us += now_us() - t0; return; } /* nothing changed: no present */
    while (!memcmp(src + y1 * SRC_W, prev + y1 * SRC_W, SRC_W)) y1--;
    memcpy(prev + y0 * SRC_W, src + y0 * SRC_W, (y1 - y0 + 1) * SRC_W);
  }
  for (int s = y0; s <= y1; s++) {
    int r0 = row_start[s], r1 = row_start[s + 1];
    if (r0 == r1) continue;
    const unsigned char *in = src + s * SRC_W;
    unsigned *d = out + (size_t)(dst_y + r0) * out_w + dst_x;
    switch (int_sx) { /* replicate each source pixel; the table is for fractional scales */
    case 1: for (int x = 0; x < SRC_W; x++) d[x] = pal[in[x]]; break;
    case 2: for (int x = 0; x < SRC_W; x++) { unsigned c = pal[in[x]]; d[2 * x] = c; d[2 * x + 1] = c; } break;
    case 3: for (int x = 0; x < SRC_W; x++) { unsigned c = pal[in[x]]; unsigned *p = d + 3 * x; p[0] = c; p[1] = c; p[2] = c; } break;
    case 4: for (int x = 0; x < SRC_W; x++) { unsigned c = pal[in[x]]; unsigned *p = d + 4 * x; p[0] = c; p[1] = c; p[2] = c; p[3] = c; } break;
    default: for (int x = 0; x < dst_w; x++) d[x] = pal[in[xmap[x]]]; break;
    }
    for (int r = r0 + 1; r < r1; r++) memcpy(out + (size_t)(dst_y + r) * out_w + dst_x, d, dst_w * 4);
  }
  if (need_full) present_rows(0, out_h);
  else present_rows(dst_y + row_start[y0], dst_y + row_start[y1 + 1]);
  need_full = 0;
  if (opt_stats) st.draw_us += now_us() - t0;
}

/* ---------------------------------------------------------------- time */

static long long t_start;
static uint32_t first_tick;
static int have_first_tick;

uint32_t DG_GetTicksMs(void) {
  /* never 0: i_timer.c treats a base time of 0 as "not read yet" */
  uint32_t t = (uint32_t)((now_us() - t_start) / 1000) + 1;
  if (!have_first_tick) { first_tick = t; have_first_tick = 1; }
  return t;
}

void DG_SleepMs(uint32_t ms) {
  long long t0 = now_us();
  if (have_first_tick) { /* i_timer.c counts tics from the first tick value it read */
    uint32_t e = (uint32_t)((t0 - t_start) / 1000) + 1 - first_tick;
    uint32_t next = ((e * 35 / 1000 + 1) * 1000 + 34) / 35;
    if (next - e > ms) ms = next - e;
  }
  struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
  st.syscalls++;
  st.sleeps++;
  nanosleep(&ts, 0);
  if (opt_stats) st.sleep_us += now_us() - t0;
}

void DG_SetWindowTitle(const char *title) {
  if (win && ui_set_title) ui_set_title(win, title);
}

/* ---------------------------------------------------------------- setup */

void DG_Init(void) { /* also called early, by DG_FetchFile or DG_ErrorBox: once is enough */
  static int done;
  if (done) return;
  done = 1;
  int force_fb = M_CheckParm("--fb") || M_CheckParm("-fb");
  if (!force_fb && ui_open && ui_pixels && ui_width && ui_height && ui_present && ui_poll_event)
    /* -fullscreen (the launcher entries pass it): the window covers the screen from the start;
     * the window manager's fullscreen key (Mod+f) puts it back among the tiles. */
    win = ui_open("Doom", "doom", 960, 600, M_CheckParm("-fullscreen") ? UI_FULLSCREEN : UI_NORMAL);
  if (win) {
    win_set_size();
    if (ui_pointer_lock) ui_pointer_lock(win, 1);   /* fullscreen (Alt+f): hide the cursor, keep the buttons in Doom */
  } else {
    unsigned info[40] = {0};
    fb_fd = open("/dev/fb0", O_RDWR);
    ev_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
    if (fb_fd < 0 || ev_fd < 0 || __syscall3(SYS_ioctl, fb_fd, 0x4600, (long)info) < 0) {
      fprintf(stderr, "doom: no window manager and no framebuffer\n");
      exit(1);
    }
    fb_set_size(info[0], info[1]);
  }
  prev = calloc(SRC_W * SRC_H, 1);
}

/* ---------------------------------------------------------------- notices */

/* Before the game picture exists: lines of text in the middle of the window, and
 * optionally a progress bar. Drawn with the theme's font and colours when libui and
 * libgfx provide them; without a font only the bar shows. */
static ui_theme nth;
static ttf_font nfont;
static ttf_cache *nfc;
static ttf_metrics nmet;
static float nfc_px; /* the size nfc was made for */
static int note_all = 1; /* the whole window must be redrawn and sent */

static void note_init(void) {
  static int done;
  if (done) return;
  done = 1;
  nth.bg = GFX_HEX(0x282828); nth.fg = GFX_HEX(0xebdbb2); nth.accent = GFX_HEX(0xfe8019); nth.dim = GFX_HEX(0x928374);
  nth.panel = GFX_HEX(0x3c3836); nth.font_size = 16;
  if (ui_theme_load) ui_theme_load(&nth, NULL);
  if (!ui_read_file || !ttf_open || !ttf_cache_new || !ttf_metrics_px || !gfx_text || !gfx_text_width || !gfx_init || !gfx_fill_rect) return;
  unsigned n;
  unsigned char *d = ui_read_file(nth.font, &n);
  float px = nth.font_size > 0 ? nth.font_size * 1.25f : 20;
  if (d && ttf_open(&nfont, d, n) == 0 && (nfc = ttf_cache_new(&nfont, px, 128))) { ttf_metrics_px(&nfont, px, &nmet); nfc_px = px; }
}

/* Make the font smaller when the widest line does not fit the window (a tiled window can be narrow). */
static void note_fit(const char *text) {
  if (!nfc) return;
  int widest = 0;
  for (const char *line = text; line;) {
    const char *nl = strchr(line, '\n');
    int len = nl ? (int)(nl - line) : (int)strlen(line), w = gfx_text_width(nfc, line, len);
    if (w > widest) widest = w;
    line = nl ? nl + 1 : NULL;
  }
  int room = out_w * 9 / 10;
  if (widest <= room || nfc_px <= 8) return;
  float px = nfc_px * room / widest;
  if (px < 8) px = 8;
  ttf_cache *c = ttf_cache_new(&nfont, px, 128);
  if (!c) return;
  if (ttf_cache_free) ttf_cache_free(nfc);
  nfc = c; nfc_px = px;
  ttf_metrics_px(&nfont, px, &nmet);
}

/* text: lines separated by \n; a line "%bar" is the progress bar (permille 0..1000). */
static void note_draw(const char *text, int permille) {
  if (!out || !gfx_init) return;
  if (note_all) note_fit(text);
  gfx_surface sf;
  gfx_init(&sf, out, out_w, out_h, out_w);
  int lh = nfc ? nmet.line_height * 3 / 2 : 0, bar_h = out_h / 120 < 4 ? 4 : out_h / 120;
  int bar_w = out_w * 6 / 10 > 640 ? 640 : out_w * 6 / 10;
  int lines = 1;
  for (const char *c = text; *c; c++) lines += *c == '\n';
  int total = lines * (lh ? lh : bar_h * 3), y = (out_h - total) / 2;
  if (y < 0) y = 0;
  int y0 = y;
  if (note_all) gfx_fill_rect(&sf, 0, 0, out_w, out_h, nth.bg);
  else gfx_fill_rect(&sf, 0, y0, out_w, total, nth.bg);
  for (const char *line = text; line;) {
    const char *nl = strchr(line, '\n');
    int len = nl ? (int)(nl - line) : (int)strlen(line);
    int step = lh ? lh : bar_h * 3;
    if (len == 4 && !memcmp(line, "%bar", 4)) {
      int bx = (out_w - bar_w) / 2, by = y + (step - bar_h) / 2;
      gfx_fill_rect(&sf, bx, by, bar_w, bar_h, nth.panel);
      gfx_fill_rect(&sf, bx, by, (int)((long long)bar_w * permille / 1000), bar_h, nth.accent);
    } else if (nfc && len) {
      gfx_text(&sf, nfc, (out_w - gfx_text_width(nfc, line, len)) / 2, y + (step + nmet.ascent - nmet.descent) / 2, line, len, nth.fg);
    }
    y += step;
    line = nl ? nl + 1 : NULL;
  }
  if (note_all) present_rows(0, out_h);
  else present_rows(y0, y0 + total > out_h ? out_h : y0 + total);
  note_all = 0;
}

/* Events while a notice shows: 1 key or click, 2 the window was closed, 0 nothing. */
static int note_events(void) {
  int hit = 0;
  if (win) {
    ui_event e;
    while (ui_poll_event(win, &e) > 0) {
      if (e.type == UI_EV_RESIZE) { win_set_size(); note_all = 1; }
      else if ((e.type == UI_EV_KEY_DOWN && !e.repeat) || e.type == UI_EV_MOUSE_DOWN) { if (!hit) hit = 1; }
      else if (e.type == UI_EV_CLOSE) hit = 2;
    }
  } else if (ev_fd >= 0) {
    struct input_event e[32];
    long n;
    while ((n = read(ev_fd, e, sizeof e)) > 0)
      for (long i = 0; i < n / (long)sizeof e[0]; i++) {
        if (e[i].type == 1 && e[i].value == 1 && !hit) hit = 1;
        else if (e[i].type == 4 && e[i].code == 0x11) { fb_set_size((unsigned)e[i].value >> 16, e[i].value & 0xffff); note_all = 1; }
      }
  }
  return hit;
}

static void note_sleep(void) { struct timespec ts = {0, 50 * 1000000L}; nanosleep(&ts, 0); }

/* Called by W_StdC_OpenFile before it opens a WAD. A remote file that has not arrived yet
 * (st_blocks short of st_size; see kernel/remote.c) is fetched here with a non-blocking
 * open, which starts the transfer and says EAGAIN until it is complete, so the window can
 * show how far it got. Anything else returns at once. */
void DG_FetchFile(const char *path) {
  struct stat s;
  if (stat(path, &s) < 0 || !S_ISREG(s.st_mode) || s.st_size <= 0 || (long long)s.st_blocks * 512 >= s.st_size) return;
  const char *name = strrchr(path, '/');
  name = name ? name + 1 : path;
  long long t0 = now_us();
  double mb = s.st_size / 1048576.0;
  fprintf(stderr, "doom: fetching %s (%.1f MB)\n", name, mb);
  DG_Init();
  note_init();
  note_all = 1;
  for (;;) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd >= 0) { close(fd); break; }
    if (errno != EAGAIN) {
      fprintf(stderr, "doom: %s: %s\n", path, strerror(errno));
      I_Error("Could not fetch the game data (%s).\nCheck the connection, then start Doom again.", name);
    }
    long long got = stat(path, &s) == 0 ? (long long)s.st_blocks * 512 : 0;
    char text[256];
    snprintf(text, sizeof text, "Fetching game data: %s\n%%bar\n%.1f of %.1f MB", name, got / 1048576.0, mb);
    note_draw(text, (int)(got * 1000 / (s.st_size ? s.st_size : 1)));
    if (note_events() == 2) exit(0);
    note_sleep();
  }
  fprintf(stderr, "doom: %s arrived in %.1f s\n", name, (now_us() - t0) / 1e6);
  if (out) memset(out, 0, (size_t)out_w * out_h * 4); /* black bars around the game picture */
  need_full = 1;
}

/* Called by I_Error. Someone who started Doom from the launcher cannot see stderr, so
 * the message is shown in the window until a key, a click, closing it, or one minute. */
void DG_ErrorBox(const char *msg) {
  if (!win && fb_fd < 0 && isatty(2)) return; /* a terminal already shows it */
  DG_Init();
  note_init();
  note_all = 1;
  char text[640];
  int n = snprintf(text, sizeof text, "%s", msg);
  while (n > 0 && text[n - 1] == '\n') text[--n] = 0;
  snprintf(text + n, sizeof text - n, "\n\nPress any key to close.");
  long long until = now_us() + 60000000LL;
  note_draw(text, 0);
  while (now_us() < until) {
    if (note_events()) break;
    if (note_all) note_draw(text, 0);
    note_sleep();
  }
}

/* Saves and the config live in $HOME/.doom/ (m_config.c asks for this). */
char *DG_ConfigDir(void) {
  const char *home = getenv("HOME");
  if (!home || !*home) home = "/home/user";
  mkdir(home, 0755);
  size_t n = strlen(home) + 8;
  char *dir = malloc(n);
  snprintf(dir, n, "%s/.doom/", home);
  return dir;
}

/* Words from DATA_DIR/doom.args go between argv[0] and the real arguments. */
static char **default_args(int *argc, char **argv) {
  int fd = open(DATA_DIR "/doom.args", O_RDONLY);
  if (fd < 0) return argv;
  static char buf[1024];
  long n = read(fd, buf, sizeof buf - 1);
  close(fd);
  if (n <= 0) return argv;
  buf[n] = 0;
  char **v = malloc((*argc + n / 2 + 2) * sizeof *v);
  int c = 0;
  v[c++] = argv[0];
  for (char *p = strtok(buf, " \t\r\n"); p; p = strtok(0, " \t\r\n")) v[c++] = p;
  for (int i = 1; i < *argc; i++) v[c++] = argv[i];
  v[c] = 0;
  *argc = c;
  return v;
}

/* -selftest: say when a save game slot file appears or changes. */
extern char *savegamedir;
static void selftest_poll(void) {
  static long long sizes[6], mtimes[6];
  if (!savegamedir) return;
  for (int i = 0; i < 6; i++) {
    char path[256];
    struct stat s;
    snprintf(path, sizeof path, "%sdoomsav%d.dsg", savegamedir, i);
    if (stat(path, &s) < 0) continue;
    if (s.st_size != sizes[i] || s.st_mtime != mtimes[i]) {
      sizes[i] = s.st_size; mtimes[i] = s.st_mtime;
      fprintf(stderr, "selftest: save slot %d: %s, %lld bytes\n", i, path, (long long)s.st_size);
    }
  }
}

static unsigned long long kernel_syscalls(void) { /* this process's count, from the kernel */
  struct procinfo pi;
  int me = getpid();
  for (int i = 0; __syscall2(SYS_procinfo, i, (long)&pi) == 0; i++)
    if (pi.pid == me) return pi.syscalls;
  return 0;
}

static unsigned long long ksys_last;
static void print_stats(long long dt_us) {
  unsigned long long ksys = kernel_syscalls();
  unsigned f = st.frames - st_last.frames;
  if (!f) f = 1;
  double secs = dt_us / 1e6;
  fprintf(stderr,
          "doom stats: %.1f fps, engine %.2f ms/frame (scale+present %.2f ms, of which present %.2f ms), "
          "sleeps %.1f/s %.0f ms/s, presents %u, syscalls/frame %.1f (platform %.1f: clock %.1f sleep %.1f read %.1f write %.1f), "
          "mem %u pages (%u KiB)\n",
          (st.frames - st_last.frames) / secs, (st.work_us - st_last.work_us) / 1000.0 / f,
          (st.draw_us - st_last.draw_us) / 1000.0 / f, (st.present_us - st_last.present_us) / 1000.0 / f, (st.sleeps - st_last.sleeps) / secs,
          (st.sleep_us - st_last.sleep_us) / 1000.0 / secs, st.presented - st_last.presented,
          (double)(ksys - ksys_last) / f, (double)(st.syscalls - st_last.syscalls) / f,
          (double)(st.clocks - st_last.clocks) / f, (double)(st.sleeps - st_last.sleeps) / f,
          (double)(st.reads - st_last.reads) / f, (double)(st.writes - st_last.writes) / f,
          (unsigned)__builtin_wasm_memory_size(0), (unsigned)__builtin_wasm_memory_size(0) * 64);
  ksys_last = ksys;
  st_last = st;
}

static void stats_at_exit(void) { /* totals over the whole run */
  memset(&st_last, 0, sizeof st_last);
  ksys_last = 0;
  fprintf(stderr, "doom stats for the whole run:\n");
  print_stats(now_us() - t_start);
}

int main(int argc, char **argv) {
  t_start = now_us();
  argv = default_args(&argc, argv);
  myargc = argc; myargv = argv; /* so DG_Init can read our flags */
  opt_stats = M_CheckParm("-stats") > 0;
  opt_selftest = M_CheckParm("-selftest") > 0;
  if (opt_stats) atexit(stats_at_exit);
  doomgeneric_Create(argc, argv);
  long long report_at = now_us() + 5000000, last_report = now_us();
  for (unsigned n = 0;; n++) {
    if (!opt_stats) {
      doomgeneric_Tick();
    } else { /* engine time = the tick minus the time it slept */
      long long t0 = now_us();
      unsigned long long slept = st.sleep_us;
      doomgeneric_Tick();
      long long t1 = now_us();
      st.work_us += (t1 - t0) - (st.sleep_us - slept);
      if (t1 >= report_at) { print_stats(t1 - last_report); last_report = t1; report_at = t1 + 5000000; }
    }
    if (opt_selftest && n % 16 == 0) selftest_poll();
  }
}
