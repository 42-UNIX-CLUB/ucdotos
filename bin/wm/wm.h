// SPDX-License-Identifier: GPL-2.0-or-later
/* wm: the UC.OS tiling window manager and compositor. Shared declarations. */
#ifndef WM_H
#define WM_H
#include "ui.h"
#include "uiproto.h"
#include "theme.h"
#include "gfx.h"

/* The kernel gives a process 32 descriptors and ppoll takes at most 32: console (3),
 * framebuffer, input, listener, one spare for reading files, the rest are clients. */
#define MAX_CLIENTS 24
#define MAX_WS 9
#define MAX_BINDS 96
#define MAX_DAMAGE 48

typedef struct { int x, y, w, h; } rect;

typedef struct client {
  int fd, pid, role, ws, hello;
  int floating, fullscreen, placed;
  char title[UIP_TITLE_MAX + 1], app_id[32];
  rect r;              /* outer rectangle on screen, border included */
  int bw;              /* border width used for this window */
  int cw, ch;          /* content size last sent in CONFIGURE */
  int req_w, req_h;    /* size the client asked for */
  unsigned *buf;       /* latest pixels, pw x ph (the client's own buffer size) */
  int pw, ph;
  /* receive state machine: header, then control payload or PRESENT pixels */
  unsigned char hdr[UIP_HDR];
  unsigned hdr_got;
  struct uip_hdr cur;
  unsigned char ctl[UIP_CTL_MAX + 1];
  unsigned ctl_got;
  int in_pixels;
  struct uip_present pp;
  unsigned px_got, px_total;
  double t_first;      /* when the first byte of the current PRESENT arrived (trace) */
  int traced;
  /* send queue (the socket is non-blocking; a client that stops reading loses events) */
  unsigned char *out;
  unsigned outlen, outcap;
  int dropped, close_asked;
  int wants_lock;      /* asked (pointer-lock on) for the mouse to be captured while it is fullscreen */
} client;

/* config (config.c) */
typedef struct { int mods, key; char cmd[96]; } binding;
typedef struct {
  int mod;              /* UI_ALT by default */
  int gap, border, radius, border_thin; /* -1 = take it from the theme */
  int workspace_count;
  int animation_ms;     /* length of the workspace slide; 0 = no slide */
  float master;         /* master area share, 0.1 .. 0.9 */
  int nbinds;
  binding binds[MAX_BINDS];
  char autostart[16][128];
  int nauto;
  int always[16];       /* autostart line is exec_always */
  int focus_new;
} wm_config;

/* globals (main.c) */
extern wm_config cfg;
extern ui_theme theme;
extern int scr_w, scr_h;           /* the screen size; compose.c paints it a band of rows at a time */
extern client *order[MAX_CLIENTS]; /* every client; order = tiling order, later floats on top */
extern int nclients;
extern int cur_ws;
extern client *focus_of[MAX_WS];
extern float master_of[MAX_WS];
extern int ptr_x, ptr_y, ptr_shown;
extern int G_gap, G_border, G_radius, G_thin; /* effective values */
extern int stats_frames, stats_writes;
extern long long stats_bytes;
extern double stats_compose_ms;
extern int trace;
extern int bar_h;

double now_ms(void);
void send_event(client *c, int type, int key, int mods, int repeat, unsigned cp, int x, int y, int button, int wheel);
void send_configure(client *c);
void drop_client(client *c);
int run_command(client *src, const char *cmd);
void wm_spawn(const char *cmdline);
void state_changed(void);

/* layout.c */
void relayout(void);
void set_focus(client *c);
client *focused(void);
client *client_at(int x, int y);
void client_added(client *c);
void client_removed(client *c);
int visible(client *c);
void content_rect(client *c, rect *out);
int layout_command(const char *verb, const char *arg);
unsigned occupied_mask(void);

/* compose.c */
void damage(int x, int y, int w, int h);
void damage_all(void);
void damage_client_border(client *c);
int damage_pending(void);
void compose(void);
void render_wallpaper(void);
void anim_begin(void);      /* before a workspace switch: note which workspace is on screen */
void anim_go(int dir);      /* after it: slide to the new one; dir +1 for a higher workspace, -1 for a lower */
void anim_end(void);
int fb_setup(int fd);
void cursor_moved(int ox, int oy);
double bench_compose(int n);

/* config.c */
void config_load(int first);
int parse_combo(const char *s, int *mods, int *key);
void apply_geometry(void);

#endif
