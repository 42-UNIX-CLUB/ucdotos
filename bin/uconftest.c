// SPDX-License-Identifier: GPL-2.0-or-later
/* uconftest: checks the .uconf parser (libuconf). Prints ok/FAIL per check and a summary
 * line "uconftest: N checks, M failed". Plain C: it also builds with the host compiler.
 * Parse mistakes the tests make on purpose are printed to stderr, as in real use. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "uconf.h"

static int fails, checks;
static void check(const char *name, int ok) {
  checks++;
  if (!ok) fails++;
  printf("  %s  %s\n", ok ? "ok  " : "FAIL", name);
}
#define CHECK(name, cond) check(name, (cond) != 0)
static int is(const char *a, const char *b) { return a && b && !strcmp(a, b); }

static void parse(uconf *c, const char *text) {
  memset(c, 0, sizeof *c);
  uconf_parse(c, text, "test.uconf");
}

int main(void) {
  uconf c;

  parse(&c, "# a comment\n[general]\ngaps = 12\nname = hello world\n\n[other]\ngaps = 3\n");
  CHECK("section and key", uconf_int(&c, "general.gaps", 0, 0, 100) == 12);
  CHECK("value runs to the end of the line", is(uconf_str(&c, "general.name", NULL), "hello world"));
  CHECK("the same key in another section", uconf_int(&c, "other.gaps", 0, 0, 100) == 3);
  CHECK("a missing key gives the default", uconf_int(&c, "general.nope", 7, 0, 100) == 7);
  CHECK("no errors", c.errors == 0);
  uconf_free(&c);

  parse(&c, "top = 1\n[general]\nborder {\n    width = 2\n    colors {\n        focus = #fe8019\n    }\n}\nafter = 3\n");
  CHECK("a key before any section has no prefix", uconf_int(&c, "top", 0, 0, 9) == 1);
  CHECK("block: section.block.key", uconf_int(&c, "general.border.width", 0, 0, 9) == 2);
  CHECK("nested blocks", is(uconf_str(&c, "general.border.colors.focus", NULL), "#fe8019"));
  CHECK("after the block, back in the section", uconf_int(&c, "general.after", 0, 0, 9) == 3);
  CHECK("no errors in blocks", c.errors == 0);
  uconf_free(&c);

  parse(&c, "[b]\ncolors { focus = #fe8019, normal = #665c54 }\none { x = 1 }\ntwo{y=2}\n");
  CHECK("inline block, first setting", is(uconf_str(&c, "b.colors.focus", NULL), "#fe8019"));
  CHECK("inline block, ', key =' starts the next", is(uconf_str(&c, "b.colors.normal", NULL), "#665c54"));
  CHECK("inline block, one setting", uconf_int(&c, "b.one.x", 0, 0, 9) == 1);
  CHECK("no blanks needed", uconf_int(&c, "b.two.y", 0, 0, 9) == 2);
  CHECK("no errors inline", c.errors == 0);
  uconf_free(&c);

  parse(&c, "a = 12   # trailing comment\nb = #282828   # a colour, then a comment\nc = x#y\nd = #\ne = value #\n#only = a comment\n");
  CHECK("'# ' after a value is a comment", uconf_int(&c, "a", 0, 0, 99) == 12);
  CHECK("#rrggbb is a value", is(uconf_str(&c, "b", NULL), "#282828"));
  CHECK("# inside a word is kept", is(uconf_str(&c, "c", NULL), "x#y"));
  CHECK("a lone # as the value is kept", is(uconf_str(&c, "d", NULL), "#"));
  CHECK("# at the end after a blank is a comment", is(uconf_str(&c, "e", NULL), "value"));
  CHECK("a line starting with # is a comment", !uconf_str(&c, "only", NULL));
  uconf_free(&c);

  parse(&c, "$mod = ALT\n$term = term -e sh\n$both = $mod SHIFT\n[binds]\nbind = $mod, Return, exec, $term\nbind = $both, Q, kill\nprice = $$5\n");
  int i = uconf_next(&c, "binds.bind", -1);
  CHECK("first bind, variables expanded", i >= 0 && is(c.e[i].value, "ALT, Return, exec, term -e sh"));
  i = uconf_next(&c, "binds.bind", i);
  CHECK("a variable defined from another", i >= 0 && is(c.e[i].value, "ALT SHIFT, Q, kill"));
  CHECK("raw keeps the value as written", i >= 0 && is(c.e[i].raw, "$both, Q, kill"));
  CHECK("no third bind", uconf_next(&c, "binds.bind", i) == -1);
  CHECK("$$ is a dollar sign", is(uconf_str(&c, "binds.price", NULL), "$5"));
  CHECK("uconf_var", is(uconf_var(&c, "mod"), "ALT"));
  CHECK("no errors with variables", c.errors == 0);
  uconf_free(&c);

  parse(&c, "x = $nope here\n");
  CHECK("an unknown variable is an error", c.errors == 1);
  CHECK("and is left as written", is(uconf_str(&c, "x", NULL), "$nope here"));
  uconf_free(&c);

  parse(&c, "a = \"quoted # not a comment\"\nb = \"tab\\tquote\\\" end\"\nc = term -e sh -c \"echo hi\"\nd = \"a\" \"b\"\ne = 'single'\n"
            "blk { cmd = \"echo }\" }\n");
  CHECK("quotes protect #", is(uconf_str(&c, "a", NULL), "quoted # not a comment"));
  CHECK("escapes in double quotes", is(uconf_str(&c, "b", NULL), "tab\tquote\" end"));
  CHECK("quotes inside a value are kept", is(uconf_str(&c, "c", NULL), "term -e sh -c \"echo hi\""));
  CHECK("two quoted strings are left as written", is(uconf_str(&c, "d", NULL), "\"a\" \"b\""));
  CHECK("single quotes", is(uconf_str(&c, "e", NULL), "single"));
  CHECK("a } in quotes does not close the block", is(uconf_str(&c, "blk.cmd", NULL), "echo }"));
  CHECK("no errors with quotes", c.errors == 0);
  uconf_free(&c);

  parse(&c, "[a.b]\nc.d = 1\n");
  CHECK("dotted header and dotted key", uconf_int(&c, "a.b.c.d", 0, 0, 9) == 1);
  uconf_free(&c);

  parse(&c, "n = 500\nbad = 12px\nyes = On\nno = false\nmaybe = perhaps\nc6 = #FE8019\nc3 = #fff\ncx = 0x00ff00\ncbad = #12\nf = 0.55\n");
  CHECK("out of range clamps", uconf_int(&c, "n", 0, 0, 100) == 100);
  CHECK("not a number gives the default", uconf_int(&c, "bad", 4, 0, 100) == 4);
  CHECK("bool On", uconf_bool(&c, "yes", 0) == 1);
  CHECK("bool false", uconf_bool(&c, "no", 1) == 0);
  CHECK("bool nonsense gives the default", uconf_bool(&c, "maybe", 1) == 1);
  CHECK("colour #RRGGBB", uconf_color(&c, "c6", 0) == 0xfe8019);
  CHECK("colour #rgb", uconf_color(&c, "c3", 0) == 0xffffff);
  CHECK("colour 0x", uconf_color(&c, "cx", 0) == 0x00ff00);
  CHECK("a bad colour gives the default", uconf_color(&c, "cbad", 7) == 7);
  CHECK("number", uconf_num(&c, "f", 0) > 0.549 && uconf_num(&c, "f", 0) < 0.551);
  CHECK("each bad value is reported", c.errors == 4);
  uconf_free(&c);

  parse(&c, "x = 1\nx = 2\n");
  CHECK("a repeated key: the last one wins", uconf_int(&c, "x", 0, 0, 9) == 2);
  uconf_free(&c);

  char b1[] = "ALT SHIFT , Q,  exec , sh -c \"a, b\"";
  char *f[4];
  int nf = uconf_split(b1, f, 4);
  CHECK("split: four fields", nf == 4);
  CHECK("split: blanks trimmed", is(f[0], "ALT SHIFT") && is(f[1], "Q") && is(f[2], "exec"));
  CHECK("split: the last field keeps its commas", is(f[3], "sh -c \"a, b\""));
  char b2[] = ", F1, kill";
  nf = uconf_split(b2, f, 4);
  CHECK("split: an empty first field", nf == 3 && is(f[0], "") && is(f[1], "F1") && is(f[2], "kill"));

  parse(&c, "[s]\nblock {\n  a = 1\n");
  CHECK("a missing } is reported", c.errors == 1);
  CHECK("and the settings before it still count", uconf_int(&c, "s.block.a", 0, 0, 9) == 1);
  uconf_free(&c);
  parse(&c, "}\n[bad name]\n= 3\nkey value\n[ok]\nk = 1\n");
  CHECK("stray }, bad header, no key, no = : four errors", c.errors == 4);
  CHECK("parsing goes on after mistakes", uconf_int(&c, "ok.k", 0, 0, 9) == 1);
  uconf_free(&c);
  parse(&c, "b {\n[s]\nk = 1\n");
  CHECK("a header inside braces is reported and closes them", c.errors == 1 && uconf_int(&c, "s.k", 0, 0, 9) == 1);
  uconf_free(&c);

  parse(&c, "[s]\nknown = 1\ntypo = 2\n");
  uconf_int(&c, "s.known", 0, 0, 9);
  int before = c.errors;
  uconf_warn_unused(&c);
  CHECK("unused settings are reported once each", c.errors == before + 1);
  uconf_free(&c);

  const char *path = "/tmp/uconftest.uconf";
  FILE *w = fopen(path, "w");
  if (w) { fputs("[file]\nvalue = from disk\n", w); fclose(w); }
  CHECK("load a file", uconf_load(&c, path) == 0 && is(uconf_str(&c, "file.value", NULL), "from disk"));
  CHECK("the file name is kept for messages", is(c.name, path));
  uconf_free(&c);
  CHECK("a missing file is -1", uconf_load(&c, "/tmp/no-such-file.uconf") == -1 && c.n == 0);
  uconf_free(&c);

  printf("uconftest: %d checks, %d failed\n", checks, fails);
  return fails != 0;
}
