// SPDX-License-Identifier: GPL-2.0-or-later
/* cc: a C compiler for UC.OS. It reads C and writes WebAssembly object files in the
 * tool-conventions "linking" format, which wasm-ld and our own ld accept.
 *
 * Pipeline: lex.c (characters to tokens), pp.c (preprocessor), parse.c (tokens to a
 * typed syntax tree), gen.c (tree to WebAssembly function bodies), wasm.c (the object
 * file), main.c (the command line). The compiler itself is plain C11 and builds both
 * with the host compiler and for UC.OS. */
#ifndef CC_H
#define CC_H
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int64_t i64;
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint8_t u8;

/* ---- util.c ---- */
void *xalloc(size_t n); /* zeroed, never freed (the compiler is short-lived) */
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xfmt(const char *fmt, ...);
char *read_file(const char *path, size_t *len); /* NULL if it cannot be read */
int file_exists(const char *path);

typedef struct { u8 *p; int len, cap; } Buf;
void buf_byte(Buf *b, int c);
void buf_mem(Buf *b, const void *p, int n);
void buf_str(Buf *b, const char *s);
void buf_uleb(Buf *b, u64 v);
void buf_sleb(Buf *b, i64 v);
void buf_uleb5(Buf *b, u32 v);  /* padded to 5 bytes, for relocated fields */
void buf_sleb5(Buf *b, int32_t v);
void buf_u32(Buf *b, u32 v);
void buf_name(Buf *b, const char *s); /* length-prefixed */
void put_uleb5(u8 *p, u32 v);
void put_sleb5(u8 *p, int32_t v);
void put_u32(u8 *p, u32 v);

typedef struct { void **p; int len, cap; } Vec;
void vec_push(Vec *v, void *x);

typedef struct { const char **keys; void **vals; int cap, used; } Map;
void *map_get(Map *m, const char *k);
void map_put(Map *m, const char *k, void *v);

/* ---- identifiers: interned; carry macro, keyword and scope bindings ---- */
typedef struct Ident {
  const char *name;
  int len;
  int kw;                 /* keyword id, 0 if none */
  struct Macro *macro;    /* preprocessor definition */
  struct Binding *sym;    /* innermost ordinary-namespace binding */
  struct Binding *tag;    /* innermost tag binding */
  struct Label *label;    /* label in the current function */
  struct Ident *next;
  unsigned hash;
} Ident;
Ident *intern(const char *s, int len);

/* ---- tokens ---- */
enum { T_EOF, T_IDENT, T_NUM, T_CHAR, T_STR, T_PUNCT, T_PLACEMARKER, T_OTHER };
enum {
  P_ARROW = 256, P_INC, P_DEC, P_SHL, P_SHR, P_LE, P_GE, P_EQ, P_NE, P_LAND, P_LOR,
  P_MULEQ, P_DIVEQ, P_MODEQ, P_ADDEQ, P_SUBEQ, P_SHLEQ, P_SHREQ, P_ANDEQ, P_XOREQ, P_OREQ,
  P_HASHHASH, P_ELLIPSIS
};

typedef struct File {
  char *name;      /* as given to #include, for diagnostics and __FILE__ */
  char *path;      /* path it was read from */
  char *data;      /* contents after line splicing, NUL terminated */
  int len;
  struct File *next;
  Ident *guard;    /* include guard macro, if the file has the classic form */
  int once;        /* #pragma once seen */
  int sysdir;      /* found in a system include directory */
  int dirlen;      /* length of the directory part of path (for "" includes) */
  int incdir;      /* index of the include directory it was found in, -1 if none */
} File;

typedef struct Hide { Ident *id; struct Hide *next; } Hide;

typedef struct Tok {
  u8 kind;
  u8 bol;          /* first token on its line */
  u8 space;        /* whitespace before it */
  u8 noexpand;     /* identifier that must never be expanded again (C 6.10.3.4p2) */
  int punct;       /* for T_PUNCT */
  const char *s;   /* spelling (not NUL terminated for T_IDENT: use id) */
  int len;
  Ident *id;       /* for T_IDENT */
  File *file;
  int line, col;
  Hide *hide;
  struct Tok *origin; /* where the macro that produced this token was invoked */
} Tok;

Tok *lex_file(File *f, int *ntok); /* array terminated by a T_EOF token */
File *new_file(const char *name, const char *path, char *data, int len);

/* ---- diagnostics ---- */
extern int nerrors, nwarnings, opt_werror, opt_wall, opt_w;
void error_at(Tok *t, const char *fmt, ...);   /* fatal: prints and exits */
void errorc_at(Tok *t, const char *fmt, ...);  /* counts, continues */
void warn_at(Tok *t, const char *fmt, ...);
void fatal(const char *fmt, ...);
void tick(void); /* a yield point for UC.OS time slicing */
char *tok_str(Tok *t);

/* ---- pp.c ---- */
void pp_init(void);
void pp_add_include(const char *dir, int sys);
void pp_define(const char *def);   /* "NAME" or "NAME=VALUE" */
void pp_undef(const char *name);
Tok **preprocess(const char *path, int *ntok); /* NULL-free array ending in T_EOF */
void pp_write(FILE *out, Tok **toks, int n);
extern Vec pp_deps;                 /* File* of every file read, for -M */

/* ---- types ---- */
enum {
  TY_VOID, TY_BOOL, TY_CHAR, TY_SCHAR, TY_UCHAR, TY_SHORT, TY_USHORT, TY_INT, TY_UINT,
  TY_LONG, TY_ULONG, TY_LLONG, TY_ULLONG, TY_FLOAT, TY_DOUBLE, TY_LDOUBLE,
  TY_PTR, TY_ARRAY, TY_FUNC, TY_STRUCT, TY_UNION
};
enum { Q_CONST = 1, Q_VOLATILE = 2, Q_RESTRICT = 4 };

typedef struct Member {
  struct Member *next;
  struct Type *ty;
  Ident *name;    /* NULL for an anonymous struct or union member, or an unnamed bit-field */
  Tok *tok;
  int offset;
  int bitoff, bitwidth; /* bitwidth 0: not a bit-field */
  int idx;
} Member;

typedef struct Rec {   /* shared by every qualified copy of a struct or union type */
  Member *members;
  int nmembers;
  int size, align;
  int complete, flexible, packed, is_union;
  Ident *tag;
  Member **marr;   /* members by index */
} Rec;

typedef struct Param { struct Type *ty; Ident *name; Tok *tok; } Param;

typedef struct Type {
  u8 kind, qual, is_enum, is_unsigned;
  int size, align;
  struct Type *base;    /* pointer target, array element, function result */
  int len;              /* array length, -1 if unknown */
  struct Node *vla_len; /* variable length array */
  struct Var *vla_size; /* runtime size in bytes of a VLA, set when declared */
  /* functions */
  Param *params;
  int nparams, variadic, proto;
  Rec *rec;
  Ident *tdname;        /* typedef name it came from, for messages */
} Type;

extern Type *ty_void, *ty_bool, *ty_char, *ty_schar, *ty_uchar, *ty_short, *ty_ushort, *ty_int,
    *ty_uint, *ty_long, *ty_ulong, *ty_llong, *ty_ullong, *ty_float, *ty_double, *ty_ldouble;
Type *ptr_to(Type *base);
Type *array_of(Type *base, int len);
Type *func_type(Type *ret);
Type *qualify(Type *t, int qual);
Type *unqual(Type *t);
void rec_track(Type *t);
void rec_completed(Rec *r);
int is_integer(Type *t);
int is_flonum(Type *t);
int is_arith(Type *t);
int is_scalar(Type *t);
int is_ptr(Type *t);      /* pointer (arrays decay before this is asked) */
int is_aggr(Type *t);     /* struct or union */
int is_memclass(Type *t); /* lives in memory and is handled by address: struct, union, long double */
int same_type(Type *a, Type *b);
char *type_str(Type *t);
Type *struct_field1(Type *t); /* single-element struct ABI: the scalar it passes as, or NULL */

/* ---- syntax tree ---- */
enum {
  /* expressions */
  N_NUM, N_FNUM, N_VAR, N_ADDR, N_DEREF, N_MEMBER, N_CAST, N_CALL, N_COMMA, N_COND,
  N_ASSIGN, N_OPASSIGN, N_PREINC, N_POSTINC,
  N_ADD, N_SUB, N_MUL, N_DIV, N_MOD, N_SHL, N_SHR, N_BAND, N_BOR, N_BXOR,
  N_EQ, N_NE, N_LT, N_LE, N_LAND, N_LOR, N_NOT, N_BNOT, N_NEG,
  N_STMTEXPR, N_INITVAR, N_BUILTIN, N_VAARG, N_ZERO, N_VLASIZE,
  /* statements */
  N_BLOCK, N_IF, N_WHILE, N_DO, N_FOR, N_SWITCH, N_CASE, N_BREAK, N_CONTINUE, N_GOTO,
  N_LABEL, N_RETURN, N_EXPR, N_DECL, N_NULL, N_VLADECL
};

typedef struct Node {
  u8 kind;
  u8 op;           /* N_OPASSIGN: the binary op kind */
  u8 flag;
  Type *ty;
  Tok *tok;
  struct Node *lhs, *rhs, *cond, *then, *els, *init, *inc, *body;
  struct Node *next;
  struct Var *var;
  Member *mem;
  struct Node **args;
  int nargs;
  Type *fty;       /* N_CALL: the function type called through; N_OPASSIGN, N_PREINC, N_POSTINC: the type the arithmetic is done in */
  i64 ival;        /* N_NUM, N_PREINC delta, N_CASE low */
  i64 ival2;       /* N_CASE high */
  double fval;
  int bid;         /* N_BUILTIN id */
  struct Label *label;
  struct Node *cases;     /* N_SWITCH: list of N_CASE through ->els */
  struct Node *defcase;
  int irregular;
  struct Init *initz;     /* N_INITVAR, N_DECL */
  u8 tgt;          /* statement contains a jump target (goto label, irregular case) */
  int s1, s2, s3;  /* state numbers when control flow is flattened (gen.c) */
} Node;

typedef struct Label {
  Ident *name; Tok *tok; int defined, used, id; Node *stmt;
} Label;

typedef struct Reloc { int off; int type; struct Sym *sym; i64 addend; } Reloc;

typedef struct Var {
  Ident *name;
  Type *ty;
  Tok *tok;
  int is_local, is_param, is_static, is_extern, is_def, is_tentative, is_inline, is_func;
  int addr_taken, referenced, emitted, noreturn, weak, is_tls, used_attr, has_extern_decl;
  int align;
  /* code generation */
  int in_mem, local, frame_off, param_idx;
  struct Sym *sym;
  /* initialised data (globals) */
  u8 *data;
  int datalen;
  Vec relocs;      /* Reloc* into data */
  int is_const;    /* goes to .rodata */
  /* functions */
  Node *body;
  Vec locals;      /* Var* of every local of the function */
  Vec params;      /* Var* of the parameters, in order */
  int has_goto, has_vla, uses_va;
  struct Var *va_area;
  const char *import_module, *import_name, *export_name, *section, *asmname;
  int vis_default;
  struct Var *next;
} Var;

typedef struct Init {
  Type *ty;
  Node *expr;
  struct Init **kids;
  int nkids;
  Member *mem;      /* for a union: the member chosen */
  int max;          /* arrays: highest index initialised + 1 */
} Init;

/* builtins that reach code generation */
enum {
  BI_NONE, BI_VA_START, BI_VA_END, BI_VA_COPY, BI_UNREACHABLE, BI_TRAP, BI_ALLOCA,
  BI_CLZ, BI_CTZ, BI_POPCOUNT, BI_BSWAP16, BI_BSWAP32, BI_BSWAP64,
  BI_SQRT, BI_FLOOR, BI_CEIL, BI_TRUNC, BI_FABS, BI_RINT, BI_COPYSIGN,
  BI_ISNAN, BI_ISINF, BI_ISFINITE, BI_ISNORMAL, BI_SIGNBIT, BI_FPCLASSIFY, BI_ISLESSGREATER, BI_ISUNORDERED,
  BI_MEMCPY, BI_MEMMOVE, BI_MEMSET, BI_MEMORY_SIZE, BI_MEMORY_GROW, BI_FRAME_ADDRESS
};

/* ---- parse.c ---- */
extern Vec globals;  /* Var* in definition order */
void parse(Tok **toks, int ntok);
i64 eval_int(Node *n, int *ok);

/* ---- object symbols (wasm.c) ---- */
enum { SYM_FUNC, SYM_DATA, SYM_GLOBAL, SYM_SECTION, SYM_EVENT, SYM_TABLE };
typedef struct Sym {
  int kind;
  const char *name;
  int flags;         /* WASM_SYM_* */
  int defined;
  int index;         /* final symbol index */
  int fidx;          /* function or global index */
  int type;          /* function type index */
  Var *var;
  /* data */
  int seg, size, align;
  u32 addr;
  /* function body */
  Buf code;
  Vec relocs;
  const char *imp_module, *imp_name, *exp_name;
} Sym;
#define SF_WEAK 1
#define SF_LOCAL 2
#define SF_HIDDEN 4
#define SF_UNDEF 0x10
#define SF_EXPORTED 0x20
#define SF_EXPLICIT 0x40
#define SF_NOSTRIP 0x80

enum {
  R_FUNCTION_INDEX_LEB = 0, R_TABLE_INDEX_SLEB = 1, R_TABLE_INDEX_I32 = 2, R_MEMORY_ADDR_LEB = 3,
  R_MEMORY_ADDR_SLEB = 4, R_MEMORY_ADDR_I32 = 5, R_TYPE_INDEX_LEB = 6, R_GLOBAL_INDEX_LEB = 7,
  R_TABLE_NUMBER_LEB = 20
};

int wasm_functype(const u8 *sig, int nparams, int nresults); /* params then results */
Sym *sym_func(Var *v);
Sym *sym_data(Var *v);
Sym *sym_stack_pointer(void);
Sym *sym_table(void);
Sym *sym_extern_func(const char *name, const u8 *sig, int np, int nr);
void wasm_write(const char *path);

/* ---- gen.c ---- */
void gen_program(void);
void func_sig(Type *fty, u8 *sig, int *np, int *nr);

/* ---- options ---- */
extern int opt_O, opt_g;
extern const char *cur_file_name;
#endif
