// SPDX-License-Identifier: GPL-2.0-or-later
/* Native stress test of the journal ring (bin/svcd/journal.c): random record sizes,
 * then check that a walk returns the newest records, consecutive, with intact text. */
#include "journal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failed;
#define CHECK(what, cond) do { checks++; if (!(cond)) { failed++; printf("FAIL %s\n", what); } } while (0)
static uint32_t first, last, count, bad;
static int walk(void *ctx, const struct jrec *r) {
  char want[64];
  int n = snprintf(want, sizeof want, "line %u ", r->seq);
  if (!count) first = r->seq;
  else if (r->seq != last + 1) bad++;
  if (r->tlen < n || memcmp(r->text, want, n)) bad++;
  for (int i = n; i < r->tlen; i++) if (r->text[i] != 'x') { bad++; break; }
  if (r->ulen != 9 || memcmp(r->unit, "a.service", 9)) bad++;
  last = r->seq;
  count++;
  return 0;
}
int main(void) {
  journal_init(16384);
  srand(7);
  char text[5000];
  for (int round = 1; round <= 200000; round++) {
    int n = snprintf(text, sizeof text, "line %d ", round);
    int extra = rand() % (round % 97 == 0 ? 3000 : 120);
    memset(text + n, 'x', extra);
    journal_add(round, 6, 1, "a.service", "id", text, n + extra);
    if (round % 1000 == 0 || round < 50) {
      first = last = count = bad = 0;
      journal_walk(0, walk, 0);
      uint32_t b, u, r, d;
      journal_stats(&b, &u, &r, &d);
      if (bad || last != (uint32_t)round || count != r || u > b) { CHECK("ring consistent", 0); printf("  round %d: count %u last %u bad %u used %u\n", round, count, last, bad, u); break; }
    }
  }
  CHECK("200000 records through a 16 KiB ring", 1);
  first = last = count = bad = 0;
  journal_walk(199990, walk, 0);
  CHECK("walk after a sequence number", count == 10 && first == 199991 && last == 200000 && !bad);
  printf("journal: %d checks, %d failed\n", checks, failed);
  return failed != 0;
}
