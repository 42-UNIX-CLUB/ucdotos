// SPDX-License-Identifier: GPL-2.0-or-later
/* libui: how a program gets a window.
 *
 * The window manager is an ordinary program that listens on a UNIX socket
 * (/run/wm.sock, or $WM_SOCKET). A client connects, asks for a window, draws
 * into its own pixel buffer and tells the manager which part changed; the
 * manager sends back input and resize events. This header is the client side.
 * The wire format is private to libui/ui.c and the manager. */
#ifndef UI_H
#define UI_H

typedef struct ui_window ui_window;

/* window roles */
#define UI_NORMAL 0   /* tiled (or floated by the user) */
#define UI_FLOATING 1 /* centred popup above the tiles: launchers, dialogs */
#define UI_BAR 2      /* a strip docked at the top of the screen, full width, height = h */
#define UI_FULLSCREEN 4
#define UI_FLOATING_BOTTOM 8 /* a floating popup centred at the bottom of the screen: the theme picker */

/* modifier bits in ui_event.mods */
#define UI_SHIFT 1
#define UI_CTRL 2
#define UI_ALT 4
#define UI_SUPER 8

enum {
  UI_EV_NONE,
  UI_EV_KEY_DOWN,   /* key = Linux key code (KEY_A = 30 ...), mods, repeat */
  UI_EV_KEY_UP,
  UI_EV_TEXT,       /* codepoint = the Unicode character a key press typed (layout already applied by the host) */
  UI_EV_MOUSE_MOVE, /* x, y in window pixels */
  UI_EV_MOUSE_DOWN, /* button: 0 left, 1 middle, 2 right */
  UI_EV_MOUSE_UP,
  UI_EV_WHEEL,      /* wheel: +1 up, -1 down */
  UI_EV_RESIZE,     /* w, h: the window has a new size; the pixel buffer was reallocated: redraw everything.
                     * Also sent with the UNCHANGED size when the user switches or reloads the theme:
                     * call ui_theme_load() again on every UI_EV_RESIZE, then redraw. */
  UI_EV_FOCUS,
  UI_EV_BLUR,
  UI_EV_CLOSE,      /* the user asked to close the window */
  UI_EV_WM_STATE,   /* for bars: workspace state changed; see ui_wm_state() */
  UI_EV_ZOOM,       /* the user pressed a zoom key (Ctrl+= Ctrl+- Ctrl+0 by default): wheel = +1 bigger,
                     * -1 smaller, 0 back to normal. A program that has no use for it ignores it. */
};
typedef struct {
  int type;
  int key, mods, repeat;
  unsigned codepoint;
  int x, y, button, wheel;
  int w, h;
} ui_event;

/* Connect and create a window. w, h are the size you would like; the manager decides
 * the real size and sends UI_EV_RESIZE before you first draw. Returns NULL if no manager is running. */
ui_window *ui_open(const char *title, const char *app_id, int w, int h, int role);
void ui_close(ui_window *win);
void ui_set_title(ui_window *win, const char *title);

/* The window's pixels: w*h unsigned ints, bytes in memory are R, G, B, A (same as libgfx). */
unsigned int *ui_pixels(ui_window *win);
int ui_width(ui_window *win);
int ui_height(ui_window *win);

/* Tell the manager a rectangle changed. Sends those pixels. */
void ui_present(ui_window *win, int x, int y, int w, int h);

/* Events. ui_fd() is the socket, for poll() together with other descriptors. */
int ui_fd(ui_window *win);
int ui_poll_event(ui_window *win, ui_event *ev);                 /* non-blocking: 1 if an event was returned */
int ui_wait_event(ui_window *win, ui_event *ev, int timeout_ms); /* blocks up to timeout_ms (-1 = forever): 1 event, 0 timeout, -1 manager gone */

/* For bars and other shell components: the manager's state. */
typedef struct {
  int workspace;          /* current workspace, 1-based */
  int workspace_count;
  unsigned occupied_mask; /* bit i set = workspace i+1 has windows */
  char focused_title[128];
} ui_wm_info;
int ui_wm_state(ui_window *win, ui_wm_info *out);
/* Ask the manager to do something, e.g. "workspace 2", "reload", "exec term". 0 on success. */
int ui_wm_command(ui_window *win, const char *command);

/* Commands the manager understands (the same words as the bind lines of ~/.config/wm/wm.uconf):
 *   exec CMD ARGS...   workspace N   move N   focus next|prev   swap next|prev|master
 *   master grow|shrink   fullscreen   float   kill   theme next|NAME   reload   exit
 *   layout master|split|toggle   open left|right|up|down PROGRAM ARGS...
 *   zoom in|out|reset (sends UI_EV_ZOOM to the focused window)
 *   stats (prints counters to the console)   bench (times full-screen composites)
 *   animation MS (length of the workspace slide until the next reload; 0 = none)
 * A window opened with role UI_FLOATING gets the size it asked for (UI_FLOATING_BOTTOM: the
 * same, placed at the bottom of the screen instead of the middle); ui_open with
 * role UI_BAR is docked at the top with height h. */

#endif
