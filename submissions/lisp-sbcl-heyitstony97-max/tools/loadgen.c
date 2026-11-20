/* A small k6-like load generator for the feed API (epoll, one thread).
 * Each connection is one "user": GET /feed -> think -> GET /posts/:id -> think -> like (LIKE%) -> post (POST%) -> think.
 * Reports latency percentiles for reads and writes separately, and the worst read latency in every second
 * (so a stall of the server shows up as a spike).
 *   loadgen HOST PORT CONNS SECONDS THINK_MS TOKENS.json [LIKE_PCT=15] [POST_PCT=2] [RAMP_S=5]
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define RBUF 65536
#define WHEEL 32768
#define NB 60000 /* 100 us buckets up to 6 s */

typedef struct {
  int fd, state, step, tok;      /* state: 0 not connected, 1 connecting, 2 thinking, 3 waiting for response */
  int next;                      /* timer wheel link */
  int is_write, rlen, need;
  long long post_id, iter;
  uint64_t t_send;
  char *rbuf;
  char out[1200];
  int olen, osent;
} Conn;

static Conn *C;
static int NC, ep, wheel[WHEEL];
static uint64_t wheel_ms;
static char **tokens;
static int ntok;
static long long newest = 500000;
static uint32_t hist[2][NB + 1];
static uint64_t count[2], errs, sock_errs, t_start, t_measure, like201, like200, post201;
static int draining;
static uint32_t sec_max[4096], sec_cnt[4096], sec_wmax[4096];
static int like_pct = 15, post_pct = 2, think_ms, port;
static const char *host;
static uint64_t rng = 88172645463325252ULL;

static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }

static void arm(int i, int delay_ms) {
  if (delay_ms < 1) delay_ms = 1;
  if (delay_ms >= WHEEL) delay_ms = WHEEL - 1;
  int s = (int)((wheel_ms + (uint64_t)delay_ms) % WHEEL);
  C[i].next = wheel[s];
  wheel[s] = i;
}
static int think(void) { return think_ms / 2 + (int)(rnd() % (uint64_t)(think_ms + 1)); }

static void do_connect(int i) {
  Conn *c = &C[i];
  c->fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  int one = 1;
  setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET; a.sin_port = htons(port); inet_pton(AF_INET, host, &a.sin_addr);
  connect(c->fd, (struct sockaddr *)&a, sizeof a);
  struct epoll_event ev = {.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP, .data.u32 = (uint32_t)i};
  epoll_ctl(ep, EPOLL_CTL_ADD, c->fd, &ev);
  c->state = 1;
}

static void flush_out(Conn *c) {
  while (c->osent < c->olen) {
    ssize_t n = send(c->fd, c->out + c->osent, c->olen - c->osent, MSG_NOSIGNAL);
    if (n <= 0) break;
    c->osent += (int)n;
  }
}

static void send_next(int i) {
  Conn *c = &C[i];
  if (draining) { c->state = 2; return; }
  const char *tok = tokens[c->tok];
  for (;;) {
    int st = c->step;
    c->is_write = 0;
    if (st == 0) {
      c->olen = snprintf(c->out, sizeof c->out, "GET /feed HTTP/1.1\r\nHost: x\r\n\r\n");
    } else if (st == 1) {
      c->post_id = newest - (long long)(rnd() % 20);
      c->olen = snprintf(c->out, sizeof c->out, "GET /posts/%lld HTTP/1.1\r\nHost: x\r\n\r\n", c->post_id);
    } else if (st == 2) {
      if ((int)(rnd() % 100) >= like_pct) { c->step = 3; continue; }
      c->is_write = 1;
      c->olen = snprintf(c->out, sizeof c->out,
                         "POST /posts/%lld/like HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nContent-Type: application/json\r\nContent-Length: 0\r\n\r\n",
                         c->post_id, tok);
    } else {
      if ((int)(rnd() % 100) >= post_pct) { c->step = 0; c->iter++; arm(i, think() * 2); c->state = 2; return; }
      char body[256];
      int bl = snprintf(body, sizeof body, "{\"body\":\"user %d says hi at %llu (VU %d, iter %lld)\"}", c->tok, (unsigned long long)now_us(), i, c->iter);
      c->is_write = 1;
      c->olen = snprintf(c->out, sizeof c->out,
                         "POST /posts HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s",
                         tok, bl, body);
    }
    break;
  }
  c->osent = 0; c->rlen = 0; c->need = -1;
  c->t_send = now_us();
  c->state = 3;
  flush_out(c);
}

static void done(int i) {
  Conn *c = &C[i];
  uint64_t t = now_us(), lat = t - c->t_send;
  int status = atoi(c->rbuf + 9);
  if (c->step == 2) { if (status == 201) like201++; else if (status == 200) like200++; }
  if (c->step == 3 && status == 201) post201++;
  if (t >= t_measure) {
    int w = c->is_write;
    uint64_t b = lat / 100;
    hist[w][b > NB ? NB : b]++;
    count[w]++;
    if (status < 200 || status > 299) errs++;
    uint64_t s = (t - t_start) / 1000000;
    if (s < 4096) {
      if (!w) { if (lat / 1000 > sec_max[s]) sec_max[s] = (uint32_t)(lat / 1000); }
      else if (lat / 1000 > sec_wmax[s]) sec_wmax[s] = (uint32_t)(lat / 1000);
      sec_cnt[s]++;
    }
  }
  if (c->step == 0) { /* remember the newest post id from the feed */
    char *p = memmem(c->rbuf, c->rlen, "{\"id\":", 6);
    if (p) { long long v = atoll(p + 6); if (v > newest) newest = v; }
  }
  int st = c->step;
  c->step = st + 1;
  if (st == 0 || st == 1) { c->state = 2; arm(i, think()); }
  else if (st == 2) send_next(i);                 /* like done: maybe post, at once (as k6 does) */
  else { c->step = 0; c->iter++; c->state = 2; arm(i, think() * 2); }
}

static void on_read(int i) {
  Conn *c = &C[i];
  for (;;) {
    ssize_t n = recv(c->fd, c->rbuf + c->rlen, RBUF - 1 - c->rlen, 0);
    if (n < 0) { if (errno == EAGAIN) return; n = 0; }
    if (n == 0) { /* closed */
      sock_errs++;
      epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, 0); close(c->fd);
      c->state = 0; c->step = 0; arm(i, 100);
      return;
    }
    c->rlen += (int)n;
    c->rbuf[c->rlen] = 0;
    if (c->state != 3) { c->rlen = 0; continue; }
    if (c->need < 0) {
      char *e = memmem(c->rbuf, c->rlen, "\r\n\r\n", 4);
      if (!e) continue;
      char *cl = memmem(c->rbuf, e - c->rbuf, "Content-Length: ", 16);
      c->need = (int)(e - c->rbuf) + 4 + (cl ? atoi(cl + 16) : 0);
    }
    if (c->rlen >= c->need) { done(i); if (C[i].state != 3) return; }
  }
}


static void pct(int w, const char *name) {
  uint64_t n = count[w], acc = 0;
  double p50 = 0, p95 = 0, p99 = 0, p999 = 0, mx = 0;
  int got = 0;
  for (int b = 0; b <= NB; b++) {
    if (!hist[w][b]) continue;
    acc += hist[w][b];
    double ms = b / 10.0;
    mx = ms;
    if (!(got & 1) && acc * 100 >= n * 50) { p50 = ms; got |= 1; }
    if (!(got & 2) && acc * 100 >= n * 95) { p95 = ms; got |= 2; }
    if (!(got & 4) && acc * 100 >= n * 99) { p99 = ms; got |= 4; }
    if (!(got & 8) && acc * 1000 >= n * 999) { p999 = ms; got |= 8; }
  }
  uint64_t o100 = 0, o250 = 0, o500 = 0, o1000 = 0;
  for (int b = 0; b <= NB; b++) {
    if (b >= 1000) o100 += hist[w][b];
    if (b >= 2500) o250 += hist[w][b];
    if (b >= 5000) o500 += hist[w][b];
    if (b >= 10000) o1000 += hist[w][b];
  }
  printf("%-6s n=%llu p50=%.1f p95=%.1f p99=%.1f p99.9=%.1f max=%.1f ms | over 100ms %.2f%% 250ms %.2f%% 500ms %.2f%% 1s %.2f%%\n",
         name, (unsigned long long)n, p50, p95, p99, p999, mx,
         n ? 100.0 * o100 / n : 0, n ? 100.0 * o250 / n : 0, n ? 100.0 * o500 / n : 0, n ? 100.0 * o1000 / n : 0);

}

int main(int argc, char **argv) {
  if (argc < 7) { fprintf(stderr, "usage: loadgen HOST PORT CONNS SECONDS THINK_MS TOKENS.json [LIKE%%] [POST%%] [RAMP_S]\n"); return 2; }
  host = argv[1]; port = atoi(argv[2]); NC = atoi(argv[3]);
  int secs = atoi(argv[4]); think_ms = atoi(argv[5]);
  if (argc > 7) like_pct = atoi(argv[7]);
  if (argc > 8) post_pct = atoi(argv[8]);
  int ramp = argc > 9 ? atoi(argv[9]) : 5;
  FILE *f = fopen(argv[6], "rb");
  if (!f) { perror("tokens"); return 1; }
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  char *tj = malloc(sz + 1);
  if (fread(tj, 1, sz, f) != (size_t)sz) return 1;
  tj[sz] = 0; fclose(f);
  tokens = malloc(sizeof(char *) * 20000);
  for (char *p = tj; (p = strstr(p, "\"token\":\"")) && ntok < 20000;) {
    p += 9; char *e = strchr(p, '"'); *e = 0; tokens[ntok++] = p; p = e + 1;
  }
  if (!ntok) { fprintf(stderr, "no tokens\n"); return 1; }
  C = calloc(NC, sizeof(Conn));
  for (int i = 0; i < WHEEL; i++) wheel[i] = -1;
  ep = epoll_create1(0);
  t_start = now_us(); t_measure = t_start + (uint64_t)ramp * 1000000;
  wheel_ms = 0;
  for (int i = 0; i < NC; i++) {
    C[i].rbuf = malloc(RBUF); C[i].tok = i % ntok; C[i].fd = -1;
    arm(i, 1 + (int)(rnd() % (uint64_t)(ramp * 1000)));
  }
  uint64_t t_end = t_start + (uint64_t)(secs + ramp) * 1000000;
  struct epoll_event evs[1024];
  for (;;) {
    uint64_t t = now_us();
    if (t >= t_end + 1500000) break;
    if (t >= t_end) draining = 1;   /* no new requests; collect the answers still on their way */
    uint64_t target = (t - t_start) / 1000;
    while (wheel_ms < target) { /* fire timers */
      wheel_ms++;
      int s = (int)(wheel_ms % WHEEL), i = wheel[s];
      wheel[s] = -1;
      while (i >= 0) {
        int nx = C[i].next;
        if (C[i].state == 0) do_connect(i);
        else if (C[i].state == 2) send_next(i);
        i = nx;
      }
    }
    int n = epoll_wait(ep, evs, 1024, 1);
    for (int k = 0; k < n; k++) {
      int i = (int)evs[k].data.u32;
      Conn *c = &C[i];
      if (c->state == 1 && (evs[k].events & EPOLLOUT)) {
        struct epoll_event ev = {.events = EPOLLIN | EPOLLRDHUP, .data.u32 = (uint32_t)i};
        epoll_ctl(ep, EPOLL_CTL_MOD, c->fd, &ev);
        c->state = 2; c->step = 0;
        send_next(i);
        continue;
      }
      if (evs[k].events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) on_read(i);
    }
  }
  double dur = (double)secs;
  printf("conns=%d think=%dms like=%d%% post=%d%% hold=%ds\n", NC, think_ms, like_pct, post_pct, secs);
  printf("requests=%llu (%.0f/s) writes=%llu (%.0f/s) http_errors=%llu socket_errors=%llu\n",
         (unsigned long long)(count[0] + count[1]), (count[0] + count[1]) / dur, (unsigned long long)count[1], count[1] / dur,
         (unsigned long long)errs, (unsigned long long)sock_errs);
  printf("acked (whole run, ramp included): new likes=%llu repeat likes=%llu posts=%llu\n",
         (unsigned long long)like201, (unsigned long long)like200, (unsigned long long)post201);
  pct(0, "reads"); pct(1, "writes");
  printf("worst read latency per second (ms):");
  for (int s = ramp; s < secs + ramp && s < 4096; s++) printf(" %u", sec_max[s]);
  printf("\nworst write latency per second (ms):");
  for (int s = ramp; s < secs + ramp && s < 4096; s++) printf(" %u", sec_wmax[s]);
  printf("\n");
  return 0;
}
