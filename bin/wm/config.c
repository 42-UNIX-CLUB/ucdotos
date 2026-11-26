// SPDX-License-Identifier: GPL-2.0-or-later
/* ~/.config/wm/config: an i3-style dotfile. See rootfs/home/user/.config/wm/config for the format. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wm.h"

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
    {"Mod+t", "theme next"}, {"Mod+Shift+r", "reload"}, {"Mod+Shift+e", "exit"},
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
  G_gap = cfg.gap >= 0 ? cfg.gap : theme.gap;
  G_border = cfg.border >= 0 ? cfg.border : theme.border;
  G_radius = cfg.radius >= 0 ? cfg.radius : theme.radius;
  G_thin = cfg.border_thin >= 0 ? cfg.border_thin : 1;
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
  snprintf(path, sizeof path, "%s/.config/wm/config", home ? home : "/home/user");
  char *s = (char *)ui_read_file(path, NULL);
  if (!s) fprintf(stderr, "wm: no %s, using built-in defaults\n", path);
  /* pass 0 reads `set mod` first so that later `bind Mod+...` lines know what Mod means */
  for (int pass = 0; pass < 2 && s; pass++) {
    if (pass == 1) default_binds();
    char *copy = strdup(s);
    if (!copy) break;
    int lineno = 0;
    for (char *line = copy, *next; line && *line; line = next) {
      next = strchr(line, '\n');
      if (next) *next++ = 0;
      lineno++;
      while (*line == ' ' || *line == '\t') line++;
      if (*line == '#' || !*line) continue;
      for (char *e = line + strlen(line); e > line && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r');) *--e = 0;
      char word[32], *rest = line;
      int n = 0;
      while (*rest && *rest != ' ' && *rest != '\t' && n < 31) word[n++] = *rest++;
      word[n] = 0;
      while (*rest == ' ' || *rest == '\t') rest++;
      if (pass == 0) {
        if (!strcmp(word, "set") && !strncmp(rest, "mod", 3) && (rest[3] == ' ' || rest[3] == '\t')) {
          int m = mod_bit(rest + 4 + strspn(rest + 4, " \t"));
          if (m && m != cfg.mod) cfg.mod = m;
        }
        continue;
      }
      if (!strcmp(word, "set")) continue; /* handled in pass 0 */
      else if (!strcmp(word, "gaps")) cfg.gap = atoi(rest) < 0 ? 0 : atoi(rest) > 100 ? 100 : atoi(rest);
      else if (!strcmp(word, "border")) cfg.border = atoi(rest) < 0 ? 0 : atoi(rest) > 20 ? 20 : atoi(rest);
      else if (!strcmp(word, "border_thin")) cfg.border_thin = atoi(rest) < 0 ? 0 : atoi(rest);
      else if (!strcmp(word, "radius")) cfg.radius = atoi(rest) < 0 ? 0 : atoi(rest) > 40 ? 40 : atoi(rest);
      else if (!strcmp(word, "workspace_count")) { int k = atoi(rest); cfg.workspace_count = k < 1 ? 1 : k > MAX_WS ? MAX_WS : k; }
      else if (!strcmp(word, "animation_ms")) cfg.animation_ms = atoi(rest) < 0 ? 0 : atoi(rest) > 5000 ? 5000 : atoi(rest);
      else if (!strcmp(word, "master_ratio")) {
        float f = (float)atof(rest); /* accept 0.55 or 55 (percent) */
        if (f > 1) f /= 100.f;
        cfg.master = f < 0.1f ? 0.1f : f > 0.9f ? 0.9f : f;
      } else if (!strcmp(word, "bind")) {
        char combo[48];
        int k = 0;
        while (*rest && *rest != ' ' && *rest != '\t' && k < 47) combo[k++] = *rest++;
        combo[k] = 0;
        while (*rest == ' ' || *rest == '\t') rest++;
        if (*rest) bind(combo, rest);
      } else if ((!strcmp(word, "exec") || !strcmp(word, "exec_always")) && cfg.nauto < 16 && *rest) {
        cfg.always[cfg.nauto] = word[4] == '_';
        strlcpy(cfg.autostart[cfg.nauto++], rest, 128);
      } else fprintf(stderr, "wm: config line %d: unknown '%s'\n", lineno, word);
    }
    free(copy);
  }
  if (!s) {
    default_binds();
    strcpy(cfg.autostart[cfg.nauto++], "bar");
    strcpy(cfg.autostart[cfg.nauto++], "term");
  }
  free(s);
  if (!first) {
    /* on reload only exec_always lines run again; `exec` lines ran at startup */
    for (int i = 0; i < cfg.nauto; i++) if (cfg.always[i]) wm_spawn(cfg.autostart[i]);
    cfg.nauto = 0;
    /* fewer workspaces than before: bring the windows of the vanished ones to the last one */
    for (int i = 0; i < nclients; i++) if (order[i]->ws >= cfg.workspace_count) order[i]->ws = cfg.workspace_count - 1;
    if (cur_ws >= cfg.workspace_count) cur_ws = cfg.workspace_count - 1;
  }
  apply_geometry();
}
