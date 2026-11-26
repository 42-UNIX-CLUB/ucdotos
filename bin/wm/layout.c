// SPDX-License-Identifier: GPL-2.0-or-later
/* Workspaces, master-and-stack tiling, focus. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wm.h"

static int is_tiled(client *c) { return c->hello && c->role == UI_NORMAL && !c->floating && !c->fullscreen; }
static int on_ws(client *c, int ws) { return c->hello && c->role != UI_BAR && c->ws == ws; }
static client *fullscreen_on(int ws) {
  for (int i = nclients - 1; i >= 0; i--) if (on_ws(order[i], ws) && order[i]->fullscreen) return order[i];
  return NULL;
}

/* drawn on the screen right now */
int visible(client *c) {
  if (!c->hello) return 0;
  if (c->role == UI_BAR) return !fullscreen_on(cur_ws);
  if (c->ws != cur_ws) return 0;
  client *f = fullscreen_on(cur_ws);
  return !f || f == c || c->floating;
}
void content_rect(client *c, rect *o) {
  o->x = c->r.x + c->bw; o->y = c->r.y + c->bw;
  o->w = c->r.w - 2 * c->bw; o->h = c->r.h - 2 * c->bw;
}
unsigned occupied_mask(void) {
  unsigned m = 0;
  for (int i = 0; i < nclients; i++) if (on_ws(order[i], order[i]->ws)) m |= 1u << order[i]->ws;
  return m;
}
client *focused(void) {
  client *f = focus_of[cur_ws];
  for (int i = 0; i < nclients; i++) if (order[i] == f && on_ws(f, cur_ws)) return f;
  return NULL;
}

/* The area left for windows once the bar is docked. */
static rect tile_area(void) {
  int g = G_gap;
  rect a = {g, g, scr_w - 2 * g, scr_h - 2 * g};
  if (bar_h) { a.y = g + bar_h + g; a.h = scr_h - a.y - g; }
  if (a.w < 16) a.w = 16;
  if (a.h < 16) a.h = 16;
  return a;
}

void relayout(void) {
  bar_h = 0;
  for (int i = 0; i < nclients; i++)
    if (order[i]->hello && order[i]->role == UI_BAR) {
      int h = order[i]->req_h < 12 ? 12 : order[i]->req_h > 96 ? 96 : order[i]->req_h;
      if (h > bar_h) bar_h = h;
    }
  rect a = tile_area();
  int g = G_gap;
  tree_sync(); /* every workspace keeps its split tree up to date, whatever its layout */
  for (int ws = 0; ws < MAX_WS; ws++) {
    if (tree_place(ws, a)) continue; /* the split layout */
    int n = 0, k = 0;
    for (int i = 0; i < nclients; i++) if (is_tiled(order[i]) && order[i]->ws == ws) n++;
    int mw = n > 1 ? (int)((a.w - g) * master_of[ws]) : a.w;
    for (int i = 0; i < nclients; i++) {
      client *c = order[i];
      if (!is_tiled(c) || c->ws != ws) continue;
      c->bw = G_border;
      if (k == 0) c->r = (rect){a.x, a.y, mw, a.h};
      else {
        int sx = a.x + mw + g, sw = a.x + a.w - sx, each = (a.h - g * (n - 2)) / (n - 1);
        int y = a.y + (k - 1) * (each + g);
        c->r = (rect){sx, y, sw, k == n - 1 ? a.y + a.h - y : each};
      }
      k++;
    }
  }
  for (int i = 0; i < nclients; i++) {
    client *c = order[i];
    if (!c->hello) continue;
    if (c->role == UI_BAR) { c->bw = 0; c->r = (rect){g, g, scr_w - 2 * g, bar_h}; }
    else if (c->fullscreen) { c->bw = 0; c->r = (rect){0, 0, scr_w, scr_h}; }
    else if (c->floating) {
      c->bw = G_border;
      int w = c->req_w + 2 * c->bw, h = c->req_h + 2 * c->bw;
      if (w > a.w) w = a.w;
      if (h > a.h) h = a.h;
      c->r.w = w; c->r.h = h;
      if (!c->placed) { c->r.x = a.x + (a.w - w) / 2; c->r.y = c->bottom ? a.y + a.h - h : a.y + (a.h - h) / 3; c->placed = 1; }
      if (c->r.x + w > scr_w) c->r.x = scr_w - w;
      if (c->r.y + h > scr_h) c->r.y = scr_h - h;
      if (c->r.x < 0) c->r.x = 0;
      if (c->r.y < 0) c->r.y = 0;
    }
    if (c->r.w < 2 * c->bw + 1) c->r.w = 2 * c->bw + 1;
    if (c->r.h < 2 * c->bw + 1) c->r.h = 2 * c->bw + 1;
    rect cr;
    content_rect(c, &cr);
    if (cr.w != c->cw || cr.h != c->ch) send_configure(c);
  }
  damage_all();
  state_changed();
}

/* Topmost window under a screen point, in the order things are drawn, reversed. */
client *client_at(int x, int y) {
  for (int pass = 0; pass < 4; pass++)
    for (int i = nclients - 1; i >= 0; i--) {
      client *c = order[i];
      if (!visible(c)) continue;
      int kind = c->floating ? 0 : c->fullscreen ? 1 : c->role == UI_BAR ? 2 : 3;
      if (kind != pass) continue;
      if (x >= c->r.x && y >= c->r.y && x < c->r.x + c->r.w && y < c->r.y + c->r.h) return c;
    }
  return NULL;
}

void set_focus(client *c) {
  client *old = focused();
  if (c && c->role == UI_BAR) return;
  if (c && c->floating) { /* raise: floats are drawn in order, so move it to the end */
    int i = 0;
    while (i < nclients && order[i] != c) i++;
    if (i < nclients) { memmove(order + i, order + i + 1, (nclients - i - 1) * sizeof order[0]); order[nclients - 1] = c; }
    damage(c->r.x, c->r.y, c->r.w, c->r.h);
  }
  if (c) focus_of[c->ws] = c;
  if (old == c) return;
  if (old) { send_event(old, UI_EV_BLUR, 0, 0, 0, 0, 0, 0, 0, 0); damage_client_border(old); }
  if (c && c->ws == cur_ws) { send_event(c, UI_EV_FOCUS, 0, 0, 0, 0, 0, 0, 0, 0); damage_client_border(c); }
  if (!c) focus_of[cur_ws] = NULL;
  state_changed();
}

/* A sensible window to focus on workspace ws when `gone` leaves it. */
static client *pick_focus(int ws, client *gone) {
  int at = 0;
  while (at < nclients && order[at] != gone) at++;
  for (int i = nclients - 1; i >= 0; i--) /* the top floating window first */
    if (order[i] != gone && on_ws(order[i], ws) && order[i]->floating) return order[i];
  for (int d = 1; d < nclients; d++) /* then the nearest tile in the stack */
    for (int s = -1; s <= 1; s += 2) {
      int i = at + s * d;
      if (i >= 0 && i < nclients && order[i] != gone && on_ws(order[i], ws)) return order[i];
    }
  return NULL;
}

void client_added(client *c) {
  if (c->role != UI_BAR) {
    relayout();
    set_focus(c);
  } else relayout();
}
/* Called while c is still in order[]; drop_client removes it afterwards and calls relayout. */
void client_removed(client *c) {
  if (c->role == UI_BAR) return;
  if (focus_of[c->ws] == c) {
    client *n = pick_focus(c->ws, c);
    focus_of[c->ws] = n;
    if (n && c->ws == cur_ws) { send_event(n, UI_EV_FOCUS, 0, 0, 0, 0, 0, 0, 0, 0); }
  }
}

static client *step(client *from, int dir, int tiled_only) {
  int at = 0;
  while (at < nclients && order[at] != from) at++;
  for (int d = 1; d <= nclients; d++) {
    client *c = order[((at + dir * d) % nclients + nclients) % nclients];
    if (on_ws(c, cur_ws) && (!tiled_only || is_tiled(c))) return c;
  }
  return NULL;
}

int layout_command(const char *verb, const char *arg) {
  client *f = focused();
  if (!strcmp(verb, "workspace")) {
    int n = atoi(arg) - 1;
    if (n < 0 || n >= cfg.workspace_count) return -1;
    if (n == cur_ws) return 0;
    if (f) send_event(f, UI_EV_BLUR, 0, 0, 0, 0, 0, 0, 0, 0);
    int dir = n > cur_ws ? 1 : -1;
    anim_begin();
    cur_ws = n;
    f = focused();
    if (f) send_event(f, UI_EV_FOCUS, 0, 0, 0, 0, 0, 0, 0, 0);
    anim_go(dir);
    damage_all();
    state_changed();
    return 0;
  }
  if (!strcmp(verb, "move")) {
    int n = atoi(arg) - 1;
    if (n < 0 || n >= cfg.workspace_count || !f) return -1;
    if (n == cur_ws) return 0;
    focus_of[cur_ws] = pick_focus(cur_ws, f);
    f->ws = n;
    send_event(f, UI_EV_BLUR, 0, 0, 0, 0, 0, 0, 0, 0);
    focus_of[n] = f;
    client *nf = focused();
    if (nf) send_event(nf, UI_EV_FOCUS, 0, 0, 0, 0, 0, 0, 0, 0);
    relayout();
    return 0;
  }
  if (!strcmp(verb, "focus")) {
    if (!f) { /* nothing focused: take the first window here */
      for (int i = 0; i < nclients; i++) if (on_ws(order[i], cur_ws)) { set_focus(order[i]); return 0; }
      return -1;
    }
    client *c = step(f, !strcmp(arg, "prev") ? -1 : 1, 0);
    if (c) set_focus(c);
    return 0;
  }
  if (!strcmp(verb, "swap")) {
    if (!f || !is_tiled(f)) return -1;
    client *c = !strcmp(arg, "master") ? NULL : step(f, !strcmp(arg, "prev") ? -1 : 1, 1);
    if (!strcmp(arg, "master"))
      for (int i = 0; i < nclients && !c; i++) if (is_tiled(order[i]) && order[i]->ws == cur_ws) c = order[i];
    if (!c || c == f) return 0;
    int a = 0, b = 0;
    while (order[a] != f) a++;
    while (order[b] != c) b++;
    order[a] = c; order[b] = f;
    tree_swap(f, c);
    relayout();
    return 0;
  }
  if (!strcmp(verb, "layout")) { /* master | split | toggle, for this workspace */
    int l = !strcmp(arg, "master") ? LAYOUT_MASTER : !strcmp(arg, "split") ? LAYOUT_SPLIT :
            !strcmp(arg, "toggle") ? !layout_of[cur_ws] : -1;
    if (l < 0) return -1;
    layout_of[cur_ws] = l;
    relayout();
    return 0;
  }
  if (!strcmp(verb, "master")) {
    float d = !strcmp(arg, "grow") ? 0.05f : !strcmp(arg, "shrink") ? -0.05f : 0;
    if (layout_of[cur_ws] == LAYOUT_SPLIT) { /* in a split tree: grow or shrink the focused window */
      if (tree_resize(f, d) == 0) relayout();
      return 0;
    }
    float m = master_of[cur_ws] + d;
    master_of[cur_ws] = m < 0.1f ? 0.1f : m > 0.9f ? 0.9f : m;
    relayout();
    return 0;
  }
  if (!strcmp(verb, "fullscreen")) {
    if (!f) return -1;
    f->fullscreen = !f->fullscreen;
    relayout();
    return 0;
  }
  if (!strcmp(verb, "float")) {
    if (!f) return -1;
    f->floating = !f->floating;
    if (f->floating) { f->placed = 0; f->req_w = scr_w * 3 / 5; f->req_h = scr_h * 3 / 5; } /* a centred popup of 60 % */
    relayout();
    return 0;
  }
  return -1;
}
