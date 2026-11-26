// SPDX-License-Identifier: GPL-2.0-or-later
/* -S: a text listing of the object. It is for reading, not for assembling (there is
 * no assembler): each function with its signature and its code in WebAssembly text
 * mnemonics where the opcode is known. */
#include "cc.h"

void wasm_write(const char *path);

void write_listing(const char *path) {
  /* write the object to a temporary buffer file, then describe it */
  char tmp[64];
  snprintf(tmp, sizeof tmp, "/tmp/cc-listing-%d.o", (int)rand());
  wasm_write(tmp);
  size_t len;
  char *data = read_file(tmp, &len);
  remove(tmp);
  FILE *f = path && strcmp(path, "-") ? fopen(path, "w") : stdout;
  if (!f) fatal("cannot write '%s'", path);
  fprintf(f, "; UC.OS cc listing of %s (%zu bytes of object code)\n", cur_file_name, len);
  extern int listing_dump(FILE *f, const u8 *p, size_t n);
  listing_dump(f, (const u8 *)data, len);
  if (f != stdout) fclose(f);
}

static u64 rd_uleb(const u8 **p) {
  u64 v = 0;
  int sh = 0;
  for (;;) {
    u8 c = *(*p)++;
    if (sh < 64) v |= (u64)(c & 0x7f) << sh;
    sh += 7;
    if (!(c & 0x80)) return v;
  }
}
static i64 rd_sleb(const u8 **p) {
  i64 v = 0;
  int sh = 0;
  u8 c;
  do {
    c = *(*p)++;
    if (sh < 64) v |= (i64)((u64)(c & 0x7f) << sh);
    sh += 7;
  } while (c & 0x80);
  if (sh < 64 && (c & 0x40)) v |= (i64)(~0ull << sh);
  return v;
}

static const char *opname(int o) {
  static const char *n[256];
  if (!n[0x0b]) {
    const char *names[][2] = {
      {"\x00", "unreachable"}, {"\x01", "nop"}, {"\x02", "block"}, {"\x03", "loop"}, {"\x04", "if"}, {"\x05", "else"},
      {"\x0b", "end"}, {"\x0c", "br"}, {"\x0d", "br_if"}, {"\x0e", "br_table"}, {"\x0f", "return"}, {"\x10", "call"},
      {"\x11", "call_indirect"}, {"\x1a", "drop"}, {"\x1b", "select"}, {"\x20", "local.get"}, {"\x21", "local.set"},
      {"\x22", "local.tee"}, {"\x23", "global.get"}, {"\x24", "global.set"}, {"\x28", "i32.load"}, {"\x29", "i64.load"},
      {"\x2a", "f32.load"}, {"\x2b", "f64.load"}, {"\x2c", "i32.load8_s"}, {"\x2d", "i32.load8_u"}, {"\x2e", "i32.load16_s"},
      {"\x2f", "i32.load16_u"}, {"\x36", "i32.store"}, {"\x37", "i64.store"}, {"\x38", "f32.store"}, {"\x39", "f64.store"},
      {"\x3a", "i32.store8"}, {"\x3b", "i32.store16"}, {"\x3f", "memory.size"}, {"\x40", "memory.grow"},
      {"\x41", "i32.const"}, {"\x42", "i64.const"}, {"\x43", "f32.const"}, {"\x44", "f64.const"},
      {"\x45", "i32.eqz"}, {"\x46", "i32.eq"}, {"\x47", "i32.ne"}, {"\x48", "i32.lt_s"}, {"\x49", "i32.lt_u"},
      {"\x4a", "i32.gt_s"}, {"\x4b", "i32.gt_u"}, {"\x4c", "i32.le_s"}, {"\x4d", "i32.le_u"}, {"\x4e", "i32.ge_s"},
      {"\x4f", "i32.ge_u"}, {"\x50", "i64.eqz"}, {"\x51", "i64.eq"}, {"\x52", "i64.ne"}, {"\x53", "i64.lt_s"},
      {"\x54", "i64.lt_u"}, {"\x57", "i64.le_s"}, {"\x58", "i64.le_u"}, {"\x5b", "f32.eq"}, {"\x5c", "f32.ne"},
      {"\x5d", "f32.lt"}, {"\x5e", "f32.gt"}, {"\x5f", "f32.le"}, {"\x60", "f32.ge"}, {"\x61", "f64.eq"}, {"\x62", "f64.ne"},
      {"\x63", "f64.lt"}, {"\x64", "f64.gt"}, {"\x65", "f64.le"}, {"\x66", "f64.ge"}, {"\x67", "i32.clz"}, {"\x68", "i32.ctz"},
      {"\x69", "i32.popcnt"}, {"\x6a", "i32.add"}, {"\x6b", "i32.sub"}, {"\x6c", "i32.mul"}, {"\x6d", "i32.div_s"},
      {"\x6e", "i32.div_u"}, {"\x6f", "i32.rem_s"}, {"\x70", "i32.rem_u"}, {"\x71", "i32.and"}, {"\x72", "i32.or"},
      {"\x73", "i32.xor"}, {"\x74", "i32.shl"}, {"\x75", "i32.shr_s"}, {"\x76", "i32.shr_u"}, {"\x79", "i64.clz"},
      {"\x7a", "i64.ctz"}, {"\x7b", "i64.popcnt"}, {"\x7c", "i64.add"}, {"\x7d", "i64.sub"}, {"\x7e", "i64.mul"},
      {"\x7f", "i64.div_s"}, {"\x80", "i64.div_u"}, {"\x81", "i64.rem_s"}, {"\x82", "i64.rem_u"}, {"\x83", "i64.and"},
      {"\x84", "i64.or"}, {"\x85", "i64.xor"}, {"\x86", "i64.shl"}, {"\x87", "i64.shr_s"}, {"\x88", "i64.shr_u"},
      {"\x8b", "f32.abs"}, {"\x8c", "f32.neg"}, {"\x8d", "f32.ceil"}, {"\x8e", "f32.floor"}, {"\x8f", "f32.trunc"},
      {"\x90", "f32.nearest"}, {"\x91", "f32.sqrt"}, {"\x92", "f32.add"}, {"\x93", "f32.sub"}, {"\x94", "f32.mul"},
      {"\x95", "f32.div"}, {"\x98", "f32.copysign"}, {"\x99", "f64.abs"}, {"\x9a", "f64.neg"}, {"\x9b", "f64.ceil"},
      {"\x9c", "f64.floor"}, {"\x9d", "f64.trunc"}, {"\x9e", "f64.nearest"}, {"\x9f", "f64.sqrt"}, {"\xa0", "f64.add"},
      {"\xa1", "f64.sub"}, {"\xa2", "f64.mul"}, {"\xa3", "f64.div"}, {"\xa6", "f64.copysign"}, {"\xa7", "i32.wrap_i64"},
      {"\xac", "i64.extend_i32_s"}, {"\xad", "i64.extend_i32_u"}, {"\xb2", "f32.convert_i32_s"},
      {"\xb3", "f32.convert_i32_u"}, {"\xb4", "f32.convert_i64_s"}, {"\xb5", "f32.convert_i64_u"},
      {"\xb6", "f32.demote_f64"}, {"\xb7", "f64.convert_i32_s"}, {"\xb8", "f64.convert_i32_u"},
      {"\xb9", "f64.convert_i64_s"}, {"\xba", "f64.convert_i64_u"}, {"\xbb", "f64.promote_f32"},
      {"\xbc", "i32.reinterpret_f32"}, {"\xbd", "i64.reinterpret_f64"}, {"\xbe", "f32.reinterpret_i32"},
      {"\xbf", "f64.reinterpret_i64"}, {"\xc0", "i32.extend8_s"}, {"\xc1", "i32.extend16_s"},
    };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) n[(u8)names[i][0][0]] = names[i][1];
  }
  return n[o];
}

int listing_dump(FILE *f, const u8 *p, size_t n) {
  const u8 *end = p + n;
  p += 8;
  int nimpf = 0;
  while (p < end) {
    int id = *p++;
    u64 sz = rd_uleb(&p);
    const u8 *s = p, *se = p + sz;
    if (id == 2) {
      u64 cnt = rd_uleb(&s);
      for (u64 i = 0; i < cnt; i++) {
        u64 l = rd_uleb(&s); const u8 *m = s; s += l;
        u64 l2 = rd_uleb(&s); const u8 *nm = s; s += l2;
        int kind = *s++;
        fprintf(f, "; import %.*s.%.*s", (int)l, m, (int)l2, nm);
        if (kind == 0) { fprintf(f, " function, type %d\n", (int)rd_uleb(&s)); nimpf++; }
        else if (kind == 1) { s++; int fl = *s++; rd_uleb(&s); if (fl & 1) rd_uleb(&s); fputs(" table\n", f); }
        else if (kind == 2) { int fl = *s++; rd_uleb(&s); if (fl & 1) rd_uleb(&s); fputs(" memory\n", f); }
        else { s += 2; fputs(" global\n", f); }
      }
    }
    if (id == 10) {
      u64 cnt = rd_uleb(&s);
      for (u64 i = 0; i < cnt; i++) {
        u64 bl = rd_uleb(&s);
        const u8 *b = s, *be = s + bl;
        fprintf(f, "\nfunction %d:\n", (int)(nimpf + i));
        u64 nl = rd_uleb(&b);
        for (u64 j = 0; j < nl; j++) { u64 c = rd_uleb(&b); int t = *b++; fprintf(f, "  .local %d x %s\n", (int)c, t == 0x7f ? "i32" : t == 0x7e ? "i64" : t == 0x7d ? "f32" : "f64"); }
        int ind = 1;
        while (b < be) {
          int o = *b++;
          if (o == 0x0b || o == 0x05) ind--;
          fprintf(f, "%*s", 2 * ind + 2, "");
          const char *nm = opname(o);
          if (o == 0xfc) {
            u64 sub = rd_uleb(&b);
            if (sub < 8) fprintf(f, "trunc_sat.%d", (int)sub);
            else if (sub == 10) { fputs("memory.copy", f); b += 2; }
            else if (sub == 11) { fputs("memory.fill", f); b++; }
            else fprintf(f, "0xfc %d", (int)sub);
          } else if (!nm) {
            fprintf(f, "0x%02x", o);
          } else {
            fputs(nm, f);
            if (o == 0x02 || o == 0x03 || o == 0x04) { int bt = *b++; if (bt != 0x40) fprintf(f, " (result 0x%02x)", bt); ind++; }
            else if (o == 0x05) ind++;
            else if (o == 0x0c || o == 0x0d || (o >= 0x20 && o <= 0x24) || o == 0x10) fprintf(f, " %d", (int)rd_uleb(&b));
            else if (o == 0x11) { int t = (int)rd_uleb(&b); rd_uleb(&b); fprintf(f, " (type %d)", t); }
            else if (o == 0x0e) { u64 c = rd_uleb(&b); for (u64 k = 0; k <= c; k++) fprintf(f, " %d", (int)rd_uleb(&b)); }
            else if (o >= 0x28 && o <= 0x3e) { int al = (int)rd_uleb(&b); fprintf(f, " offset=%d align=%d", (int)rd_uleb(&b), 1 << al); }
            else if (o == 0x3f || o == 0x40) b++;
            else if (o == 0x41 || o == 0x42) fprintf(f, " %lld", (long long)rd_sleb(&b));
            else if (o == 0x43) { float x; memcpy(&x, b, 4); b += 4; fprintf(f, " %g", x); }
            else if (o == 0x44) { double x; memcpy(&x, b, 8); b += 8; fprintf(f, " %g", x); }
          }
          fputc('\n', f);
        }
        s = be;
      }
    }
    p = se;
  }
  return 0;
}
