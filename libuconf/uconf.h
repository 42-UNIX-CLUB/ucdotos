// SPDX-License-Identifier: GPL-2.0-or-later
/* uconf: the dotfile format of UC.OS (.uconf files in ~/.config and the themes).
 *
 * It mixes three formats people already know:
 *   [section]                 TOML headers; dotted names ([a.b]) are allowed
 *   name { ... }              JSON-style braces open a block inside a section; blocks nest
 *   $mod = ALT                hyprlang variables, used later as $mod
 *   bind = $mod SHIFT, Q, kill   hyprlang key bindings: MODS, key, action, arguments
 *
 * The rest:
 *   key = value               one setting; the value runs to the end of the line
 *   # comment                 a line whose first character is #, or "# " after a value.
 *                             A # with no blank after it is part of the value: #fe8019
 *   "text"                    a value that is one quoted string loses its quotes
 *                             (escapes \" \\ \n \t); other quotes are kept as written
 *   inside braces, ", key =" starts the next setting, so one line can hold several:
 *                             colors { focus = #fe8019, normal = #665c54 }
 *   a key may repeat (bind, exec): every one is kept, in order.
 *
 * A setting's full name is its section, its blocks and its key joined with dots:
 *   [general] border { width = 2 }   is   general.border.width
 *
 * Mistakes do not stop the parser: each is reported with the file and line, and the
 * line is skipped. Programs can also warn about settings they never read (typos).
 * Plain C, no system calls of its own besides reading the file: the terminal's native
 * tests build it with the host compiler too. */
#ifndef UCONF_H
#define UCONF_H
#include <stddef.h>

typedef struct {
  const char *key;   /* full dotted name */
  const char *value; /* variables expanded, outer quotes removed */
  const char *raw;   /* the value as written in the file: $variables and quotes kept */
  int line;
  int used;          /* set by the readers below */
} uconf_entry;

typedef struct {
  char name[64];     /* for messages: the file name */
  uconf_entry *e;
  int n, cap;
  char **vars;       /* name, value, name, value ... */
  int nvars;
  char **blocks;     /* every allocation, to free them */
  int nblocks;
  int errors;
} uconf;

/* Read and parse a file. Returns 0, or -1 if it cannot be read (c is then empty but
 * usable). Parse mistakes are printed to stderr as "name:line: message". */
int uconf_load(uconf *c, const char *path);
/* Parse text already in memory; name is used in messages. */
void uconf_parse(uconf *c, const char *text, const char *name);
void uconf_free(uconf *c);

/* Readers. A missing setting gives def. Every reader marks what it read as used. */
const char *uconf_str(uconf *c, const char *key, const char *def);
int uconf_int(uconf *c, const char *key, int def, int min, int max);
double uconf_num(uconf *c, const char *key, double def);
int uconf_bool(uconf *c, const char *key, int def);           /* true/false yes/no on/off 1/0 */
unsigned uconf_color(uconf *c, const char *key, unsigned def); /* #rrggbb, rrggbb, 0xrrggbb, #rgb -> 0xRRGGBB */
/* Repeated settings: the index of the next entry named key after index `after` (start
 * with -1), or -1 when there are no more. */
int uconf_next(uconf *c, const char *key, int after);
const char *uconf_var(uconf *c, const char *name);            /* a $variable, or NULL */

/* Split s in place at commas into at most max fields, blanks trimmed. The last field
 * keeps any further commas. Returns the number of fields. */
int uconf_split(char *s, char **out, int max);
/* Parse a colour on its own; returns -1 if it is not one. */
int uconf_parse_color(const char *s, unsigned *rgb);

/* Print "name:line: unknown setting 'key'" for every setting nothing read. */
void uconf_warn_unused(uconf *c);

#endif
