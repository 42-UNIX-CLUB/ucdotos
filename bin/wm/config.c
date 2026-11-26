// SPDX-License-Identifier: GPL-2.0-or-later
/* ~/.config/wm/wm.uconf: the window manager's dotfile, in the .uconf format (libuconf/uconf.h).
 * See rootfs/home/user/.config/wm/wm.uconf for every setting, with comments. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wm.h"
#include "uconf.h"

static const struct { const char *name; int code; } keys[] = {
  {"Escape", 1}, {"minus", 12}, {"equal", 13}, {"BackSpace", 14}, {"Tab", 15},
  {"bracketleft", 26}, {"bracketright", 27}, {"Return", 28}, {"Enter", 28}, {"semicolon", 39}, {"apostrophe", 40},
  {"grave", 41}, {"backslash", 43}, {"comma", 51}, {"period", 52}, {"slash", 53}, {"space", 57},
  {"F1", 59}, {"F2", 60}, {"F3", 61}, {"F4", 62}, {"F5", 63}, {"F6", 64}, {"F7", 65}, {"F8", 66}, {"F9", 67}, {"F10", 68},
  {"F11", 87}, {"F12", 88}, {"Home", 102}, {"Up", 103}, {"Prior", 104}, {"PageUp", 104}, {"Left", 105}, {"Right", 106},
  {"End", 107}, {"Down", 108}, {"Next", 109}, {"PageDown", 109}, {"Insert", 110}, {"Delete", 111},
};
static const char letters[] = "qwertyuiop\0\0\0\0asdfghjkl\0\0\0\0\0\0zxcvbnm"; /* Linux codes 16..50 */

static int key_code(const char *s) {
  if (!s[1]) {
    char ch = s[0] >= 'A' && s[0] <= 'Z' ? s[0] + 32 : s[0];
    if (ch >= '1' && ch <= '9') return ch - '1' + 2;
    if (ch == '0') return 11;
    for (int i = 0; i < (int)sizeof letters; i++) if (letters[i] == ch && ch) return 16 + i;
  }
  for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++) if (!strcasecmp(keys[i].name, s)) return keys[i].code;
  return -1;
}
static int mod_bit(const char *s) {
  if (!strcasecmp(s, "Mod")) return cfg.mod;
  if (!strcasecmp(s, "Shift")) return UI_SHIFT;
  if (!strcasecmp(s, "Ctrl") || !strcasecmp(s, "Control")) return UI_CTRL;
  if (!strcasecmp(s, "Alt") || !strcasecmp(s, "Mod1")) return UI_ALT;
  if (!strcasecmp(s, "Super") || !strcasecmp(s, "Mod4")) return UI_SUPER;
  return 0;
}
/* "Mod+Shift+q" -> modifier bits and a Linux key code. */
int parse_combo(const char *s, int *mods, int *key) {
  char part[32];
  *mods = 0; *key = -1;
  while (*s) {
    int n = 0;
    while (*s && *s != '+' && n < 31) part[n++] = *s++;
    part[n] = 0;
    if (*s == '+') s++;
    int m = mod_bit(part);
    if (m && *s) *mods |= m;     /* a modifier name before the last '+' */
    else if ((*key = key_code(part)) < 0) return -1;
  }
  return *key < 0 ? -1 : 0;
}

static void bind(const char *combo, const char *cmd) {
  int m, k;
  if (parse_combo(combo, &m, &k) < 0) { fprintf(stderr, "wm: config: unknown key '%s'\n", combo); return; }
  for (int i = 0; i < cfg.nbinds; i++) /* a later line replaces an earlier binding of the same keys */
    if (cfg.binds[i].mods == m && cfg.binds[i].key == k) { strlcpy(cfg.binds[i].cmd, cmd, sizeof cfg.binds[i].cmd); return; }
  if (cfg.nbinds == MAX_BINDS) return;
  binding *b = &cfg.binds[cfg.nbinds++];
  b->mods = m; b->key = k;
  strlcpy(b->cmd, cmd, sizeof b->cmd);
}

/* The bindings every user gets even with no config file at all. */
static void default_binds(void) {
  static const char *d[][2] = {
    {"Mod+Return", "exec term"}, {"Mod+d", "exec launcher"}, {"Mod+Shift+q", "kill"},
    {"Mod+j", "focus next"}, {"Mod+k", "focus prev"}, {"Mod+Down", "focus next"}, {"Mod+Up", "focus prev"},
    {"Mod+Right", "focus next"}, {"Mod+Left", "focus prev"},
    {"Mod+Shift+j", "swap next"}, {"Mod+Shift+k", "swap prev"}, {"Mod+Shift+Down", "swap next"}, {"Mod+Shift+Up", "swap prev"},
    {"Mod+h", "master shrink"}, {"Mod+l", "master grow"}, {"Mod+f", "fullscreen"}, {"Mod+Shift+space", "float"},
    {"Mod+t", "theme next"}, {"Mod+Shift+t", "exec themes"}, {"Mod+Shift+r", "reload"}, {"Mod+Shift+e", "exit"},
    {"Mod+s", "layout toggle"},
    {"Mod+Ctrl+h", "open left term"}, {"Mod+Ctrl+l", "open right term"}, {"Mod+Ctrl+k", "open up term"}, {"Mod+Ctrl+j", "open down term"},
    {"Mod+Ctrl+Left", "open left term"}, {"Mod+Ctrl+Right", "open right term"}, {"Mod+Ctrl+Up", "open up term"}, {"Mod+Ctrl+Down", "open down term"},
    {"Ctrl+equal", "zoom in"}, {"Ctrl+Shift+equal", "zoom in"}, {"Ctrl+minus", "zoom out"}, {"Ctrl+0", "zoom reset"},
  };
  for (unsigned i = 0; i < sizeof d / sizeof d[0]; i++) bind(d[i][0], d[i][1]);
  for (int i = 1; i <= 9; i++) {
    char k[24], c[24];
    snprintf(k, sizeof k, "Mod+%d", i); snprintf(c, sizeof c, "workspace %d", i); bind(k, c);
    snprintf(k, sizeof k, "Mod+Shift+%d", i); snprintf(c, sizeof c, "move %d", i); bind(k, c);
  }
}

void apply_geometry(void) {
  /* border colours set in wm.uconf win over the theme's (0 = not set) */
  if (cfg.col_focus) theme.border_focus = cfg.col_focus;
  if (cfg.col_normal) theme.border_normal = cfg.col_normal;
  if (cfg.col_float) theme.border_float = cfg.col_float;
  G_gap = cfg.gap >= 0 ? cfg.gap : theme.gap;
  G_border = cfg.border >= 0 ? cfg.border : theme.border;
  G_radius = cfg.radius >= 0 ? cfg.radius : theme.radius;
  G_thin = cfg.border_thin >= 0 ? cfg.border_thin : 1;
}

/* "ALT SHIFT", "SUPER_SHIFT", "CTRL+ALT" or "" -> modifier bits; -1 for an unknown name.
 * MOD (or $mod, which the parser has already replaced) is the configured modifier. */
static int mods_of(const char *s) {
  int m = 0;
  char part[16];
  while (*s) {
    int n = 0;
    while (*s && *s != ' ' && *s != '\t' && *s != '_' && *s != '+') { if (n < 15) part[n++] = *s; s++; }
    part[n] = 0;
    while (*s == ' ' || *s == '\t' || *s == '_' || *s == '+') s++;
    if (!n) continue;
    int b = !strcasecmp(part, "Win") || !strcasecmp(part, "Logo") ? UI_SUPER : mod_bit(part);
    if (!b) return -1;
    m |= b;
  }
  return m;
}
/* bind = MODS, key, action, arguments   (hyprlang style; the arguments keep their commas)
 * unbind = MODS, key */
static void bind_entry(uconf *c, uconf_entry *e, int unbind) {
  char v[256], *f[4];
  strlcpy(v, e->value, sizeof v);
  int n = uconf_split(v, f, unbind ? 2 : 4);
  int m = n >= 2 ? mods_of(f[0]) : -1, k = n >= 2 ? key_code(f[1]) : -1;
  if (n < (unbind ? 2 : 3) || m < 0 || k < 0) {
    fprintf(stderr, "%s:%d: %s needs MODS, key%s, with known names: '%s'\n", c->name, e->line,
            unbind ? "unbind" : "bind", unbind ? "" : ", action[, arguments]", e->value);
    return;
  }
  if (unbind) {
    for (int i = 0; i < cfg.nbinds; i++)
      if (cfg.binds[i].mods == m && cfg.binds[i].key == k) { cfg.binds[i] = cfg.binds[--cfg.nbinds]; i--; }
    return;
  }
  for (int i = 0; i < cfg.nbinds; i++) /* the same keys again: the later line wins */
    if (cfg.binds[i].mods == m && cfg.binds[i].key == k) { cfg.binds[i] = cfg.binds[--cfg.nbinds]; break; }
  if (cfg.nbinds == MAX_BINDS) return;
  binding *b = &cfg.binds[cfg.nbinds++];
  b->mods = m; b->key = k;
  snprintf(b->cmd, sizeof b->cmd, "%s%s%s", f[2], n == 4 && *f[3] ? " " : "", n == 4 ? f[3] : "");
}

void config_load(int first) {
  memset(&cfg, 0, sizeof cfg);
  cfg.mod = UI_ALT;
  cfg.gap = cfg.border = cfg.radius = cfg.border_thin = -1;
  cfg.workspace_count = 4;
  cfg.animation_ms = 160;
  cfg.master = 0.5f;
  const char *home = getenv("HOME");
  char path[256];
  snprintf(path, sizeof path, "%s/.config/wm/wm.uconf", home ? home : "/home/user");
  uconf c;
  int have = uconf_load(&c, path) == 0;
  if (!have) fprintf(stderr, "wm: no %s, using built-in defaults\n", path);
  const char *mod = uconf_var(&c, "mod");
  if (mod) {
    int m = mods_of(mod);
    if (m > 0) cfg.mod = m;
    else fprintf(stderr, "%s: $mod = %s is not a modifier (ALT, SUPER, CTRL, SHIFT)\n", c.name, mod);
  }
  cfg.gap = uconf_int(&c, "general.gaps", -1, 0, 100);
  cfg.workspace_count = uconf_int(&c, "general.workspaces", 4, 1, MAX_WS);
  cfg.animation_ms = uconf_int(&c, "general.animation", 160, 0, 5000);
  const char *lay = uconf_str(&c, "general.layout", "master");
  if (strcmp(lay, "master") && strcmp(lay, "split")) fprintf(stderr, "%s: layout = %s is not master or split\n", c.name, lay);
  for (int ws = 0; ws < MAX_WS; ws++) layout_of[ws] = !strcmp(lay, "split") ? LAYOUT_SPLIT : LAYOUT_MASTER;
  float f = (float)uconf_num(&c, "general.master_ratio", 0.5); /* 0.55 or 55 (percent) */
  if (f > 1) f /= 100.f;
  cfg.master = f < 0.1f ? 0.1f : f > 0.9f ? 0.9f : f;
  cfg.border = uconf_int(&c, "general.border.width", -1, 0, 20);
  cfg.border_thin = uconf_int(&c, "general.border.thin", -1, 0, 20);
  cfg.radius = uconf_int(&c, "general.border.radius", -1, 0, 40);
  static const char *col_keys[3] = {"general.border.colors.focus", "general.border.colors.normal", "general.border.colors.float"};
  unsigned *col_dst[3] = {&cfg.col_focus, &cfg.col_normal, &cfg.col_float};
  for (int i = 0; i < 3; i++)
    if (uconf_str(&c, col_keys[i], NULL)) *col_dst[i] = GFX_HEX(uconf_color(&c, col_keys[i], 0));
  /* bindings: the built-in ones first (unless defaults = false), then the file's, in order */
  if (!have || uconf_bool(&c, "binds.defaults", 1)) default_binds();
  for (int i = 0; i < c.n; i++) {
    uconf_entry *e = &c.e[i];
    if (!strcmp(e->key, "binds.bind")) { e->used = 1; bind_entry(&c, e, 0); }
    else if (!strcmp(e->key, "binds.unbind")) { e->used = 1; bind_entry(&c, e, 1); }
    else if ((!strcmp(e->key, "autostart.exec") || !strcmp(e->key, "autostart.exec_always")) && cfg.nauto < 16) {
      e->used = 1;
      cfg.always[cfg.nauto] = e->key[14] == '_';
      strlcpy(cfg.autostart[cfg.nauto++], e->value, 128);
    }
  }
  if (!have) {
    strcpy(cfg.autostart[cfg.nauto++], "bar");
    strcpy(cfg.autostart[cfg.nauto++], "term");
  }
  uconf_warn_unused(&c);
  uconf_free(&c);
  if (!first) {
    /* on reload only exec_always lines run again; exec lines ran at startup */
    for (int i = 0; i < cfg.nauto; i++) if (cfg.always[i]) wm_spawn(cfg.autostart[i]);
    cfg.nauto = 0;
    /* fewer workspaces than before: bring the windows of the vanished ones to the last one */
    for (int i = 0; i < nclients; i++) if (order[i]->ws >= cfg.workspace_count) order[i]->ws = cfg.workspace_count - 1;
    if (cur_ws >= cfg.workspace_count) cur_ws = cfg.workspace_count - 1;
  }
  apply_geometry();
}
