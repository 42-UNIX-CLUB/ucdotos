// SPDX-License-Identifier: GPL-2.0-or-later
/* The split layout: each workspace keeps a binary tree of splits (like i3 or Hyprland's
 * dwindle). Every tiled window is a leaf; an inner node splits its rectangle side by side
 * ('h') or one above the other ('v') at `ratio`. A new window halves the focused one: on the
 * side asked for (open left|right|up|down), else along the longer side.
 *
 * Memory: the nodes live in a static pool sized for the client limit, so this file never
 * allocates. The trees are maintained by tree_sync(), which relayout() calls first: it
 * drops leaves whose window is gone or no longer tiled here (comparing pointers only, never
 * following one to a window that may have been freed) and adds windows that have no leaf.
 * Every workspace keeps its tree whatever its layout, so switching between master and
 * split keeps the arrangement. */
#include <string.h>
#include "wm.h"

typedef struct node {
  struct node *a, *b, *parent;
  client *c;          /* leaves only */
  float ratio;        /* share of the first child */
  char split;         /* 0 leaf, 'h' side by side, 'v' one above the other */
  char ws;
} node;

#define MAX_NODES (2 * MAX_CLIENTS)
static node pool[MAX_NODES];
static node *free_list;
static node *root_of[MAX_WS];
int layout_of[MAX_WS];  /* LAYOUT_MASTER or LAYOUT_SPLIT */

static node *get(void) {
  static int ready;
  if (!ready) { for (int i = 0; i < MAX_NODES; i++) { pool[i].a = free_list; free_list = &pool[i]; } ready = 1; }
  node *n = free_list;
  if (!n) return NULL; /* cannot happen: at most MAX_CLIENTS leaves and one fewer inner nodes */
  free_list = n->a;
  memset(n, 0, sizeof *n);
  return n;
}
static void put(node *n) { n->a = free_list; free_list = n; n->c = NULL; }

/* windows that belong in the tree: normal windows of this workspace, fullscreen included
 * (its place is kept behind it), floating ones and bars excluded */
static int belongs(const client *c, int ws) { return c->hello && c->role == UI_NORMAL && !c->floating && c->ws == ws; }
static int alive(const client *c) {
  for (int i = 0; i < nclients; i++) if (order[i] == c) return 1;
  return 0;
}

static void replace(node *old, node *with) {
  with->parent = old->parent;
  if (!old->parent) root_of[(int)old->ws] = with;
  else if (old->parent->a == old) old->parent->a = with;
  else old->parent->b = with;
}
static void remove_leaf(node *l) {
  node *p = l->parent;
  if (l->c && alive(l->c) && l->c->leaf == l) l->c->leaf = NULL;
  if (!p) root_of[(int)l->ws] = NULL;
  else { replace(p, p->a == l ? p->b : p->a); put(p); }
  put(l);
}
static node *last_leaf(node *n) {
  while (n && n->split) n = n->b;
  return n;
}

/* Put c in its workspace's tree, splitting `target` (or the focused window, or the last one)
 * on side dir: 'l' 'r' 'u' 'd', or 0 for the longer side. */
static void insert(client *c, client *target, int dir) {
  int ws = c->ws;
  node *l = get();
  if (!l) return;
  l->c = c; l->ws = (char)ws;
  c->leaf = l;
  if (!root_of[ws]) { root_of[ws] = l; return; }
  node *t = target && alive(target) && target->leaf && target->ws == ws ? target->leaf : NULL;
  if (!t) { client *f = focus_of[ws]; t = f && alive(f) && f->leaf && f->ws == ws && f != c ? f->leaf : NULL; }
  if (!t) t = last_leaf(root_of[ws]);
  if (!dir) dir = t->c && t->c->r.w >= t->c->r.h ? 'r' : 'd';
  node *n = get();
  if (!n) { put(l); c->leaf = NULL; return; }
  n->ws = (char)ws;
  n->split = dir == 'l' || dir == 'r' ? 'h' : 'v';
  n->ratio = 0.5f;
  replace(t, n);
  if (dir == 'l' || dir == 'u') { n->a = l; n->b = t; } else { n->a = t; n->b = l; }
  l->parent = t->parent = n;
}

static int collect(node *n, node **out, int k) {
  if (!n) return k;
  if (!n->split) { if (k < MAX_CLIENTS) out[k++] = n; return k; }
  return collect(n->b, out, collect(n->a, out, k));
}

void tree_sync(void) {
  for (int ws = 0; ws < MAX_WS; ws++) {
    node *leaves[MAX_CLIENTS];
    int n = collect(root_of[ws], leaves, 0);
    for (int i = 0; i < n; i++) {
      client *c = leaves[i]->c;
      if (!c || !alive(c) || !belongs(c, ws) || c->leaf != leaves[i]) remove_leaf(leaves[i]);
    }
  }
  for (int i = 0; i < nclients; i++) {
    client *c = order[i];
    if (c->leaf && !belongs(c, c->ws)) c->leaf = NULL; /* its old leaf went just above */
    if (!c->leaf && belongs(c, c->ws)) {
      client *t = c->insert_target;
      insert(c, t && alive(t) ? t : NULL, c->insert_dir);
      c->insert_target = NULL;
      c->insert_dir = 0;
    }
  }
}

static void place(node *n, rect r, int g) {
  if (!n) return;
  if (!n->split) { n->c->bw = G_border; n->c->r = r; return; }
  rect ra = r, rb = r;
  if (n->split == 'h') {
    ra.w = (int)((r.w - g) * n->ratio);
    rb.x = r.x + ra.w + g; rb.w = r.w - ra.w - g;
  } else {
    ra.h = (int)((r.h - g) * n->ratio);
    rb.y = r.y + ra.h + g; rb.h = r.h - ra.h - g;
  }
  place(n->a, ra, g);
  place(n->b, rb, g);
}
/* Lay out workspace ws in area a if it uses the split layout; 0 if it does not. */
int tree_place(int ws, rect a) {
  if (layout_of[ws] != LAYOUT_SPLIT) return 0;
  place(root_of[ws], a, G_gap);
  return 1;
}

/* Grow (d > 0) or shrink the focused window inside the split that holds it. */
int tree_resize(client *c, float d) {
  if (!c || !c->leaf || !c->leaf->parent) return -1;
  node *p = c->leaf->parent;
  float r = p->ratio + (p->a == c->leaf ? d : -d);
  p->ratio = r < 0.1f ? 0.1f : r > 0.9f ? 0.9f : r;
  return 0;
}
/* Two windows trade places in the tree (order[] is swapped by the caller). */
void tree_swap(client *x, client *y) {
  if (!x->leaf || !y->leaf) return;
  node *lx = x->leaf, *ly = y->leaf;
  lx->c = y; ly->c = x;
  x->leaf = ly; y->leaf = lx;
}
