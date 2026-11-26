// SPDX-License-Identifier: GPL-2.0-or-later
/* The cc command line: gcc-style options, one translation unit per process, and the
 * link step done by running ld. */
#include "cc.h"
#include <sys/wait.h>
#include <unistd.h>
#ifndef __wasm__
#include <spawn.h>
extern char **environ;
#endif

int opt_O, opt_g;
const char *cur_file_name;

static const char *sysroot = "";
static int opt_c, opt_S, opt_E, opt_v, nostdinc, nostdlib, opt_M, opt_MM, opt_MD, opt_MMD, opt_MP, opt_shared;
static const char *opt_o, *opt_MF, *opt_MT, *opt_x, *ld_path;
static Vec inputs, ldargs, incs, sysincs, defs;
static const char *self_path;
static int tmp_count;

static void usage(void) {
  fputs("usage: cc [options] file...\n"
        "  -c            compile to an object file, do not link\n"
        "  -E            preprocess only\n"
        "  -S            write a text listing of the generated code\n"
        "  -o FILE       output file\n"
        "  -I DIR, -isystem DIR, -D NAME[=VAL], -U NAME\n"
        "  -L DIR, -l NAME, -Wl,ARGS\n"
        "  -M -MM -MD -MMD -MF FILE -MT TARGET -MP   dependency output for make\n"
        "  -O0..-O3, -g, -Wall, -Werror, -w, -std=..., -x c\n"
        "  --sysroot=DIR  look for /usr/include and /usr/lib under DIR\n", stderr);
}

static int run(char **argv) {
  if (opt_v) {
    for (int i = 0; argv[i]; i++) fprintf(stderr, "%s%s", i ? " " : "", argv[i]);
    fputc('\n', stderr);
  }
  int pid, status;
#ifdef __wasm__
  if (strchr(argv[0], '/')) pid = spawn(argv[0], argv, environ, NULL);
  else pid = spawnp(argv[0], argv, NULL);
  if (pid < 0) { fprintf(stderr, "cc: cannot run '%s'\n", argv[0]); return 1; }
#else
  if (posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ)) { fprintf(stderr, "cc: cannot run '%s'\n", argv[0]); return 1; }
#endif
  if (waitpid(pid, &status, 0) < 0) return 1;
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  fprintf(stderr, "cc: '%s' was killed by signal %d\n", argv[0], WTERMSIG(status));
  return 1;
}

static const char *ext(const char *p) {
  const char *d = strrchr(p, '.'), *s = strrchr(p, '/');
  if (!d || (s && d < s)) return "";
  return d + 1;
}
static char *replace_ext(const char *p, const char *e) {
  const char *s = strrchr(p, '/');
  const char *base = s ? s + 1 : p;
  const char *d = strrchr(base, '.');
  int n = d ? (int)(d - base) : (int)strlen(base);
  return xfmt("%.*s.%s", n, base, e);
}
static char *tmp_name(const char *e) { return xfmt("/tmp/cc-%d-%d.%s", (int)getpid(), tmp_count++, e); }

static void write_deps(const char *src, const char *obj) {
  FILE *f = stdout;
  const char *path = opt_MF;
  if (!path && (opt_MD || opt_MMD)) path = opt_o && opt_c ? replace_ext(opt_o, "d") : replace_ext(src, "d");
  if (path && strcmp(path, "-")) {
    if (opt_MD || opt_MMD) {
      /* -MD with -o dir/x.o writes dir/x.d */
      if (!opt_MF && opt_o && opt_c) {
        const char *s = strrchr(opt_o, '/');
        if (s) path = xfmt("%.*s/%s", (int)(s - opt_o), opt_o, replace_ext(opt_o, "d"));
      }
    }
    f = fopen(path, "w");
    if (!f) fatal("cannot write '%s'", path);
  }
  int skipsys = opt_MM || opt_MMD;
  const char *target = opt_MT ? opt_MT : obj;
  int col = fprintf(f, "%s:", target);
  for (int i = 0; i < pp_deps.len; i++) {
    File *d = pp_deps.p[i];
    if (skipsys && d->sysdir) continue;
    int l = (int)strlen(d->path);
    if (col + l > 76) { fputs(" \\\n ", f); col = 1; }
    fprintf(f, " %s", d->path);
    col += l + 1;
  }
  fputc('\n', f);
  if (opt_MP)
    for (int i = 1; i < pp_deps.len; i++) {
      File *d = pp_deps.p[i];
      if (skipsys && d->sysdir) continue;
      fprintf(f, "\n%s:\n", d->path);
    }
  if (f != stdout) fclose(f);
}

void write_listing(const char *path); /* listing.c */

/* Compile one C file in this process. */
static int compile_one(const char *src, const char *out) {
  cur_file_name = src;
  pp_init();
  for (int i = 0; i < defs.len; i++) {
    const char *d = defs.p[i];
    if (d[0] == 'D') pp_define(d + 1);
    else pp_undef(d + 1);
  }
  for (int i = 0; i < incs.len; i++) pp_add_include(incs.p[i], 0);
  for (int i = 0; i < sysincs.len; i++) pp_add_include(sysincs.p[i], 1);
  if (!nostdinc) {
    pp_add_include(xfmt("%s/usr/include", sysroot), 1);
    pp_add_include(xfmt("%s/usr/lib/cc/include", sysroot), 1);
  }
  int ntok;
  Tok **toks = preprocess(src, &ntok);
  const char *objname = opt_MT ? opt_MT : opt_o && opt_c ? opt_o : replace_ext(src, "o");
  if (opt_M || opt_MM) { write_deps(src, objname); return 0; }
  if (opt_E) {
    FILE *f = out && strcmp(out, "-") ? fopen(out, "w") : stdout;
    if (!f) fatal("cannot write '%s'", out);
    pp_write(f, toks, ntok);
    if (f != stdout) fclose(f);
    return 0;
  }
  if (opt_MD || opt_MMD) write_deps(src, objname);
  parse(toks, ntok);
  gen_program();
  if (nerrors) return 1;
  if (opt_S) write_listing(out);
  else wasm_write(out);
  return nerrors ? 1 : 0;
}

/* Run this compiler again for another C file (the compiler keeps global state, so
 * each translation unit gets its own process). */
static int compile_child(const char *src, const char *out, int argc, char **argv) {
  Vec a = {0};
  vec_push(&a, (void *)self_path);
  for (int i = 1; i < argc; i++) {
    const char *s = argv[i];
    if (!strcmp(s, "-o")) { i++; continue; }
    if (!strcmp(s, "-c") || !strcmp(s, "-S") || !strcmp(s, "-E")) continue;
    int is_input = 0;
    for (int j = 0; j < inputs.len; j++) if (inputs.p[j] == s) is_input = 1;
    if (is_input) continue;
    vec_push(&a, (void *)s);
  }
  vec_push(&a, opt_S ? "-S" : "-c");
  vec_push(&a, (void *)src);
  vec_push(&a, "-o");
  vec_push(&a, (void *)out);
  vec_push(&a, NULL);
  return run((char **)a.p);
}

static const char *find_ld(void) {
  if (ld_path) return ld_path;
  const char *s = strrchr(self_path, '/');
  if (s) {
    char *p = xfmt("%.*s/ld", (int)(s - self_path), self_path);
    if (file_exists(p)) return p;
  }
  return "ld";
}

int main(int argc, char **argv) {
  self_path = argv[0];
  if (argc < 2) { usage(); return 1; }
  for (int i = 1; i < argc; i++) {
    char *a = argv[i];
#define ARG() (a[2] ? a + 2 : (++i < argc ? argv[i] : (fatal("missing argument to '%s'", a), "")))
    if (a[0] != '-' || !strcmp(a, "-")) { vec_push(&inputs, a); continue; }
    if (!strcmp(a, "-c")) opt_c = 1;
    else if (!strcmp(a, "-S")) opt_S = 1;
    else if (!strcmp(a, "-E")) opt_E = 1;
    else if (!strcmp(a, "-v")) opt_v = 1;
    else if (!strcmp(a, "--version")) { puts("cc (UC.OS C compiler) 0.1, target wasm32-ucos"); return 0; }
    else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(); return 0; }
    else if (!strncmp(a, "-o", 2)) opt_o = ARG();
    else if (!strncmp(a, "-I", 2)) vec_push(&incs, ARG());
    else if (!strcmp(a, "-isystem")) { if (++i >= argc) fatal("missing argument to -isystem"); vec_push(&sysincs, argv[i]); }
    else if (!strcmp(a, "-iquote")) { if (++i >= argc) fatal("missing argument to -iquote"); vec_push(&incs, argv[i]); }
    else if (!strncmp(a, "-D", 2)) vec_push(&defs, xfmt("D%s", ARG()));
    else if (!strncmp(a, "-U", 2)) vec_push(&defs, xfmt("U%s", ARG()));
    else if (!strncmp(a, "-L", 2)) { vec_push(&ldargs, "-L"); vec_push(&ldargs, ARG()); }
    else if (!strncmp(a, "-l", 2)) vec_push(&ldargs, xfmt("-l%s", ARG()));
    else if (!strncmp(a, "-Wl,", 4)) {
      char *s = xstrdup(a + 4);
      for (char *t = strtok(s, ","); t; t = strtok(NULL, ",")) vec_push(&ldargs, t);
    }
    else if (!strcmp(a, "-Xlinker")) { if (++i >= argc) fatal("missing argument to -Xlinker"); vec_push(&ldargs, argv[i]); }
    else if (!strncmp(a, "-O", 2)) opt_O = a[2] == 's' || a[2] == 'z' ? 2 : a[2] ? a[2] - '0' : 1;
    else if (!strncmp(a, "-g", 2)) opt_g = 1;
    else if (!strcmp(a, "-Wall") || !strcmp(a, "-Wextra")) opt_wall = 1;
    else if (!strcmp(a, "-Werror")) opt_werror = 1;
    else if (!strcmp(a, "-w")) opt_w = 1;
    else if (!strncmp(a, "-W", 2)) ; /* other warning switches are accepted */
    else if (!strncmp(a, "-std=", 5)) {
      const char *s = a + 5;
      if (strstr(s, "++")) fatal("C++ is not supported");
    }
    else if (!strcmp(a, "-ansi") || !strcmp(a, "-pedantic") || !strcmp(a, "-pedantic-errors")) ;
    else if (!strncmp(a, "-x", 2)) {
      opt_x = ARG();
      if (strcmp(opt_x, "c") && strcmp(opt_x, "none") && strcmp(opt_x, "c-header")) fatal("language '%s' is not supported", opt_x);
    }
    else if (!strcmp(a, "-shared") || !strcmp(a, "-fPIC") || !strcmp(a, "-fpic")) {
      if (!strcmp(a, "-shared")) opt_shared = 1;
    }
    else if (!strcmp(a, "-M")) opt_M = 1;
    else if (!strcmp(a, "-MM")) opt_MM = 1;
    else if (!strcmp(a, "-MD")) opt_MD = 1;
    else if (!strcmp(a, "-MMD")) opt_MMD = 1;
    else if (!strcmp(a, "-MP")) opt_MP = 1;
    else if (!strcmp(a, "-MF")) { if (++i >= argc) fatal("missing argument to -MF"); opt_MF = argv[i]; }
    else if (!strcmp(a, "-MT") || !strcmp(a, "-MQ")) { if (++i >= argc) fatal("missing argument to %s", a); opt_MT = argv[i]; }
    else if (!strncmp(a, "--sysroot=", 10)) sysroot = a + 10;
    else if (!strcmp(a, "--sysroot")) { if (++i >= argc) fatal("missing argument to --sysroot"); sysroot = argv[i]; }
    else if (!strncmp(a, "--ld=", 5)) ld_path = a + 5;
    else if (!strcmp(a, "-nostdinc")) nostdinc = 1;
    else if (!strcmp(a, "-nostdlib") || !strcmp(a, "-nodefaultlibs") || !strcmp(a, "-nostartfiles")) nostdlib = 1;
    else if (!strcmp(a, "-static") || !strcmp(a, "-pipe") || !strcmp(a, "-rdynamic") || !strcmp(a, "-s")) ;
    else if (!strcmp(a, "-pthread")) fatal("threads are not supported on UC.OS");
    else if (!strncmp(a, "-f", 2) || !strncmp(a, "-m", 2)) ; /* code generation switches: accepted, ignored */
    else if (!strcmp(a, "-include")) fatal("-include is not supported");
    else { fprintf(stderr, "cc: unknown option '%s'\n", a); return 1; }
#undef ARG
  }
  if (opt_shared) fatal("-shared: UC.OS has no shared libraries; every program is linked statically");
  if (!inputs.len) fatal("no input files");
  Vec csrc = {0}, objs = {0};
  for (int i = 0; i < inputs.len; i++) {
    const char *in = inputs.p[i];
    const char *e = ext(in);
    if (!strcmp(e, "c") || !strcmp(e, "h") || !strcmp(in, "-") || (opt_x && strcmp(opt_x, "none"))) vec_push(&csrc, (void *)in);
    else if (!strcmp(e, "o") || !strcmp(e, "a")) vec_push(&objs, (void *)in);
    else if (!strcmp(e, "s") || !strcmp(e, "S")) fatal("'%s': there is no assembler; WebAssembly text is not supported", in);
    else fatal("'%s': unknown file type", in);
  }
  int only_compile = opt_c || opt_S || opt_E || opt_M || opt_MM;
  if (only_compile) {
    if (objs.len && !opt_E) warn_at(NULL, "linker input files are unused with -c, -S or -E");
    if (opt_o && csrc.len > 1 && !opt_E) fatal("cannot specify -o with -c or -S and several files");
    int rc = 0;
    for (int i = 0; i < csrc.len; i++) {
      const char *src = csrc.p[i];
      const char *out = opt_o ? opt_o : opt_E ? "-" : replace_ext(src, opt_S ? "s" : "o");
      if (i == 0) rc |= compile_one(src, out);
      else rc |= compile_child(src, out, argc, argv);
    }
    return rc ? 1 : 0;
  }
  /* compile and link */
  Vec tmps = {0};
  Vec lobjs = {0};
  int rc = 0;
  for (int i = 0; i < inputs.len; i++) {
    const char *in = inputs.p[i];
    int is_c = 0;
    for (int j = 0; j < csrc.len; j++) if (csrc.p[j] == in) is_c = 1;
    if (!is_c) { vec_push(&lobjs, (void *)in); continue; }
    char *o = tmp_name("o");
    vec_push(&tmps, o);
    vec_push(&lobjs, o);
    if (tmps.len == 1) {
      int r = compile_one(in, o);
      if (r) { rc = r; break; }
    } else {
      int r = compile_child(in, o, argc, argv);
      if (r) { rc = r; break; }
    }
  }
  if (!rc) {
    Vec a = {0};
    vec_push(&a, (void *)find_ld());
    vec_push(&a, "-o");
    vec_push(&a, (void *)(opt_o ? opt_o : "a.out"));
    if (!nostdlib) vec_push(&a, xfmt("--crt0=%s/usr/lib/crt0_2.o,%s/usr/lib/crt0_3.o", sysroot, sysroot));
    for (int i = 0; i < lobjs.len; i++) vec_push(&a, lobjs.p[i]);
    for (int i = 0; i < ldargs.len; i++) vec_push(&a, ldargs.p[i]);
    if (!nostdlib) {
      vec_push(&a, "-L");
      vec_push(&a, xfmt("%s/usr/lib", sysroot));
      vec_push(&a, "-lc");
    }
    vec_push(&a, NULL);
    rc = run((char **)a.p);
  }
  for (int i = 0; i < tmps.len; i++) remove(tmps.p[i]);
  return rc ? 1 : 0;
}
