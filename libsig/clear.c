// SPDX-License-Identifier: GPL-2.0-or-later
/* ASCII armor and cleartext signed messages (RFC 9580 sections 6 and 7,
 * RFC 4880 sections 6 and 7).
 *
 * The cleartext parser is strict on purpose. apt has had real bugs where the
 * text it parsed was not the text gpgv had checked: content before the
 * "BEGIN PGP SIGNED MESSAGE" line or after the signature (CVE-2013-1051 era
 * InRelease handling), a file split differently by apt and by gpgv
 * (CVE-2016-1252), and unsigned files that only looked signed. Here one parser
 * produces both the bytes that are hashed and the bytes the caller may read,
 * and the input is refused unless it is exactly
 *     "-----BEGIN PGP SIGNED MESSAGE-----"
 *     Hash headers (only "Hash:"; at least none, any number)
 *     one empty line
 *     the dash-escaped text
 *     "-----BEGIN PGP SIGNATURE-----" ... "-----END PGP SIGNATURE-----"
 *     nothing after it but empty lines.
 * Also refused: NUL bytes anywhere, a bare CR inside a line, a line of the
 * text that starts with "-" without being dash-escaped (except the signature
 * line itself), a line longer than SIG_MAX_LINE bytes, an unknown hash name.
 *
 * Canonical text (what is hashed): every line with its trailing spaces and
 * tabs removed, lines joined with CR LF, no line ending after the last line.
 * RFC 4880 7.1 and RFC 9580 7.2 both say trailing spaces and tabs; RFC 4880
 * 7.1 adds that this happens when the signature is made, RFC 9580 that the
 * verifier must do the same. GnuPG strips spaces, tabs and the CR of a CR LF
 * ending. This code does exactly that: " \t" before the line ending, with CR
 * LF and LF both accepted as line endings. */
#include "sig_int.h"

#define SIG_MAX_LINE 16384

unsigned sig_crc24(const void *p, size_t n) {
  const uint8_t *b = p;
  uint32_t crc = 0xb704ce;
  for (size_t i = 0; i < n; i++) {
    crc ^= (uint32_t)b[i] << 16;
    for (int k = 0; k < 8; k++) { crc <<= 1; if (crc & 0x1000000) crc ^= 0x1864cfb; }
  }
  return crc & 0xffffff;
}

/* ---------- lines ---------- */
typedef struct { const char *p; size_t n; size_t next; int has_nl; } line;
/* the line starting at pos, without its LF and without a CR right before it */
static int get_line(const char *t, size_t n, size_t pos, line *l) {
  if (pos >= n) return 0;
  const char *nl = memchr(t + pos, '\n', n - pos);
  size_t end = nl ? (size_t)(nl - t) : n;
  l->p = t + pos;
  l->n = end - pos;
  l->has_nl = nl != 0;
  l->next = nl ? end + 1 : n;
  if (l->n && l->p[l->n - 1] == '\r') l->n--;
  return 1;
}
static size_t trim_len(const char *p, size_t n) { while (n && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--; return n; }
static int line_is(const line *l, const char *s) {
  size_t k = strlen(s), n = trim_len(l->p, l->n);
  return n == k && !memcmp(l->p, s, k);
}
static int b64v(int c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

/* ---------- armor ---------- */
long sig_armor_decode(const char *t, size_t n, size_t *pos, const char *kind, unsigned char **out, char *err, size_t errlen) {
  *out = 0;
  char begin[64], end[64];
  size_t kl = strlen(kind);
  if (kl > 40) { sig_errf(err, errlen, "bad armor kind"); return -1; }
  memcpy(begin, "-----BEGIN ", 11); memcpy(begin + 11, kind, kl); memcpy(begin + 11 + kl, "-----", 6);
  memcpy(end, "-----END ", 9); memcpy(end + 9, kind, kl); memcpy(end + 9 + kl, "-----", 6);
  line l;
  size_t p = *pos;
  for (;;) {
    if (!get_line(t, n, p, &l)) return 0;
    p = l.next;
    if (line_is(&l, begin)) break;
  }
  /* headers up to an empty line */
  for (;;) {
    if (!get_line(t, n, p, &l)) { sig_errf(err, errlen, "armor ends inside its headers"); return -1; }
    p = l.next;
    if (trim_len(l.p, l.n) == 0) break;
    if (!memchr(l.p, ':', l.n) || l.n > SIG_MAX_LINE) { sig_errf(err, errlen, "armor header without ':' (or missing empty line after the headers)"); return -1; }
  }
  size_t cap = (n - p) / 4 * 3 + 3, o = 0;
  unsigned char *buf = malloc(cap ? cap : 1);
  if (!buf) { sig_errf(err, errlen, "out of memory"); return -1; }
  uint32_t acc = 0;
  int q = 0, pad = 0, have_crc = 0;
  uint32_t crc = 0;
  for (;;) {
    if (!get_line(t, n, p, &l)) { free(buf); sig_errf(err, errlen, "armor has no END line"); return -1; }
    p = l.next;
    size_t ln = trim_len(l.p, l.n);
    if (ln >= 5 && !memcmp(l.p, "-----", 5)) {
      if (!line_is(&l, end)) { free(buf); sig_errf(err, errlen, "armor END line does not match its BEGIN line"); return -1; }
      break;
    }
    if (have_crc) { free(buf); sig_errf(err, errlen, "data after the armor checksum"); return -1; }
    if (ln && l.p[0] == '=') {
      if (ln != 5) { free(buf); sig_errf(err, errlen, "malformed armor checksum line"); return -1; }
      for (int i = 1; i < 5; i++) {
        int v = b64v((unsigned char)l.p[i]);
        if (v < 0) { free(buf); sig_errf(err, errlen, "malformed armor checksum line"); return -1; }
        crc = crc << 6 | (uint32_t)v;
      }
      have_crc = 1;
      continue;
    }
    for (size_t i = 0; i < ln; i++) {
      int c = (unsigned char)l.p[i];
      if (c == '=') {
        if (q < 2) { free(buf); sig_errf(err, errlen, "misplaced '=' in armored data"); return -1; }
        pad++;
        if (q + pad > 4) { free(buf); sig_errf(err, errlen, "too much '=' padding in armored data"); return -1; }
        continue;
      }
      int v = b64v(c);
      if (v < 0) { free(buf); sig_errf(err, errlen, "a character that is not base64 in armored data"); return -1; }
      if (pad) { free(buf); sig_errf(err, errlen, "data after '=' padding in armored data"); return -1; }
      acc = acc << 6 | (uint32_t)v;
      if (++q == 4) {
        buf[o++] = (uint8_t)(acc >> 16); buf[o++] = (uint8_t)(acc >> 8); buf[o++] = (uint8_t)acc;
        q = 0; acc = 0;
      }
    }
  }
  if (q) {
    if (q == 1 || q + pad != 4) { free(buf); sig_errf(err, errlen, "armored data ends in the middle of a base64 group"); return -1; }
    if (q == 2) { if (acc & 0xf) { free(buf); sig_errf(err, errlen, "non-canonical base64 at the end of armored data"); return -1; } buf[o++] = (uint8_t)(acc >> 4); }
    else { if (acc & 0x3) { free(buf); sig_errf(err, errlen, "non-canonical base64 at the end of armored data"); return -1; } buf[o++] = (uint8_t)(acc >> 10); buf[o++] = (uint8_t)(acc >> 2); }
  } else if (pad) { free(buf); sig_errf(err, errlen, "misplaced '=' in armored data"); return -1; }
  if (o == 0) { free(buf); sig_errf(err, errlen, "empty armored block"); return -1; }
  if (have_crc && sig_crc24(buf, o) != crc) { free(buf); sig_errf(err, errlen, "armor checksum (CRC-24) does not match"); return -1; }
  *pos = p;
  *out = buf;
  return (long)o;
}

/* ---------- cleartext ---------- */
void sig_clear_free(sig_clear *c) { free(c->canon); free(c->text); free(c->sig); memset(c, 0, sizeof *c); }

static int hash_by_name(const char *p, size_t n) {
  static const struct { const char *s; int a; } t[] = {{"MD5", H_MD5}, {"SHA1", H_SHA1}, {"RIPEMD160", H_RIPEMD160}, {"SHA256", H_SHA256},
                                                       {"SHA384", H_SHA384}, {"SHA512", H_SHA512}, {"SHA224", H_SHA224}, {"SHA3-256", 12}, {"SHA3-512", 14}};
  for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (strlen(t[i].s) == n && !memcmp(t[i].s, p, n)) return t[i].a;
  return -1;
}

int sig_clear_split(const void *data, size_t n, sig_clear *out, char *err, size_t errlen) {
  const char *t = data;
  memset(out, 0, sizeof *out);
  if (memchr(t, 0, n)) { sig_errf(err, errlen, "the file contains a NUL byte"); return -1; }
  line l;
  if (!get_line(t, n, 0, &l) || !line_is(&l, "-----BEGIN PGP SIGNED MESSAGE-----")) {
    sig_errf(err, errlen, "the file does not start with -----BEGIN PGP SIGNED MESSAGE-----");
    return -1;
  }
  size_t p = l.next;
  /* headers: only Hash */
  for (;;) {
    if (!get_line(t, n, p, &l)) { sig_errf(err, errlen, "the message ends inside its headers"); return -1; }
    p = l.next;
    size_t ln = trim_len(l.p, l.n);
    if (!ln) break;
    if (ln < 6 || memcmp(l.p, "Hash: ", 6)) { sig_errf(err, errlen, "a header other than Hash: in a cleartext signed message"); return -1; }
    size_t i = 6;
    while (i < ln) {
      while (i < ln && (l.p[i] == ' ' || l.p[i] == ',')) i++;
      size_t s = i;
      while (i < ln && l.p[i] != ',' && l.p[i] != ' ') i++;
      if (i == s) break;
      int a = hash_by_name(l.p + s, i - s);
      if (a < 0) { sig_errf(err, errlen, "unknown hash name in the Hash: header"); return -1; }
      out->hashes |= 1u << a;
    }
  }
  /* No Hash header: RFC 4880 7 says the hash is then MD5, which GnuPG follows
   * (a version 4 signature over SHA-256 without a Hash header fails in gpgv
   * with "signature digest conflict"). RFC 9580 7.1 makes the header optional.
   * The stricter reading is kept: without a header, only MD5 is declared, and
   * MD5 is never accepted, so such a message cannot verify. */
  if (!out->hashes) out->hashes = 1u << H_MD5;
  /* the text */
  size_t cap = n + 1;
  out->canon = malloc(cap + n);  /* CR LF can add one byte per line: at most n more */
  out->text = malloc(cap);
  if (!out->canon || !out->text) { sig_clear_free(out); sig_errf(err, errlen, "out of memory"); return -1; }
  size_t co = 0, to = 0;
  int first = 1, ended = 0;
  while (get_line(t, n, p, &l)) {
    p = l.next;
    if (l.n > SIG_MAX_LINE) { sig_clear_free(out); sig_errf(err, errlen, "a line of the signed text is too long"); return -1; }
    if (memchr(l.p, '\r', l.n)) { sig_clear_free(out); sig_errf(err, errlen, "a carriage return inside a line of the signed text"); return -1; }
    if (!l.has_nl) break;                  /* the last line must end before the signature */
    const char *s = l.p;
    size_t sn = l.n;
    if (sn && s[0] == '-') {
      if (line_is(&l, "-----BEGIN PGP SIGNATURE-----")) { ended = 1; break; }
      if (sn < 2 || s[1] != ' ') { sig_clear_free(out); sig_errf(err, errlen, "a line of the signed text starts with '-' and is not dash-escaped"); return -1; }
      s += 2; sn -= 2;
    }
    sn = trim_len(s, sn);
    if (!first) { out->canon[co++] = '\r'; out->canon[co++] = '\n'; }
    memcpy(out->canon + co, s, sn); co += sn;
    memcpy(out->text + to, s, sn); to += sn;
    out->text[to++] = '\n';
    first = 0;
  }
  if (!ended) { sig_clear_free(out); sig_errf(err, errlen, "no -----BEGIN PGP SIGNATURE----- line after the signed text"); return -1; }
  out->canon[co] = 0; out->canonlen = co;
  out->text[to] = 0; out->textlen = to;
  /* the signature: re-read from its BEGIN line */
  size_t sigpos = (size_t)(l.p - t);
  long sl = sig_armor_decode(t, n, &sigpos, "PGP SIGNATURE", &out->sig, err, errlen);
  if (sl <= 0) { if (sl == 0) sig_errf(err, errlen, "no signature block"); sig_clear_free(out); return -1; }
  out->siglen = (size_t)sl;
  for (size_t k = sigpos; k < n; k++)
    if (t[k] != ' ' && t[k] != '\t' && t[k] != '\r' && t[k] != '\n') { sig_clear_free(out); sig_errf(err, errlen, "text after the signature block"); return -1; }
  return 0;
}
