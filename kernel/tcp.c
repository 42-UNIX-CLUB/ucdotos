// SPDX-License-Identifier: GPL-2.0-or-later
/* TCP (RFC 9293). What is here: the full connection state machine, retransmission
 * with RFC 6298 timers and Karn's rule, flow control with zero-window probes and
 * silly window avoidance on both sides, slow start, congestion avoidance, fast
 * retransmit and NewReno recovery (RFC 5681, 6582), delayed ACKs, the MSS option,
 * TIME_WAIT. What is not: window scaling, timestamps, SACK, keepalive, urgent data.
 * Without window scaling a connection has at most 64 KiB in flight. */
#include "net.h"

#define F_FIN 1
#define F_SYN 2
#define F_RST 4
#define F_PSH 8
#define F_ACK 16
#define TCP_HDR 20
#define RBUF 65535u          /* receive buffer: the largest window a header can carry */
#define SBUF 65536u          /* send buffer */
#define RTO_INIT 1000.0      /* RFC 6298 */
#define RTO_MIN 200.0        /* as Linux */
#define RTO_MAX 60000.0
#define DELACK_MS 40.0
#define TIME_WAIT_MS 60000.0 /* 2 MSL, as Linux */
#define FIN_WAIT2_MS 60000.0 /* a closed socket waiting for the peer's FIN gives up after this */
#define SYN_RETRIES 6
#define DATA_RETRIES 15
#define MSS_DEFAULT 536

#define SEQ_LT(a, b) ((s32)((a) - (b)) < 0)
#define SEQ_LEQ(a, b) ((s32)((a) - (b)) <= 0)
#define SEQ_GT(a, b) ((s32)((a) - (b)) > 0)
#define SEQ_GEQ(a, b) ((s32)((a) - (b)) >= 0)

struct ooo { struct ooo *next; u32 seq, len; int fin; u8 data[]; };

struct tcb {
  int state;
  u32 laddr, raddr;
  u16 lport, rport;
  struct inet *sk;       /* the socket, NULL once closed (or before accept) */
  struct inet *listener; /* for a connection not yet accepted */
  int backlog;           /* a listener's queue limit */
  u32 ready;             /* accept order; 0 = not ready */
  u8 ttl, fin_queued;
  /* sending */
  u32 iss, snd_una, snd_nxt, snd_max, snd_wnd, snd_wl1, snd_wl2;
  u8 *sbuf;
  u32 sb_head, sb_len, sb_seq; /* sb_seq: the sequence number of the first byte in sbuf */
  u16 mss, rmss;         /* the segment size we send; the one we announce */
  u32 cwnd, ssthresh, dupacks, recover;
  u8 in_recovery, rtt_on, persist_shift;
  u32 rtt_seq;
  double rtt_t, srtt, rttvar, rto;
  u32 retries, retransmits;
  /* receiving */
  u32 irs, rcv_nxt, rcv_adv; /* rcv_adv: the right edge of the window last announced */
  u8 *rbuf;
  u32 rb_head, rb_len, unacked;
  u8 fin_rcvd;
  struct ooo *ooo;
  u32 ooo_bytes;
  struct ktimer rtx, delack, persist, wait;
  struct tcb *next;
};
static struct tcb *tcbs;
static u32 ready_counter;


/* ---- allocation ---- */
static struct tcb *tcb_new(void) {
  struct tcb *t = kzalloc(sizeof *t);
  if (!t) return NULL;
  t->rbuf = kmalloc(RBUF);
  t->sbuf = kmalloc(SBUF);
  if (!t->rbuf || !t->sbuf) { kfree(t->rbuf); kfree(t->sbuf); kfree(t); return NULL; }
  t->rto = RTO_INIT;
  t->mss = MSS_DEFAULT;
  t->ssthresh = 0x7fffffff;
  t->ttl = 64;
  t->next = tcbs;
  tcbs = t;
  return t;
}
static void tcb_free(struct tcb *t) {
  timer_cancel(&t->rtx); timer_cancel(&t->delack); timer_cancel(&t->persist); timer_cancel(&t->wait);
  for (struct tcb **pp = &tcbs; *pp; pp = &(*pp)->next)
    if (*pp == t) { *pp = t->next; break; }
  while (t->ooo) { struct ooo *o = t->ooo; t->ooo = o->next; kfree(o); }
  if (t->sk) t->sk->tcb = NULL;
  kfree(t->rbuf); kfree(t->sbuf); kfree(t);
}
/* The connection is over. A socket keeps its (closed) tcb so it can read the error;
 * a connection nobody holds any more is freed. */
static void tcp_done(struct tcb *t, int err) {
  timer_cancel(&t->rtx); timer_cancel(&t->delack); timer_cancel(&t->persist); timer_cancel(&t->wait);
  t->state = TCP_CLOSE;
  if (t->sk) {
    if (err) { t->sk->err = err; t->rb_len = 0; }
    sk_wake(t->sk);
  } else tcb_free(t);
}

/* ---- sending segments ---- */
static u32 rcv_window(struct tcb *t) {
  u32 space = RBUF - t->rb_len;
  if (space < t->rmss && space < RBUF / 4) space = 0;     /* receiver silly window avoidance */
  u32 cur = SEQ_GT(t->rcv_adv, t->rcv_nxt) ? t->rcv_adv - t->rcv_nxt : 0;
  if (space < cur) space = cur;                            /* never move the right edge back */
  return space > 65535 ? 65535 : space;
}
static void send_raw(u32 laddr, u16 lport, u32 raddr, u16 rport, u32 seq, u32 ack, u8 flags, u16 win, u8 ttl,
                     const u8 *opt, u32 optlen, const u8 *d1, u32 n1, const u8 *d2, u32 n2) {
  u32 len = TCP_HDR + optlen + n1 + n2;
  struct pkt *p = pkt_alloc(IP_HDR + len);
  if (!p) return;
  u8 *h = p->data + IP_HDR;
  wr16(h, lport); wr16(h + 2, rport);
  wr32(h + 4, seq); wr32(h + 8, ack);
  h[12] = (u8)((5 + optlen / 4) << 4);
  h[13] = flags;
  wr16(h + 14, win);
  wr16(h + 16, 0); wr16(h + 18, 0);
  if (optlen) memcpy(h + TCP_HDR, opt, optlen);
  if (n1) memcpy(h + TCP_HDR + optlen, d1, n1);
  if (n2) memcpy(h + TCP_HDR + optlen + n1, d2, n2);
  wr16(h + 16, csum_fold(csum_add(csum_pseudo(laddr, raddr, IPPROTO_TCP, len), h, len)));
  ip_send(p, IPPROTO_TCP, laddr, raddr, ttl);
}
static void send_rst(u32 laddr, u16 lport, u32 raddr, u16 rport, u32 seq, u32 ack, u8 flags) {
  send_raw(laddr, lport, raddr, rport, seq, ack, flags, 0, 64, NULL, 0, NULL, 0, NULL, 0);
}
/* One segment: dlen bytes of the send buffer starting at sequence number seq. */
static void tcp_send(struct tcb *t, u32 seq, u8 flags, u32 dlen) {
  u8 opt[4];
  u32 optlen = 0;
  if (flags & F_SYN) { opt[0] = 2; opt[1] = 4; wr16(opt + 2, t->rmss); optlen = 4; }
  u32 win = rcv_window(t);
  const u8 *d1 = NULL, *d2 = NULL;
  u32 n1 = 0, n2 = 0;
  if (dlen) {
    u32 start = (t->sb_head + (seq - t->sb_seq)) % SBUF;
    n1 = SBUF - start < dlen ? SBUF - start : dlen;
    n2 = dlen - n1;
    d1 = t->sbuf + start; d2 = t->sbuf;
  }
  if (flags & F_ACK) {
    if (SEQ_GT(t->rcv_nxt + win, t->rcv_adv)) t->rcv_adv = t->rcv_nxt + win;
    t->unacked = 0;
    timer_cancel(&t->delack);
  }
  send_raw(t->laddr, t->lport, t->raddr, t->rport, seq, (flags & F_ACK) ? t->rcv_nxt : 0, flags, (u16)win, t->ttl,
           opt, optlen, d1, n1, d2, n2);
}
static void send_ack(struct tcb *t) { tcp_send(t, t->snd_nxt, F_ACK, 0); }
static void send_syn(struct tcb *t) { tcp_send(t, t->iss, t->state == TCP_SYN_SENT ? F_SYN : F_SYN | F_ACK, 0); }

static void rtx_timeout(void *);
static void persist_timeout(void *);
static void arm_rtx(struct tcb *t) { timer_set(&t->rtx, t->rto, rtx_timeout, t); }

/* Send what the windows allow. */
static void tcp_output(struct tcb *t) {
  int st = t->state;
  if (st != TCP_ESTABLISHED && st != TCP_CLOSE_WAIT && st != TCP_FIN_WAIT1 && st != TCP_CLOSING && st != TCP_LAST_ACK) return;
  for (;;) {
    u32 off = t->snd_nxt - t->sb_seq;
    if (off > t->sb_len) break;                      /* the FIN is out */
    u32 avail = t->sb_len - off;
    u32 wnd = t->snd_wnd < t->cwnd ? t->snd_wnd : t->cwnd;
    u32 flight = t->snd_nxt - t->snd_una;
    u32 can = wnd > flight ? wnd - flight : 0;
    u32 n = avail < can ? avail : can;
    if (n > t->mss) n = t->mss;
    int fin = t->fin_queued && off + n == t->sb_len;
    if (!n && !fin) break;
    if (n && n < t->mss && n < avail && flight) break; /* sender silly window avoidance: wait for the window to open */
    if (fin && n == 0 && avail) break;
    u8 flags = F_ACK | (n && off + n == t->sb_len ? F_PSH : 0) | (fin ? F_FIN : 0);
    if (!t->rtt_on && n && t->snd_nxt == t->snd_max) { t->rtt_on = 1; t->rtt_seq = t->snd_nxt; t->rtt_t = host_now(); }
    tcp_send(t, t->snd_nxt, flags, n);
    t->snd_nxt += n + (fin ? 1 : 0);
    if (SEQ_GT(t->snd_nxt, t->snd_max)) t->snd_max = t->snd_nxt;
    if (!t->rtx.armed) arm_rtx(t);
    if (fin) break;
  }
  /* Data waiting, a closed window and nothing in flight: probe the window, or the
   * connection would wait forever for an update that was lost. */
  u32 unsent = t->sb_len - (t->snd_nxt - t->sb_seq > t->sb_len ? t->sb_len : t->snd_nxt - t->sb_seq);
  if (unsent && t->snd_wnd == 0 && t->snd_nxt == t->snd_una && !t->rtx.armed && !t->persist.armed) {
    t->persist_shift = 0;
    timer_set(&t->persist, t->rto, persist_timeout, t);
  }
}

/* ---- timers ---- */
static void rtx_timeout(void *a) {
  struct tcb *t = a;
  if (t->state == TCP_SYN_SENT || t->state == TCP_SYN_RECV) {
    if (++t->retries > SYN_RETRIES) {
      if (!t->sk && t->listener) { tcb_free(t); return; }
      tcp_done(t, ETIMEDOUT);
      return;
    }
    t->rto = t->rto * 2 > RTO_MAX ? RTO_MAX : t->rto * 2;
    t->rtt_on = 0;
    t->retransmits++;
    send_syn(t);
    arm_rtx(t);
    return;
  }
  if (t->snd_una == t->snd_max) return;
  if (++t->retries > DATA_RETRIES) {
    send_rst(t->laddr, t->lport, t->raddr, t->rport, t->snd_nxt, 0, F_RST);
    tcp_done(t, ETIMEDOUT);
    return;
  }
  u32 flight = t->snd_max - t->snd_una;
  t->ssthresh = flight / 2 > 2u * t->mss ? flight / 2 : 2u * t->mss;
  t->cwnd = t->mss;
  t->in_recovery = 0;
  t->dupacks = 0;
  t->rto = t->rto * 2 > RTO_MAX ? RTO_MAX : t->rto * 2;
  t->rtt_on = 0;
  t->snd_nxt = t->snd_una;                           /* go back and resend from the first lost byte */
  t->retransmits++;
  tcp_output(t);
  if (!t->rtx.armed && t->snd_una != t->snd_max) arm_rtx(t);
}
static void persist_timeout(void *a) {
  struct tcb *t = a;
  if (t->snd_wnd) { tcp_output(t); return; }
  /* An old sequence number: the peer cannot accept it and answers with an ACK that
   * carries its current window. */
  tcp_send(t, t->snd_una - 1, F_ACK, 0);
  if (t->persist_shift < 6) t->persist_shift++;
  double ms = t->rto * (double)(1u << t->persist_shift);
  timer_set(&t->persist, ms > RTO_MAX ? RTO_MAX : ms, persist_timeout, t);
}
static void delack_timeout(void *a) { send_ack(a); }
static void wait_timeout(void *a) { tcp_done(a, 0); }
static void enter_time_wait(struct tcb *t) {
  t->state = TCP_TIME_WAIT;
  timer_cancel(&t->rtx); timer_cancel(&t->persist); timer_cancel(&t->delack);
  timer_set(&t->wait, TIME_WAIT_MS, wait_timeout, t);
}

/* ---- ACKs ---- */
static void rtt_sample(struct tcb *t) {
  double r = host_now() - t->rtt_t;
  if (t->srtt == 0) { t->srtt = r; t->rttvar = r / 2; }
  else {
    double d = t->srtt > r ? t->srtt - r : r - t->srtt;
    t->rttvar = 0.75 * t->rttvar + 0.25 * d;
    t->srtt = 0.875 * t->srtt + 0.125 * r;
  }
  double rto = t->srtt + (4 * t->rttvar > 1 ? 4 * t->rttvar : 1);
  t->rto = rto < RTO_MIN ? RTO_MIN : rto > RTO_MAX ? RTO_MAX : rto;
  t->rtt_on = 0;
}
static void init_cwnd(struct tcb *t) { /* RFC 6928 */
  u32 w = 2u * t->mss > 14600 ? 2u * t->mss : 14600;
  t->cwnd = 10u * t->mss < w ? 10u * t->mss : w;
}
/* Returns 0 if the segment must be dropped. */
static int process_ack(struct tcb *t, u32 seq, u32 ack, u32 win, int dlen, int synfin) {
  if (SEQ_GT(ack, t->snd_max)) { send_ack(t); return 0; } /* acks something never sent */
  if (SEQ_LEQ(ack, t->snd_una)) {
    if (ack == t->snd_una && !dlen && !synfin && win == t->snd_wnd && t->snd_max != t->snd_una) {
      if (t->in_recovery) { t->cwnd += t->mss; tcp_output(t); }
      else if (++t->dupacks == 3) {                    /* fast retransmit */
        u32 flight = t->snd_max - t->snd_una;
        t->ssthresh = flight / 2 > 2u * t->mss ? flight / 2 : 2u * t->mss;
        t->recover = t->snd_max;
        t->in_recovery = 1;
        t->rtt_on = 0;
        t->retransmits++;
        u32 n = t->sb_len - (t->snd_una - t->sb_seq);
        if (n > t->mss) n = t->mss;
        int fin = t->fin_queued && t->snd_una - t->sb_seq + n == t->sb_len;
        tcp_send(t, t->snd_una, F_ACK | (fin ? F_FIN : 0), n);
        t->cwnd = t->ssthresh + 3u * t->mss;
        arm_rtx(t);
      }
      return 1;
    }
  } else {
    u32 acked = ack - t->snd_una;
    if (SEQ_GT(ack, t->sb_seq)) {                      /* drop acknowledged bytes from the send buffer */
      u32 n = ack - t->sb_seq;
      if (n > t->sb_len) n = t->sb_len;
      t->sb_head = (t->sb_head + n) % SBUF;
      t->sb_len -= n;
      t->sb_seq += n;
    }
    t->snd_una = ack;
    if (SEQ_LT(t->snd_nxt, ack)) t->snd_nxt = ack;
    if (t->rtt_on && SEQ_GT(ack, t->rtt_seq)) rtt_sample(t);
    t->retries = 0;
    if (t->in_recovery) {
      if (SEQ_GEQ(ack, t->recover)) { t->in_recovery = 0; t->cwnd = t->ssthresh; }
      else {                                           /* partial ACK: the next hole is lost too */
        u32 n = t->sb_len < t->mss ? t->sb_len : t->mss;
        if (n) { tcp_send(t, t->snd_una, F_ACK, n); t->retransmits++; }
        t->cwnd = t->cwnd > acked ? t->cwnd - acked : 0;
        t->cwnd += t->mss;
      }
    } else if (t->cwnd < t->ssthresh) t->cwnd += acked < t->mss ? acked : t->mss; /* slow start */
    else t->cwnd += (u32)t->mss * t->mss / t->cwnd > 0 ? (u32)t->mss * t->mss / t->cwnd : 1; /* congestion avoidance */
    t->dupacks = 0;
    if (t->snd_una == t->snd_max) timer_cancel(&t->rtx); else arm_rtx(t);
    if (t->sk) sk_wake(t->sk);                         /* room in the send buffer */
  }
  if (SEQ_LT(t->snd_wl1, seq) || (t->snd_wl1 == seq && SEQ_LEQ(t->snd_wl2, ack))) {
    t->snd_wnd = win;
    t->snd_wl1 = seq;
    t->snd_wl2 = ack;
    if (win && t->persist.armed) timer_cancel(&t->persist);
  }
  return 1;
}
static int fin_acked(struct tcb *t) { return t->fin_queued && t->snd_una == t->sb_seq + t->sb_len + 1; }

/* ---- receiving data ---- */
static u32 rb_put(struct tcb *t, const u8 *d, u32 n) {
  u32 room = RBUF - t->rb_len;
  if (n > room) n = room;
  u32 tail = (t->rb_head + t->rb_len) % RBUF;
  u32 first = RBUF - tail < n ? RBUF - tail : n;
  memcpy(t->rbuf + tail, d, first);
  memcpy(t->rbuf, d + first, n - first);
  t->rb_len += n;
  return n;
}
static void got_fin(struct tcb *t) {
  t->rcv_nxt++;
  t->fin_rcvd = 1;
  switch (t->state) {
  case TCP_SYN_RECV: case TCP_ESTABLISHED: t->state = TCP_CLOSE_WAIT; break;
  case TCP_FIN_WAIT1: if (fin_acked(t)) enter_time_wait(t); else t->state = TCP_CLOSING; break;
  case TCP_FIN_WAIT2: enter_time_wait(t); break;
  }
  if (t->sk) sk_wake(t->sk);
}
/* In-order bytes, then whatever the out-of-order queue can now supply. */
static void take_data(struct tcb *t, u32 seq, const u8 *d, u32 len, int fin) {
  if (SEQ_LT(seq, t->rcv_nxt)) {
    u32 skip = t->rcv_nxt - seq;
    if (skip >= len) { d += len; len = 0; } else { d += skip; len -= skip; }
    seq = t->rcv_nxt;
  }
  if (seq == t->rcv_nxt) {
    u32 got = rb_put(t, d, len);
    t->rcv_nxt += got;
    t->unacked += got;
    if (fin && got == len) got_fin(t);
    for (int again = 1; again && !t->fin_rcvd;) {
      again = 0;
      for (struct ooo **pp = &t->ooo; *pp; pp = &(*pp)->next) {
        struct ooo *o = *pp;
        if (SEQ_GT(o->seq, t->rcv_nxt)) continue;
        *pp = o->next;
        t->ooo_bytes -= o->len;
        u32 end = o->seq + o->len;
        if (SEQ_GT(end, t->rcv_nxt)) {
          u32 skip = t->rcv_nxt - o->seq, n = o->len - skip;
          u32 put = rb_put(t, o->data + skip, n);
          t->rcv_nxt += put;
          t->unacked += put;
          if (o->fin && put == n) got_fin(t);
        } else if (o->fin && end == t->rcv_nxt) got_fin(t);
        kfree(o);
        again = 1;
        break;
      }
    }
    if (got && t->sk) sk_wake(t->sk);
    if (t->ooo || t->fin_rcvd || t->unacked >= 2u * t->rmss || t->unacked >= RBUF / 2) send_ack(t);
    else if (t->unacked && !t->delack.armed) timer_set(&t->delack, DELACK_MS, delack_timeout, t);
    return;
  }
  /* A hole before this segment: keep it, and ACK at once so the sender sees a duplicate. */
  if ((len || fin) && t->ooo_bytes + len <= RBUF) {
    int dup = 0;
    for (struct ooo *o = t->ooo; o; o = o->next) if (o->seq == seq && o->len >= len) dup = 1;
    struct ooo *o = dup ? NULL : kmalloc(sizeof *o + len);
    if (o) {
      o->seq = seq; o->len = len; o->fin = fin;
      memcpy(o->data, d, len);
      o->next = t->ooo;
      t->ooo = o;
      t->ooo_bytes += len;
    }
  }
  send_ack(t);
}

/* ---- input ---- */
/* An exact connection first, then a listener on the port; a segment for an address
 * outside the machine can only reach a transparent listener (the router). */
static struct tcb *lookup(u32 saddr, u16 sport, u32 daddr, u16 dport) {
  struct tcb *l = NULL, *tl = NULL;
  for (struct tcb *t = tcbs; t; t = t->next) {
    if (t->state == TCP_CLOSE) continue;
    if (t->state == TCP_LISTEN) {
      if (t->sk && t->sk->transparent) { if (!t->sk->tport || t->sk->tport == dport) tl = t; }
      else if (t->lport == dport && (!t->laddr || t->laddr == daddr)) l = t;
      continue;
    }
    if (t->lport == dport && t->rport == sport && t->raddr == saddr && t->laddr == daddr) return t;
  }
  if (is_local(daddr)) return l;
  return tl;
}
static u16 parse_mss(const u8 *h, u32 doff) {
  for (u32 i = TCP_HDR; i < doff;) {
    u8 kind = h[i];
    if (kind == 0) break;
    if (kind == 1) { i++; continue; }
    if (i + 1 >= doff || h[i + 1] < 2) break;
    if (kind == 2 && h[i + 1] == 4 && i + 4 <= doff) return rd16(h + i + 2);
    i += h[i + 1];
  }
  return 0;
}
static u16 my_mss(u32 raddr) {                         /* what we announce: the route's MTU less the headers */
  struct netif *n = route(raddr);
  return (u16)(n ? n->mtu - IP_HDR - TCP_HDR : MSS_DEFAULT);
}
static u16 mss_for(u32 raddr, u16 peer) {
  struct netif *n = route(raddr);
  u32 mine = n ? n->mtu - IP_HDR - TCP_HDR : MSS_DEFAULT;
  u32 m = peer ? peer : MSS_DEFAULT;
  return (u16)(m < mine ? m : mine);
}

void tcp_input(struct pkt *p) {
  u8 *h = p->data + p->off;
  u32 len = p->len - p->off;
  if (len < TCP_HDR) goto drop;
  u32 doff = (u32)(h[12] >> 4) * 4;
  if (doff < TCP_HDR || doff > len) goto drop;
  if (csum_fold(csum_add(csum_pseudo(p->saddr, p->daddr, IPPROTO_TCP, len), h, len)) != 0) goto drop;
  u16 sport = rd16(h), dport = rd16(h + 2);
  u32 seq = rd32(h + 4), ack = rd32(h + 8), win = rd16(h + 14);
  u8 fl = h[13];
  const u8 *data = h + doff;
  u32 dlen = len - doff;
  struct tcb *t = lookup(p->saddr, sport, p->daddr, dport);

  if (!t) {                                            /* nobody here: refuse */
    if (fl & F_RST) goto drop;
    if (fl & F_ACK) send_rst(p->daddr, dport, p->saddr, sport, ack, 0, F_RST);
    else send_rst(p->daddr, dport, p->saddr, sport, 0, seq + dlen + !!(fl & F_SYN) + !!(fl & F_FIN), F_RST | F_ACK);
    goto drop;
  }

  if (t->state == TCP_LISTEN) {
    if (fl & F_RST) goto drop;
    if (fl & F_ACK) { send_rst(p->daddr, dport, p->saddr, sport, ack, 0, F_RST); goto drop; }
    if (!(fl & F_SYN)) goto drop;
    int queued = 0;
    for (struct tcb *c = tcbs; c; c = c->next) if (c->listener == t->sk) queued++;
    if (queued >= t->backlog) goto drop;               /* the client will retry its SYN */
    struct tcb *c = tcb_new();
    if (!c) goto drop;
    c->laddr = p->daddr; c->lport = dport; c->raddr = p->saddr; c->rport = sport;
    c->listener = t->sk;
    c->ttl = t->ttl;
    c->state = TCP_SYN_RECV;
    c->irs = seq; c->rcv_nxt = seq + 1; c->rcv_adv = c->rcv_nxt;
    c->iss = host_random();
    c->snd_una = c->iss; c->snd_nxt = c->snd_max = c->iss + 1; c->sb_seq = c->iss + 1;
    c->snd_wnd = win; c->snd_wl1 = seq; c->snd_wl2 = c->iss;
    c->rmss = my_mss(c->raddr);
    c->mss = mss_for(c->raddr, parse_mss(h, doff));
    c->rtt_on = 1; c->rtt_seq = c->iss; c->rtt_t = host_now();
    send_syn(c);
    arm_rtx(c);
    goto drop;
  }

  if (t->state == TCP_SYN_SENT) {
    int ack_ok = (fl & F_ACK) && SEQ_GT(ack, t->iss) && SEQ_LEQ(ack, t->snd_max);
    if ((fl & F_ACK) && !ack_ok) { if (!(fl & F_RST)) send_rst(p->daddr, dport, p->saddr, sport, ack, 0, F_RST); goto drop; }
    if (fl & F_RST) { if (ack_ok) tcp_done(t, ECONNREFUSED); goto drop; }
    if (!(fl & F_SYN)) goto drop;
    t->irs = seq; t->rcv_nxt = seq + 1; t->rcv_adv = t->rcv_nxt;
    t->mss = mss_for(t->raddr, parse_mss(h, doff));
    if (ack_ok) {
      t->snd_una = ack;
      if (t->rtt_on && t->retransmits == 0) rtt_sample(t);
      t->rtt_on = 0;
      t->retries = 0;
      timer_cancel(&t->rtx);
      t->snd_wnd = win; t->snd_wl1 = seq; t->snd_wl2 = ack;
      t->state = TCP_ESTABLISHED;
      init_cwnd(t);
      send_ack(t);
      if (t->sk) sk_wake(t->sk);
    } else {                                           /* simultaneous open */
      t->state = TCP_SYN_RECV;
      send_syn(t);
    }
    goto drop;
  }

  /* Synchronized states. First: is the segment inside the receive window? */
  if (t->state == TCP_SYN_RECV && (fl & F_SYN) && !(fl & F_ACK) && seq == t->irs) { send_syn(t); goto drop; } /* our SYN-ACK was lost */
  u32 seglen = dlen + !!(fl & F_SYN) + !!(fl & F_FIN);
  u32 rwnd = rcv_window(t);
  int in_win;
  if (!seglen) in_win = rwnd ? SEQ_GEQ(seq, t->rcv_nxt) && SEQ_LT(seq, t->rcv_nxt + rwnd) : seq == t->rcv_nxt;
  else in_win = rwnd && ((SEQ_GEQ(seq, t->rcv_nxt) && SEQ_LT(seq, t->rcv_nxt + rwnd)) ||
                         (SEQ_GEQ(seq + seglen - 1, t->rcv_nxt) && SEQ_LT(seq + seglen - 1, t->rcv_nxt + rwnd)));
  if (!in_win) {
    if (!(fl & F_RST)) send_ack(t);
    if (t->state == TCP_TIME_WAIT && (fl & F_FIN)) timer_set(&t->wait, TIME_WAIT_MS, wait_timeout, t);
    goto drop;
  }
  if (fl & F_RST) {
    if (t->state == TCP_SYN_RECV && !t->sk) { tcb_free(t); goto drop; }
    int st = t->state;
    tcp_done(t, st == TCP_CLOSE_WAIT ? EPIPE : (st == TCP_CLOSING || st == TCP_LAST_ACK || st == TCP_TIME_WAIT) ? 0 : ECONNRESET);
    goto drop;
  }
  if (fl & F_SYN) { send_ack(t); goto drop; }          /* RFC 5961: answer with a challenge ACK */
  if (!(fl & F_ACK)) goto drop;

  if (t->state == TCP_SYN_RECV) {
    if (!SEQ_GT(ack, t->snd_una) || SEQ_GT(ack, t->snd_max)) { send_rst(p->daddr, dport, p->saddr, sport, ack, 0, F_RST); goto drop; }
    t->state = TCP_ESTABLISHED;
    if (t->rtt_on && t->retransmits == 0) rtt_sample(t);
    t->rtt_on = 0;
    t->retries = 0;
    t->snd_una = ack;
    timer_cancel(&t->rtx);
    t->snd_wnd = win; t->snd_wl1 = seq; t->snd_wl2 = ack;
    init_cwnd(t);
    if (t->listener) { t->ready = ++ready_counter; sk_wake(t->listener); }
    else if (t->sk) sk_wake(t->sk);
  }
  if (!process_ack(t, seq, ack, win, (int)dlen, fl & (F_SYN | F_FIN))) goto drop;
  switch (t->state) {
  case TCP_FIN_WAIT1:
    if (fin_acked(t)) {
      t->state = TCP_FIN_WAIT2;
      timer_cancel(&t->rtx);
      if (!t->sk && !t->listener) timer_set(&t->wait, FIN_WAIT2_MS, wait_timeout, t);
    }
    break;
  case TCP_CLOSING: if (fin_acked(t)) enter_time_wait(t); break;
  case TCP_LAST_ACK: if (fin_acked(t)) { tcp_done(t, 0); goto drop; } break;
  case TCP_TIME_WAIT: if (fl & F_FIN) { send_ack(t); timer_set(&t->wait, TIME_WAIT_MS, wait_timeout, t); } goto drop;
  }

  if (dlen || (fl & F_FIN)) {
    int st = t->state;
    if (st == TCP_ESTABLISHED || st == TCP_FIN_WAIT1 || st == TCP_FIN_WAIT2) {
      if (dlen && !t->sk && !t->listener) {            /* data for a socket that was closed: reset */
        send_rst(t->laddr, t->lport, t->raddr, t->rport, t->snd_nxt, 0, F_RST);
        tcp_done(t, 0);
        goto drop;
      }
      take_data(t, seq, data, dlen, fl & F_FIN);
    } else if (fl & F_FIN) send_ack(t);                /* a retransmitted FIN after ours */
  }
  tcp_output(t);
drop:
  pkt_free(p);
}

/* ---- socket calls ---- */
static void queue_fin(struct tcb *t) {
  if (t->fin_queued) return;
  if (t->state == TCP_ESTABLISHED || t->state == TCP_SYN_RECV) t->state = TCP_FIN_WAIT1;
  else if (t->state == TCP_CLOSE_WAIT) t->state = TCP_LAST_ACK;
  else return;
  t->fin_queued = 1;
  tcp_output(t);
}
long tcp_connect(struct inet *s, int nonblock) {
  struct tcb *t = s->tcb;
  if (t) {
    if (t->state == TCP_LISTEN) return -EINVAL;
    if (t->state == TCP_SYN_SENT || t->state == TCP_SYN_RECV) {
      if (!s->connecting) return -EALREADY;
      if (nonblock) return -EALREADY;
      wq_add(&s->wq);
      return block(-1);
    }
    if (t->state == TCP_CLOSE && s->err) { int e = s->err; s->err = 0; s->connecting = 0; return -e; }
    if (s->connecting) { s->connecting = 0; return 0; }  /* the retry after a blocking connect */
    return -EISCONN;
  }
  if (s->err) { int e = s->err; s->err = 0; return -e; }
  struct netif *n = route(s->raddr);
  if (!n) return -ENETUNREACH;
  if (!s->laddr) s->laddr = n->addr;
  if (!s->lport) {
    s->lport = port_ephemeral(IPPROTO_TCP);
    if (!s->lport) return -EADDRNOTAVAIL;
  }
  for (struct tcb *o = tcbs; o; o = o->next)
    if (o->state != TCP_CLOSE && o->state != TCP_LISTEN && o->lport == s->lport && o->rport == s->rport &&
        o->laddr == s->laddr && o->raddr == s->raddr) return -EADDRNOTAVAIL;
  t = tcb_new();
  if (!t) return -ENOMEM;
  t->laddr = s->laddr; t->lport = s->lport; t->raddr = s->raddr; t->rport = s->rport;
  t->ttl = s->ttl ? s->ttl : 64;
  t->state = TCP_SYN_SENT;
  t->iss = host_random();
  t->snd_una = t->iss; t->snd_nxt = t->snd_max = t->iss + 1; t->sb_seq = t->iss + 1;
  t->rmss = my_mss(t->raddr);
  t->rtt_on = 1; t->rtt_seq = t->iss; t->rtt_t = host_now();
  t->sk = s;
  s->tcb = t;
  s->connecting = !nonblock;           /* a blocking caller retries this call and gets 0 */
  send_syn(t);
  arm_rtx(t);
  if (nonblock) return -EINPROGRESS;
  wq_add(&s->wq);
  return block(-1);
}
long tcp_listen(struct inet *s, int backlog) {
  if (backlog < 1) backlog = 1;
  if (backlog > 64) backlog = 64;
  if (s->tcb) {
    if (s->tcb->state != TCP_LISTEN) return -EINVAL;
    s->tcb->backlog = backlog;
    return 0;
  }
  if (!s->lport) {
    s->lport = port_ephemeral(IPPROTO_TCP);
    if (!s->lport) return -EADDRINUSE;
  }
  for (struct tcb *o = tcbs; o; o = o->next)
    if (o->state == TCP_LISTEN && o->lport == s->lport && (!o->laddr || !s->laddr || o->laddr == s->laddr)) return -EADDRINUSE;
  struct tcb *t = tcb_new();
  if (!t) return -ENOMEM;
  t->state = TCP_LISTEN;
  t->laddr = s->laddr; t->lport = s->lport;
  t->ttl = s->ttl ? s->ttl : 64;
  t->backlog = backlog;
  t->sk = s;
  s->tcb = t;
  return 0;
}
long tcp_accept(struct inet *l, struct inet **out, int nonblock) {
  if (!l->tcb || l->tcb->state != TCP_LISTEN) return -EINVAL;
  struct tcb *c = NULL;
  for (struct tcb *t = tcbs; t; t = t->next)
    if (t->listener == l && t->ready && (!c || t->ready < c->ready)) c = t;
  if (!c) {
    if (nonblock) return -EAGAIN;
    wq_add(&l->rq);
    return block(-1);
  }
  struct inet *s = kzalloc(sizeof *s);
  if (!s) return -ENOMEM;
  s->family = AF_INET; s->type = SOCK_STREAM; s->proto = IPPROTO_TCP;
  s->laddr = c->laddr; s->lport = c->lport; s->raddr = c->raddr; s->rport = c->rport;
  s->reuse = l->reuse; s->ttl = l->ttl; s->connected = 1;
  s->owner = cur->pid;
  s->tcb = c;
  c->sk = s;
  c->listener = NULL;
  c->ready = 0;
  s->next = inets;
  inets = s;
  *out = s;
  return 0;
}
long tcp_read(struct inet *s, u32 ubuf, u32 len, int nonblock, int peek) {
  struct tcb *t = s->tcb;
  if (!t || t->state == TCP_LISTEN) return -ENOTCONN;
  if (t->state == TCP_SYN_SENT || t->state == TCP_SYN_RECV || (!t->rb_len && !t->fin_rcvd && t->state != TCP_CLOSE && !s->shut_rd)) {
    if (s->err) { int e = s->err; s->err = 0; return -e; }
    if (nonblock) return -EAGAIN;
    wq_add(&s->rq);
    return block(-1);
  }
  if (!t->rb_len) {
    if (s->err) { int e = s->err; s->err = 0; return -e; }
    return 0;                                          /* end of stream */
  }
  if (!len) return 0;
  u32 n = len < t->rb_len ? len : t->rb_len;
  u32 first = RBUF - t->rb_head < n ? RBUF - t->rb_head : n;
  if (copy_to_user(ubuf, t->rbuf + t->rb_head, first) || copy_to_user(ubuf + first, t->rbuf, n - first)) return -EFAULT;
  if (peek) return n;
  t->rb_head = (t->rb_head + n) % RBUF;
  t->rb_len -= n;
  /* Tell the sender when reading has opened the window by a useful amount. */
  int st = t->state;
  if (st == TCP_ESTABLISHED || st == TCP_FIN_WAIT1 || st == TCP_FIN_WAIT2) {
    u32 right = t->rcv_nxt + rcv_window(t);
    u32 step = RBUF / 2 < 2u * t->rmss ? RBUF / 2 : 2u * t->rmss;
    if (SEQ_GT(right, t->rcv_adv) && right - t->rcv_adv >= step) send_ack(t);
  }
  return n;
}
long tcp_write(struct inet *s, u32 ubuf, u32 len, int nonblock) {
  struct tcb *t = s->tcb;
  if (!t || t->state == TCP_LISTEN) return -ENOTCONN;
  if (s->err) { int e = s->err; s->err = 0; return -e; }
  if (t->state == TCP_SYN_SENT || t->state == TCP_SYN_RECV) {
    if (nonblock) return -EAGAIN;
    wq_add(&s->wq);
    return block(-1);
  }
  if ((t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT) || t->fin_queued) return -EPIPE;
  u32 room = SBUF - t->sb_len;
  if (!room) {
    if (nonblock) return -EAGAIN;
    wq_add(&s->wq);
    return block(-1);
  }
  u32 n = len < room ? len : room;
  u32 tail = (t->sb_head + t->sb_len) % SBUF;
  u32 first = SBUF - tail < n ? SBUF - tail : n;
  if (copy_from_user(t->sbuf + tail, ubuf, first) || copy_from_user(t->sbuf, ubuf + first, n - first)) return -EFAULT;
  t->sb_len += n;
  tcp_output(t);
  return n;
}
long tcp_shutdown(struct inet *s, int how) {
  struct tcb *t = s->tcb;
  if (!t || t->state == TCP_LISTEN || t->state == TCP_SYN_SENT || t->state == TCP_CLOSE) return -ENOTCONN;
  if (how == SHUT_RD || how == SHUT_RDWR) { s->shut_rd = 1; sk_wake(s); }
  if (how == SHUT_WR || how == SHUT_RDWR) queue_fin(t);
  return 0;
}
void tcp_close(struct inet *s) {
  struct tcb *t = s->tcb;
  if (!t) return;
  if (t->state == TCP_LISTEN) {                        /* refuse the connections nobody accepted */
    for (struct tcb *c = tcbs, *nx; c; c = nx) {
      nx = c->next;
      if (c->listener != s) continue;
      send_rst(c->laddr, c->lport, c->raddr, c->rport, c->snd_nxt, 0, F_RST);
      tcb_free(c);
    }
    tcb_free(t);
    return;
  }
  t->sk = NULL;
  s->tcb = NULL;
  int st = t->state;
  if (st == TCP_CLOSE || st == TCP_SYN_SENT) { tcb_free(t); return; }
  if (s->linger0 && st != TCP_TIME_WAIT) {            /* SO_LINGER with 0 s: abort with a reset */
    send_rst(t->laddr, t->lport, t->raddr, t->rport, t->snd_nxt, 0, F_RST);
    tcb_free(t);
    return;
  }
  if (t->rb_len && st != TCP_TIME_WAIT) {              /* unread data: abort, as RFC 2525 asks */
    send_rst(t->laddr, t->lport, t->raddr, t->rport, t->snd_nxt, 0, F_RST);
    tcb_free(t);
    return;
  }
  if (st == TCP_ESTABLISHED || st == TCP_SYN_RECV || st == TCP_CLOSE_WAIT) queue_fin(t);
  else if (st == TCP_FIN_WAIT2) timer_set(&t->wait, FIN_WAIT2_MS, wait_timeout, t);
}
int tcp_poll(struct inet *s) {
  struct tcb *t = s->tcb;
  if (!t) return POLLOUT | POLLHUP;
  int m = 0;
  if (t->state == TCP_LISTEN) {
    for (struct tcb *c = tcbs; c; c = c->next) if (c->listener == s && c->ready) return POLLIN;
    return 0;
  }
  if (s->err) m |= POLLERR;
  switch (t->state) {
  case TCP_SYN_SENT: case TCP_SYN_RECV: return m;
  case TCP_CLOSE: return m | POLLIN | POLLOUT | POLLHUP;
  }
  if (t->rb_len || t->fin_rcvd || s->shut_rd) m |= POLLIN;
  if (t->fin_rcvd && t->fin_queued) m |= POLLHUP;
  if (t->fin_queued || t->sb_len < SBUF) m |= POLLOUT;  /* after shutdown, a write fails at once */
  return m;
}
int tcp_count(void) { int n = 0; for (struct tcb *t = tcbs; t; t = t->next) n++; return n; }
int tcp_info(int index, struct sockinfo *o) {
  struct tcb *t = tcbs;
  while (t && index-- > 0) t = t->next;
  if (!t) return 0;
  o->proto = IPPROTO_TCP;
  o->state = (u8)t->state;
  o->laddr = t->laddr; o->lport = t->lport; o->raddr = t->raddr; o->rport = t->rport;
  o->owner = t->sk ? (u16)t->sk->owner : t->listener ? (u16)t->listener->owner : 0;
  if (t->state == TCP_LISTEN) {
    for (struct tcb *c = tcbs; c; c = c->next) if (c->listener == t->sk && c->ready) o->rx_queue++;
    o->tx_queue = (u32)t->backlog;
    return 1;
  }
  o->rx_queue = t->rb_len;
  o->tx_queue = t->sb_len;
  o->cwnd = t->cwnd; o->ssthresh = t->ssthresh; o->snd_wnd = t->snd_wnd; o->mss = t->mss;
  o->srtt_us = (u32)(t->srtt * 1000); o->rto_ms = (u32)t->rto; o->retransmits = t->retransmits;
  return 1;
}
/* A local port still held by a connection whose socket is gone (TIME_WAIT and the like). */
int tcp_port_used(u16 port, u32 addr, int reuse) {
  for (struct tcb *t = tcbs; t; t = t->next) {
    if (t->sk || t->listener || t->state == TCP_CLOSE || t->lport != port) continue;
    if (addr && t->laddr && t->laddr != addr) continue;
    if (!reuse) return 1;
  }
  return 0;
}
int tcp_state(const struct tcb *t) { return t->state; }
