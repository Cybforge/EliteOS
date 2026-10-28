/*
 * Sockets for System V programs.
 *
 *   AF_UNIX   stream, datagram and seqpacket, named by file path or abstract name, with descriptor passing
 *   AF_INET   stream and datagram over the kernel's own TCP/IP stack; connections inside the host never leave it
 *
 * Everything that waits sleeps on the system-wide readiness counter (sv_poll_gen), which every state change
 * below bumps, so blocking calls, poll, select and epoll all see the same events.
 */
#include <kernel.h>
#include <sched.h>
#include <mm.h>
#include <umm.h>
#include <net.h>
#include <abierr.h>
#include "sv.h"
#include "svcalls.h"

#define AF_UNSPEC 0
#define AF_UNIX   1
#define AF_INET   2

#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define SOCK_SEQPACKET 5
#define SOCK_NONBLOCK  04000
#define SOCK_CLOEXEC   02000000

#define MSG_OOB        0x01
#define MSG_PEEK       0x02
#define MSG_TRUNC      0x20
#define MSG_DONTWAIT   0x40
#define MSG_WAITALL    0x100
#define MSG_NOSIGNAL   0x4000
#define MSG_CTRUNC     0x08
#define MSG_CMSG_CLOEXEC 0x40000000

#define SOL_SOCKET 1
#define SCM_RIGHTS 1
#define SCM_CREDENTIALS 2

#define POLLIN    0x001
#define POLLOUT   0x004
#define POLLERR   0x008
#define POLLHUP   0x010
#define POLLRDHUP 0x2000

#define O_NONBLOCK 04000

#define STREAM_BUF  (128 * 1024)
#define MSGQ_BYTES  (208 * 1024)
#define SCM_MAX_FDS 253
#define CHUNK       (64 * 1024)
#define UNIX_PATH   108

enum { SS_NEW, SS_LISTEN, SS_CONNECTING, SS_CONNECTED };
enum { K_UNIX, K_INET_STREAM, K_INET_DGRAM };

/* descriptors (and credentials) travelling with data */
struct scm {
    struct scm *next;
    uint64_t pos;                   /* stream: the position of the first byte they belong to */
    int nfds;
    struct sfile **f;
};

struct umsg {
    struct umsg *next;
    size_t len;
    uint8_t *data;
    struct raddr_u { uint8_t len; char path[UNIX_PATH]; } from;
    struct scm *scm;
};

struct raddr {
    int family;
    uint8_t ulen;                   /* unix: bytes of path in use (the abstract name includes its leading NUL) */
    char path[UNIX_PATH];
    uint32_t ip;
    uint16_t port;
};

struct usock {
    struct usock *all_next;
    int kind, type, proto;
    int state;
    /* options */
    int rcvbuf, sndbuf;
    bool reuseaddr, reuseport, keepalive, passcred, broadcast, nodelay;
    uint64_t rcvtimeo_us, sndtimeo_us;
    int err;                        /* pending SO_ERROR */
    int pid;
    uint32_t uid, gid;
    int peer_pid;
    uint32_t peer_uid, peer_gid;
    /* addresses */
    bool bound;
    bool accepted;                  /* the server's end of a connection: shares its listener's name and port */
    struct raddr_u uname;           /* unix: our name */
    uint32_t lip;                   /* inet */
    uint16_t lport;
    bool connected_addr;
    uint32_t rip;
    uint16_t rport;
    /* links */
    struct usock *peer;
    bool peer_gone;
    bool rd_shut, wr_shut, peer_wr_shut;
    bool loop;                      /* an inet stream whose other end is inside the host */
    /* stream receive */
    uint8_t *ring;
    size_t rhead, rcount;
    uint64_t rin, rout;
    struct scm *scm_head, *scm_tail;
    /* message receive */
    struct umsg *mq_head, *mq_tail;
    size_t mq_bytes;
    /* listening */
    int backlog, nacc;
    struct usock *acc_head, *acc_tail, *acc_next;
    /* the stack */
    tcp_sock_t *tcp;
    tcp_listener_t *tcpl;
    udp_ep_t *udp;
};

static spinlock_t sk_lock = SPINLOCK_INIT("sockets");
static struct usock *all_socks;
static bool hooked;
static uint16_t next_loop_port = 40000;

/* ------------------------------------------------------------------------
 * helpers
 * ---------------------------------------------------------------------- */

static inline bool is_ring(const struct usock *u) { return u->ring != NULL; }
static inline bool is_msg(const struct usock *u) { return u->kind == K_UNIX && u->type != SOCK_STREAM; }

static void scm_free(struct scm *s)
{
    if (!s) return;
    for (int i = 0; i < s->nfds; i++) sfile_put(s->f[i]);
    kfree(s->f);
    kfree(s);
}

static void umsg_free(struct umsg *m)
{
    scm_free(m->scm);
    kfree(m->data);
    kfree(m);
}

static struct usock *sk_new(int kind, int type, int proto)
{
    struct usock *u = kzalloc(sizeof(*u));
    if (!u) return NULL;
    struct sv_group *g = sv_curgrp();
    u->kind = kind;
    u->type = type;
    u->proto = proto;
    u->rcvbuf = u->sndbuf = 212992;
    u->pid = g ? g->tgid : 0;
    u->uid = g ? g->euid : 0;
    u->gid = g ? g->egid : 0;
    spin_lock(&sk_lock);
    u->all_next = all_socks;
    all_socks = u;
    spin_unlock(&sk_lock);
    return u;
}

static void sk_unlink_all(struct usock *u)
{
    for (struct usock **pp = &all_socks; *pp; pp = &(*pp)->all_next)
        if (*pp == u) { *pp = u->all_next; break; }
}

/* a socket that never got a descriptor */
static void sk_discard(struct usock *u)
{
    spin_lock(&sk_lock);
    sk_unlink_all(u);
    spin_unlock(&sk_lock);
    kfree(u->ring);
    kfree(u);
}

/* wait for something to change; 0 woken, -EAGAIN time is up, -EINTR a signal */
static long sk_sleep(uint64_t gen, uint64_t deadline_ns)
{
    uint64_t ms = 0;
    if (deadline_ns) {
        uint64_t now = sv_now_ns();
        if (now >= deadline_ns) return -ERR_AGAIN;
        ms = (deadline_ns - now + 999999) / 1000000;
    }
    long w = sv_poll_wait_gen(gen, ms);
    if (w < 0) return w;
    return w == 1 ? -ERR_AGAIN : 0;
}

static uint64_t make_deadline(uint64_t timeo_us)
{
    return timeo_us ? sv_now_ns() + timeo_us * 1000 : 0;
}

static void raise_sigpipe(int flags)
{
    if (flags & MSG_NOSIGNAL) return;
    struct sv_task *t = sv_cur();
    if (t) sv_send_signal(t->g, t, SIGPIPE);
}

static bool nonblocking(struct sfile *f, int flags) { return (flags & MSG_DONTWAIT) || (f && (f->flags & O_NONBLOCK)); }

/* ------------------------------------------------------------------------
 * names
 * ---------------------------------------------------------------------- */

static void ipport_to_sockaddr(uint32_t ip, uint16_t port, uint8_t *sa)
{
    memset(sa, 0, 16);
    sa[0] = AF_INET;
    sa[2] = (uint8_t)(port >> 8);
    sa[3] = (uint8_t)port;
    sa[4] = (uint8_t)(ip >> 24);
    sa[5] = (uint8_t)(ip >> 16);
    sa[6] = (uint8_t)(ip >> 8);
    sa[7] = (uint8_t)ip;
}

/* read a sockaddr from user memory */
static long read_addr(uint64_t uaddr, uint32_t len, struct raddr *a)
{
    memset(a, 0, sizeof(*a));
    if (len < 2 || len > 128) return -ERR_INVAL;
    uint8_t buf[128];
    if (copy_from_user(buf, uaddr, len)) return -ERR_FAULT;
    a->family = buf[0] | (buf[1] << 8);
    if (a->family == AF_INET) {
        if (len < 16) return -ERR_INVAL;
        a->port = (uint16_t)((buf[2] << 8) | buf[3]);
        a->ip = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) | ((uint32_t)buf[6] << 8) | buf[7];
    } else if (a->family == AF_UNIX) {
        size_t pl = len - 2;
        if (pl > UNIX_PATH) pl = UNIX_PATH;
        if (pl == 0) { a->ulen = 0; return 0; }
        memcpy(a->path, buf + 2, pl);
        if (a->path[0] == 0) {
            a->ulen = (uint8_t)pl;                          /* abstract: as given */
        } else {
            size_t n = strnlen(a->path, pl);
            a->ulen = (uint8_t)(n + 1);                     /* path including its NUL */
            if (n < UNIX_PATH) a->path[n] = 0;
        }
    }
    return 0;
}

/* write a sockaddr to user memory, honouring the caller's buffer size, and report the real size */
static long write_addr(uint64_t uaddr, uint64_t ulen, const uint8_t *sa, uint32_t salen)
{
    if (!uaddr || !ulen) return 0;
    uint32_t have;
    if (copy_from_user(&have, ulen, 4)) return -ERR_FAULT;
    uint32_t n = MIN(have, salen);
    if (n && copy_to_user(uaddr, sa, n)) return -ERR_FAULT;
    if (copy_to_user(ulen, &salen, 4)) return -ERR_FAULT;
    return 0;
}

static uint32_t unix_sockaddr(const struct raddr_u *n, uint8_t *sa)
{
    memset(sa, 0, 2 + UNIX_PATH);
    sa[0] = AF_UNIX;
    memcpy(sa + 2, n->path, n->len);
    return 2 + n->len;
}

static struct usock *find_unix(const struct raddr *a, int type)
{
    for (struct usock *u = all_socks; u; u = u->all_next)
        if (u->kind == K_UNIX && u->bound && !u->accepted && u->uname.len == a->ulen &&
            !memcmp(u->uname.path, a->path, a->ulen) && (type == 0 || u->type == type))
            return u;
    return NULL;
}

static bool unix_name_taken(const struct raddr *a)
{
    for (struct usock *u = all_socks; u; u = u->all_next)
        if (u->kind == K_UNIX && u->bound && !u->accepted && u->uname.len == a->ulen && !memcmp(u->uname.path, a->path, a->ulen))
            return true;
    return false;
}

static bool inet_ip_ok(uint32_t ip) { return ip == 0 || net_is_local_ip(ip); }

static bool inet_port_taken(const struct usock *self, int kind, uint32_t ip, uint16_t port)
{
    for (struct usock *u = all_socks; u; u = u->all_next) {
        if (u == self || u->kind != kind || !u->bound || u->accepted || u->lport != port) continue;
        if (u->lip && ip && u->lip != ip) continue;
        if (kind == K_INET_DGRAM && ((u->reuseport && self->reuseport) || (u->reuseaddr && self->reuseaddr))) continue;
        if (kind == K_INET_STREAM && u->reuseaddr && self->reuseaddr && u->state != SS_LISTEN) continue;
        return true;
    }
    return false;
}

static uint16_t pick_port(const struct usock *self, int kind)
{
    for (int k = 0; k < 20000; k++) {
        uint16_t p = (uint16_t)(32768 + (next_loop_port++ % 28000));
        if (!inet_port_taken(self, kind, 0, p)) return p;
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * stream ring and message queue primitives (sk_lock held)
 * ---------------------------------------------------------------------- */

static size_t ring_put(struct usock *d, const uint8_t *p, size_t n)
{
    size_t c = MIN(n, STREAM_BUF - d->rcount);
    size_t tail = (d->rhead + d->rcount) % STREAM_BUF, first = MIN(c, (size_t)STREAM_BUF - tail);
    memcpy(d->ring + tail, p, first);
    memcpy(d->ring, p + first, c - first);
    d->rcount += c;
    d->rin += c;
    return c;
}

static size_t ring_get(struct usock *u, uint8_t *p, size_t n)
{
    size_t c = MIN(n, u->rcount), first = MIN(c, (size_t)STREAM_BUF - u->rhead);
    memcpy(p, u->ring + u->rhead, first);
    memcpy(p + first, u->ring, c - first);
    u->rhead = (u->rhead + c) % STREAM_BUF;
    u->rcount -= c;
    u->rout += c;
    return c;
}

static void scm_append(struct usock *d, struct scm *s)
{
    s->next = NULL;
    if (d->scm_tail) d->scm_tail->next = s; else d->scm_head = s;
    d->scm_tail = s;
}

static bool ring_alloc(struct usock *u)
{
    u->ring = kmalloc(STREAM_BUF);
    return u->ring != NULL;
}

/* two sockets become each other's peer */
static void link_pair(struct usock *a, struct usock *b)
{
    a->peer = b;
    b->peer = a;
    a->peer_pid = b->pid; a->peer_uid = b->uid; a->peer_gid = b->gid;
    b->peer_pid = a->pid; b->peer_uid = a->uid; b->peer_gid = a->gid;
    a->state = b->state = SS_CONNECTED;
}

/* ------------------------------------------------------------------------
 * close
 * ---------------------------------------------------------------------- */

static void sk_close(struct sfile *f)
{
    struct usock *u = f->priv;
    struct scm *doomed_scm = NULL;
    struct umsg *doomed_msgs = NULL;
    struct usock *doomed_acc = NULL;

    spin_lock(&sk_lock);
    sk_unlink_all(u);
    if (u->peer && u->peer->peer == u) {
        struct usock *p = u->peer;
        p->peer = NULL;
        p->peer_gone = true;
    }
    /* datagram sockets that had us as their destination */
    for (struct usock *o = all_socks; o; o = o->all_next)
        if (o->peer == u) { o->peer = NULL; o->err = ERR_CONNREFUSED; }
    u->peer = NULL;
    doomed_scm = u->scm_head;
    u->scm_head = u->scm_tail = NULL;
    doomed_msgs = u->mq_head;
    u->mq_head = u->mq_tail = NULL;
    doomed_acc = u->acc_head;
    u->acc_head = u->acc_tail = NULL;
    u->nacc = 0;
    /* connections nobody accepted: their other ends see a dead peer */
    for (struct usock *a = doomed_acc; a; a = a->acc_next) {
        if (a->peer) { a->peer->peer = NULL; a->peer->peer_gone = true; a->peer->err = ERR_CONNRESET; }
        a->peer = NULL;
        sk_unlink_all(a);
    }
    spin_unlock(&sk_lock);

    while (doomed_scm) { struct scm *n = doomed_scm->next; scm_free(doomed_scm); doomed_scm = n; }
    while (doomed_msgs) { struct umsg *n = doomed_msgs->next; umsg_free(doomed_msgs); doomed_msgs = n; }
    while (doomed_acc) {
        struct usock *n = doomed_acc->acc_next;
        while (doomed_acc->scm_head) { struct scm *s = doomed_acc->scm_head->next; scm_free(doomed_acc->scm_head); doomed_acc->scm_head = s; }
        kfree(doomed_acc->ring);
        kfree(doomed_acc);
        doomed_acc = n;
    }
    if (u->tcp) tcp_release(u->tcp);
    if (u->tcpl) tcp_listen_close(u->tcpl);
    if (u->udp) udp_ep_close(u->udp);
    kfree(u->ring);
    kfree(u);
    sv_poll_wake();
}

/* ------------------------------------------------------------------------
 * poll
 * ---------------------------------------------------------------------- */

static uint32_t sk_poll(struct sfile *f, uint32_t ev)
{
    struct usock *u = f->priv;
    uint32_t r = 0;
    UNUSED(ev);
    if (u->kind == K_INET_STREAM && !u->loop) {
        if (u->state == SS_LISTEN) {
            spin_lock(&sk_lock);
            bool ready = u->nacc > 0 || (u->tcpl && tcp_listener_ready(u->tcpl));
            spin_unlock(&sk_lock);
            return ready ? POLLIN : 0;
        }
        if (!u->tcp) return u->state == SS_NEW ? POLLOUT | POLLHUP : POLLHUP;
        uint32_t t = tcp_flags(u->tcp);
        if (u->state == SS_CONNECTING) {
            if (t & TCPF_WRITE) { u->state = SS_CONNECTED; return POLLOUT; }
            if (t & (TCPF_ERR | TCPF_CLOSED)) {
                u->err = (t & TCPF_TIMEDOUT) ? ERR_TIMEDOUT : ERR_CONNREFUSED;
                return POLLERR | POLLHUP | POLLOUT;
            }
            return 0;
        }
        if (t & TCPF_READ) r |= POLLIN;
        if (t & TCPF_EOF) r |= POLLIN | POLLRDHUP | ((t & TCPF_CLOSED) ? POLLHUP : 0);
        if (t & TCPF_WRITE) r |= POLLOUT;
        if (t & TCPF_ERR) r |= POLLERR | POLLHUP;
        return r;
    }
    if (u->kind == K_INET_DGRAM) {
        r = POLLOUT;
        if (u->udp && udp_ep_ready(u->udp)) r |= POLLIN;
        if (u->err) r |= POLLERR;
        return r;
    }
    spin_lock(&sk_lock);
    if (u->state == SS_LISTEN) {
        if (u->nacc > 0 || (u->tcpl && tcp_listener_ready(u->tcpl))) r |= POLLIN;
    } else if (is_ring(u)) {
        if (u->rcount) r |= POLLIN;
        if (u->rd_shut || u->peer_wr_shut || u->peer_gone) r |= POLLIN | POLLRDHUP;
        if (u->peer_gone && !u->rcount) r |= POLLHUP;
        if (u->peer && !u->peer->rd_shut && !u->wr_shut && u->peer->rcount < STREAM_BUF) r |= POLLOUT;
        if (u->wr_shut || u->peer_gone || (u->peer && u->peer->rd_shut)) r |= POLLOUT;     /* the write fails at once */
    } else {                                                /* message sockets */
        if (u->mq_head) r |= POLLIN;
        if (u->peer_gone && !u->mq_head && u->type == SOCK_SEQPACKET) r |= POLLHUP | POLLIN;
        if (!u->peer || u->peer->mq_bytes < MSGQ_BYTES) r |= POLLOUT;
    }
    if (u->err) r |= POLLERR;
    spin_unlock(&sk_lock);
    return r;
}

/* ------------------------------------------------------------------------
 * stream write and read through a ring (unix streams, loopback TCP)
 * ---------------------------------------------------------------------- */

static long ring_send(struct sfile *f, struct usock *u, const uint8_t *buf, size_t n, int flags, struct scm **scm)
{
    size_t done = 0;
    uint64_t deadline = make_deadline(u->sndtimeo_us);
    for (;;) {
        uint64_t gen = sv_poll_gen();
        spin_lock(&sk_lock);
        struct usock *d = u->peer;
        if (u->wr_shut || !d || d->rd_shut) {
            spin_unlock(&sk_lock);
            if (!done) { raise_sigpipe(flags); return -ERR_PIPE; }
            return (long)done;
        }
        size_t room = STREAM_BUF - d->rcount;
        if (room && (done < n || n == 0)) {
            size_t c = MIN(n - done, room);
            if (*scm) { (*scm)->pos = d->rin; scm_append(d, *scm); *scm = NULL; }
            if (c) ring_put(d, buf + done, c);
            done += c;
            spin_unlock(&sk_lock);
            sv_poll_wake();
            if (done >= n) return (long)done;
            continue;
        }
        spin_unlock(&sk_lock);
        if (done >= n) return (long)done;
        if (nonblocking(f, flags)) return done ? (long)done : -ERR_AGAIN;
        long w = sk_sleep(gen, deadline);
        if (w < 0) return done ? (long)done : w;
    }
}

/* up to n bytes; descriptors that arrive with them are handed back in *scm (or dropped when scm is NULL) */
static long ring_recv(struct sfile *f, struct usock *u, uint8_t *buf, size_t n, int flags, struct scm **scm_out)
{
    uint64_t deadline = make_deadline(u->rcvtimeo_us);
    bool peek = flags & MSG_PEEK;
    for (;;) {
        uint64_t gen = sv_poll_gen();
        spin_lock(&sk_lock);
        if (u->rcount) {
            size_t want = n;
            struct scm *s = u->scm_head;
            struct scm *take = NULL;
            if (s && !peek) {
                if (s->pos > u->rout) want = MIN(want, (size_t)(s->pos - u->rout));      /* stop before the next batch */
                else { take = s; u->scm_head = s->next; if (!u->scm_head) u->scm_tail = NULL; if (s->next) want = MIN(want, (size_t)(s->next->pos - u->rout)); }
            }
            size_t c;
            if (peek) {
                c = MIN(want, u->rcount);
                size_t first = MIN(c, (size_t)STREAM_BUF - u->rhead);
                memcpy(buf, u->ring + u->rhead, first);
                memcpy(buf + first, u->ring, c - first);
            } else {
                c = ring_get(u, buf, want);
            }
            spin_unlock(&sk_lock);
            if (take) { if (scm_out) *scm_out = take; else scm_free(take); }
            sv_poll_wake();                                    /* a writer may be waiting for room */
            return (long)c;
        }
        bool eof = u->rd_shut || u->peer_wr_shut || u->peer_gone || !u->peer;
        int err = u->err;
        if (eof && err && u->peer_gone) u->err = 0;
        spin_unlock(&sk_lock);
        if (eof) return err ? -err : 0;
        if (nonblocking(f, flags)) return -ERR_AGAIN;
        long w = sk_sleep(gen, deadline);
        if (w < 0) return w;
    }
}

/* ------------------------------------------------------------------------
 * messages (unix datagram and seqpacket)
 * ---------------------------------------------------------------------- */

static long msg_send(struct sfile *f, struct usock *u, struct usock *dst, const uint8_t *buf, size_t n, int flags,
                     struct scm **scm)
{
    if (n > MSGQ_BYTES / 2) return -ERR_MSGSIZE;
    struct umsg *m = kzalloc(sizeof(*m));
    if (!m) return -ERR_NOMEM;
    m->data = kmalloc(n ? n : 1);
    if (!m->data) { kfree(m); return -ERR_NOMEM; }
    memcpy(m->data, buf, n);
    m->len = n;
    m->from = u->uname;
    uint64_t deadline = make_deadline(u->sndtimeo_us);
    for (;;) {
        uint64_t gen = sv_poll_gen();
        spin_lock(&sk_lock);
        bool alive = false;
        for (struct usock *x = all_socks; x; x = x->all_next) if (x == dst) { alive = true; break; }
        if (!alive || dst->rd_shut) {
            spin_unlock(&sk_lock);
            umsg_free(m);
            if (u->type == SOCK_SEQPACKET) raise_sigpipe(flags);
            return u->type == SOCK_SEQPACKET ? -ERR_PIPE : -ERR_CONNREFUSED;
        }
        if (dst->mq_bytes + n <= MSGQ_BYTES || dst->mq_bytes == 0) {
            if (*scm) { m->scm = *scm; *scm = NULL; }
            if (dst->mq_tail) dst->mq_tail->next = m; else dst->mq_head = m;
            dst->mq_tail = m;
            dst->mq_bytes += n;
            spin_unlock(&sk_lock);
            sv_poll_wake();
            return (long)n;
        }
        spin_unlock(&sk_lock);
        if (nonblocking(f, flags)) { umsg_free(m); return -ERR_AGAIN; }
        long w = sk_sleep(gen, deadline);
        if (w < 0) { umsg_free(m); return w; }
    }
}

static long msg_recv(struct sfile *f, struct usock *u, uint8_t *buf, size_t cap, int flags, struct raddr *from,
                     struct scm **scm_out, bool *truncated)
{
    uint64_t deadline = make_deadline(u->rcvtimeo_us);
    for (;;) {
        uint64_t gen = sv_poll_gen();
        spin_lock(&sk_lock);
        struct umsg *m = u->mq_head;
        if (m) {
            size_t c = MIN(cap, m->len);
            memcpy(buf, m->data, c);
            *truncated = m->len > cap;
            long full = (long)m->len;
            if (from) {
                from->family = AF_UNIX;
                from->ulen = m->from.len;
                memcpy(from->path, m->from.path, UNIX_PATH);
            }
            if (!(flags & MSG_PEEK)) {
                u->mq_head = m->next;
                if (!u->mq_head) u->mq_tail = NULL;
                u->mq_bytes -= m->len;
                spin_unlock(&sk_lock);
                if (m->scm) { if (scm_out) { *scm_out = m->scm; m->scm = NULL; } }
                umsg_free(m);
                sv_poll_wake();
            } else {
                spin_unlock(&sk_lock);
            }
            return (flags & MSG_TRUNC) ? full : (long)c;
        }
        bool eof = u->type == SOCK_SEQPACKET && (u->peer_gone || u->rd_shut || u->peer_wr_shut);
        int err = u->err;
        if (err) u->err = 0;
        spin_unlock(&sk_lock);
        if (eof) return 0;
        if (err) return -err;
        if (nonblocking(f, flags)) return -ERR_AGAIN;
        long w = sk_sleep(gen, deadline);
        if (w < 0) return w;
    }
}

/* ------------------------------------------------------------------------
 * the generic send and receive on one flat kernel buffer
 * ---------------------------------------------------------------------- */

static long tcp_send_all(struct sfile *f, struct usock *u, const uint8_t *buf, size_t n, int flags)
{
    size_t done = 0;
    uint64_t deadline = make_deadline(u->sndtimeo_us);
    for (;;) {
        uint64_t gen = sv_poll_gen();
        if (!u->tcp || u->state != SS_CONNECTED) return done ? (long)done : -ERR_NOTCONN;
        if (u->wr_shut) { raise_sigpipe(flags); return done ? (long)done : -ERR_PIPE; }
        long k = tcp_send_nb(u->tcp, buf + done, n - done);
        if (k < 0) {
            uint32_t t = tcp_flags(u->tcp);
            if (done) return (long)done;
            if (t & TCPF_ERR) return -((t & TCPF_TIMEDOUT) ? ERR_TIMEDOUT : ERR_CONNRESET);
            raise_sigpipe(flags);
            return -ERR_PIPE;
        }
        done += (size_t)k;
        if (done >= n) return (long)done;
        if (nonblocking(f, flags)) return done ? (long)done : -ERR_AGAIN;
        long w = sk_sleep(gen, deadline);
        if (w < 0) return done ? (long)done : w;
    }
}

static long tcp_recv_some(struct sfile *f, struct usock *u, uint8_t *buf, size_t n, int flags)
{
    uint64_t deadline = make_deadline(u->rcvtimeo_us);
    for (;;) {
        uint64_t gen = sv_poll_gen();
        if (!u->tcp) return -ERR_NOTCONN;
        long k = tcp_recv(u->tcp, buf, n, 0);
        if (k >= 0) { if (k) sv_poll_wake(); return k; }
        if (k == -1) return -ERR_CONNRESET;
        if (nonblocking(f, flags)) return -ERR_AGAIN;
        long w = sk_sleep(gen, deadline);
        if (w < 0) return w;
    }
}

static long udp_send_to(struct usock *u, const uint8_t *buf, size_t n, const struct raddr *to)
{
    uint32_t ip = to ? to->ip : u->rip;
    uint16_t port = to ? to->port : u->rport;
    if (!ip && !port) return -ERR_DESTADDRREQ;
    if (!ip) ip = 0x7F000001;
    if (!u->udp) {
        u->udp = udp_ep_open(u->bound ? u->lport : 0);
        if (!u->udp) return -ERR_ADDRINUSE;
        u->lport = udp_ep_port(u->udp);
        u->bound = true;
    }
    long r = udp_ep_send(u->udp, ip, port, buf, n);
    if (r < 0) return n > 1464 ? -ERR_MSGSIZE : -ERR_NETUNREACH;
    return r;
}

static long udp_recv_one(struct sfile *f, struct usock *u, uint8_t *buf, size_t cap, int flags, struct raddr *from, bool *trunc)
{
    uint64_t deadline = make_deadline(u->rcvtimeo_us);
    uint8_t tmp[1500];
    for (;;) {
        uint64_t gen = sv_poll_gen();
        if (u->udp) {
            uint32_t fip; uint16_t fport;
            long n = udp_ep_recv(u->udp, tmp, sizeof(tmp), &fip, &fport, flags & MSG_PEEK);
            if (n >= 0) {
                if (u->rport && (fip != u->rip || fport != u->rport)) continue;     /* connected: only the peer is heard */
                size_t c = MIN(cap, (size_t)n);
                memcpy(buf, tmp, c);
                *trunc = (size_t)n > cap;
                if (from) { from->family = AF_INET; from->ip = fip; from->port = fport; }
                return (flags & MSG_TRUNC) ? n : (long)c;
            }
        }
        if (u->err) { int e = u->err; u->err = 0; return -e; }
        if (nonblocking(f, flags)) return -ERR_AGAIN;
        long w = sk_sleep(gen, deadline);
        if (w < 0) return w;
    }
}

static long do_send(struct sfile *f, struct usock *u, const uint8_t *buf, size_t n, int flags, const struct raddr *to,
                    struct scm **scm)
{
    if (flags & MSG_OOB) return -ERR_OPNOTSUPP;
    if (u->kind == K_INET_DGRAM) return udp_send_to(u, buf, n, to);
    if (u->kind == K_INET_STREAM && !u->loop) {
        if (u->state == SS_CONNECTING) return -ERR_AGAIN;
        return tcp_send_all(f, u, buf, n, flags);
    }
    if (u->kind == K_UNIX && u->type == SOCK_DGRAM) {
        struct usock *dst = NULL;
        if (to && to->ulen) {
            spin_lock(&sk_lock);
            dst = find_unix(to, SOCK_DGRAM);
            spin_unlock(&sk_lock);
            if (!dst) return -ERR_NOENT;
        } else {
            dst = u->peer;
            if (!dst) return -ERR_DESTADDRREQ;
        }
        return msg_send(f, u, dst, buf, n, flags, scm);
    }
    if (u->state != SS_CONNECTED && !(u->peer)) {
        if (u->peer_gone) { raise_sigpipe(flags); return -ERR_PIPE; }
        return -ERR_NOTCONN;
    }
    if (u->kind == K_UNIX && u->type == SOCK_SEQPACKET) {
        if (!u->peer) { raise_sigpipe(flags); return -ERR_PIPE; }
        return msg_send(f, u, u->peer, buf, n, flags, scm);
    }
    return ring_send(f, u, buf, n, flags, scm);
}

static long do_recv(struct sfile *f, struct usock *u, uint8_t *buf, size_t cap, int flags, struct raddr *from,
                    struct scm **scm_out, bool *trunc)
{
    *trunc = false;
    if (flags & MSG_OOB) return -ERR_INVAL;
    if (u->kind == K_INET_DGRAM) return udp_recv_one(f, u, buf, cap, flags, from, trunc);
    if (u->state == SS_LISTEN) return -ERR_INVAL;
    if (u->kind == K_INET_STREAM && !u->loop) {
        if (u->state == SS_CONNECTING) return -ERR_AGAIN;
        if (u->state != SS_CONNECTED) return -ERR_NOTCONN;
        long got = 0;
        if (flags & MSG_PEEK) return -ERR_OPNOTSUPP;
        got = tcp_recv_some(f, u, buf, cap, flags);
        while (got > 0 && (flags & MSG_WAITALL) && (size_t)got < cap) {
            long k = tcp_recv_some(f, u, buf + got, cap - (size_t)got, flags);
            if (k <= 0) break;
            got += k;
        }
        return got;
    }
    if (is_msg(u)) {
        if (u->type == SOCK_SEQPACKET && u->state != SS_CONNECTED && !u->peer_gone) return -ERR_NOTCONN;
        return msg_recv(f, u, buf, cap, flags, from, scm_out, trunc);
    }
    if (u->state != SS_CONNECTED && !u->peer_gone && !u->rcount) return -ERR_NOTCONN;
    long got = ring_recv(f, u, buf, cap, flags, scm_out);
    while (got > 0 && (flags & MSG_WAITALL) && (size_t)got < cap && !(scm_out && *scm_out)) {
        long k = ring_recv(f, u, buf + got, cap - (size_t)got, flags, scm_out);
        if (k <= 0) break;
        got += k;
    }
    return got;
}

/* ------------------------------------------------------------------------
 * sfile operations: read and write like any other file
 * ---------------------------------------------------------------------- */

static long sk_read(struct sfile *f, void *buf, size_t n, uint64_t *off)
{
    UNUSED(off);
    bool tr;
    return do_recv(f, f->priv, buf, n, 0, NULL, NULL, &tr);
}

static long sk_write(struct sfile *f, const void *buf, size_t n, uint64_t *off)
{
    UNUSED(off);
    struct scm *none = NULL;
    return do_send(f, f->priv, buf, n, 0, NULL, &none);
}

static int sk_stat(struct sfile *f, struct vfs_stat *st, uint32_t *mode, uint64_t *ino)
{
    st->type = VN_FILE;
    st->size = 0;
    st->mtime = 0;
    *mode = 0140777;                                        /* S_IFSOCK */
    *ino = (uint64_t)(uintptr_t)f->priv >> 4;
    return 0;
}

static long iface_ioctl(unsigned long cmd, uint64_t arg);

static long sk_ioctl(struct sfile *f, unsigned long cmd, uint64_t arg)
{
    struct usock *u = f->priv;
    if (cmd == 0x541B) {                                    /* FIONREAD */
        int v = 0;
        spin_lock(&sk_lock);
        if (is_ring(u)) v = (int)u->rcount;
        else if (is_msg(u)) v = u->mq_head ? (int)u->mq_head->len : 0;
        spin_unlock(&sk_lock);
        if (u->kind == K_INET_STREAM && !u->loop && u->tcp) v = (int)tcp_readable(u->tcp);
        if (u->kind == K_INET_DGRAM && u->udp && udp_ep_ready(u->udp)) {
            uint8_t tmp[1500];
            long n = udp_ep_recv(u->udp, tmp, sizeof(tmp), NULL, NULL, true);
            v = n > 0 ? (int)n : 0;
        }
        return copy_to_user(arg, &v, 4) ? -ERR_FAULT : 0;
    }
    if (cmd == 0x5411) {                                    /* TIOCOUTQ / SIOCOUTQ */
        int v = 0;
        if (u->kind == K_INET_STREAM && !u->loop && u->tcp) v = (int)tcp_unsent(u->tcp);
        return copy_to_user(arg, &v, 4) ? -ERR_FAULT : 0;
    }
    if (cmd >= 0x8900 && cmd < 0x8950) return iface_ioctl(cmd, arg);
    return -ERR_NOTTY;
}

static const struct sfile_ops sock_ops = {
    .read = sk_read, .write = sk_write, .poll = sk_poll, .ioctl = sk_ioctl, .stat = sk_stat, .close = sk_close,
    .name = "socket",
};

static struct usock *file_sock(struct sfile *f) { return f && f->ops == &sock_ops ? f->priv : NULL; }

/* ------------------------------------------------------------------------
 * creating sockets
 * ---------------------------------------------------------------------- */

static void hook_net(void)
{
    if (hooked) return;
    hooked = true;
    net_activity_hook = sv_poll_wake;
}

static int install_sock(struct usock *u, int flags, struct sfile **out)
{
    struct sfile *f = sfile_new(&sock_ops, 2 | (flags & SOCK_NONBLOCK ? O_NONBLOCK : 0));
    if (!f) return -ERR_NOMEM;
    f->priv = u;
    strlcpy(f->path, "socket:[sock]", sizeof(f->path));
    int fd = sv_fd_alloc(sv_curgrp(), f, 0, (flags & SOCK_CLOEXEC) != 0);
    if (fd < 0) {
        /* the file owns the socket now: closing it frees it */
        sfile_put(f);
        return fd;
    }
    if (out) *out = f;
    return fd;
}

static long make_socket(int domain, int type, int proto)
{
    int base = type & 0xF, flags = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);
    hook_net();
    int kind;
    if (domain == AF_UNIX) {
        if (base != SOCK_STREAM && base != SOCK_DGRAM && base != SOCK_SEQPACKET) return -ERR_PROTONOSUPPORT;
        if (proto) return -ERR_PROTONOSUPPORT;
        kind = K_UNIX;
    } else if (domain == AF_INET) {
        if (base == SOCK_STREAM) { if (proto && proto != 6) return -ERR_PROTONOSUPPORT; kind = K_INET_STREAM; }
        else if (base == SOCK_DGRAM) { if (proto && proto != 17) return -ERR_PROTONOSUPPORT; kind = K_INET_DGRAM; }
        else return -ERR_PROTONOSUPPORT;                   /* raw and ICMP sockets are not offered */
    } else {
        return -ERR_AFNOSUPPORT;
    }
    struct usock *u = sk_new(kind, base, proto);
    if (!u) return -ERR_NOMEM;
    if (kind == K_UNIX && base == SOCK_STREAM && !ring_alloc(u)) { sk_discard(u); return -ERR_NOMEM; }
    return install_sock(u, flags, NULL);
}

SC(s_socket) { return make_socket((int)r->rdi, (int)r->rsi, (int)r->rdx); }

SC(s_socketpair)
{
    int domain = (int)r->rdi, type = (int)r->rsi, base = type & 0xF, flags = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (domain != AF_UNIX) return -ERR_AFNOSUPPORT;
    if (base != SOCK_STREAM && base != SOCK_DGRAM && base != SOCK_SEQPACKET) return -ERR_PROTONOSUPPORT;
    hook_net();
    struct usock *a = sk_new(K_UNIX, base, 0), *b = sk_new(K_UNIX, base, 0);
    if (!a || !b || (base == SOCK_STREAM && (!ring_alloc(a) || !ring_alloc(b)))) {
        if (a) sk_discard(a);
        if (b) sk_discard(b);
        return -ERR_NOMEM;
    }
    spin_lock(&sk_lock);
    link_pair(a, b);
    spin_unlock(&sk_lock);
    int fa = install_sock(a, flags, NULL);
    if (fa < 0) { sk_discard(b); return fa; }
    int fb = install_sock(b, flags, NULL);
    if (fb < 0) { sv_fd_close(sv_curgrp(), fa); return fb; }     /* b was freed by the failed install */
    int fds[2] = { fa, fb };
    if (copy_to_user(r->r10, fds, sizeof(fds))) { sv_fd_close(sv_curgrp(), fa); sv_fd_close(sv_curgrp(), fb); return -ERR_FAULT; }
    return 0;
}

/* ------------------------------------------------------------------------
 * bind, listen, connect, accept
 * ---------------------------------------------------------------------- */

static long unix_bind(struct usock *u, struct raddr *a)
{
    char path[VFS_PATH_MAX];
    if (u->bound) return -ERR_INVAL;
    if (a->ulen == 0) {                                     /* autobind: a fresh abstract name */
        static uint32_t counter;
        spin_lock(&sk_lock);
        for (;;) {
            a->path[0] = 0;
            uint32_t v = ++counter & 0xFFFFF;
            for (int i = 0; i < 5; i++) a->path[1 + i] = "0123456789abcdef"[(v >> (4 * i)) & 15];
            a->ulen = 6;
            if (!unix_name_taken(a)) break;
        }
        u->uname.len = a->ulen;
        memcpy(u->uname.path, a->path, UNIX_PATH);
        u->bound = true;
        spin_unlock(&sk_lock);
        return 0;
    }
    if (a->path[0] != 0) {
        struct sv_group *g = sv_curgrp();
        if (!vfs_resolve(g->cwd, a->path, path, sizeof(path))) return -ERR_NAMETOOLONG;
        if (vfs_lookup(path)) return -ERR_ADDRINUSE;
        char dir[VFS_PATH_MAX];
        vfs_dirname(path, dir, sizeof(dir));
        if (!vfs_lookup(dir)) return -ERR_NOENT;
        vnode_t *vn = vfs_open(path, true);
        if (!vn) return -ERR_ACCES;
        vfs_close(vn);
        memset(a->path, 0, UNIX_PATH);
        size_t n = MIN(strlen(path), (size_t)UNIX_PATH - 1);
        memcpy(a->path, path, n);
        a->ulen = (uint8_t)(n + 1);
    }
    spin_lock(&sk_lock);
    if (a->path[0] != 0) {
        /* the file did not exist a moment ago, so a socket still holding this name lost it when the file was removed */
        for (struct usock *o; (o = find_unix(a, 0)) != NULL;) o->bound = false;
    } else if (unix_name_taken(a)) {
        spin_unlock(&sk_lock);
        return -ERR_ADDRINUSE;
    }
    u->uname.len = a->ulen;
    memcpy(u->uname.path, a->path, UNIX_PATH);
    u->bound = true;
    spin_unlock(&sk_lock);
    return 0;
}

static long inet_bind(struct usock *u, const struct raddr *a)
{
    if (u->bound) return -ERR_INVAL;
    if (!inet_ip_ok(a->ip)) return -ERR_ADDRNOTAVAIL;
    spin_lock(&sk_lock);
    uint16_t port = a->port ? a->port : pick_port(u, u->kind);
    if (!port || (a->port && inet_port_taken(u, u->kind, a->ip, port))) { spin_unlock(&sk_lock); return -ERR_ADDRINUSE; }
    u->lip = a->ip;
    u->lport = port;
    u->bound = true;
    spin_unlock(&sk_lock);
    if (u->kind == K_INET_DGRAM) {
        u->udp = udp_ep_open(port);
        if (!u->udp) { u->bound = false; return -ERR_ADDRINUSE; }
    }
    return 0;
}

SC(s_bind)
{
    struct sfile *f = sv_fd_get(sv_curgrp(), (int)r->rdi);
    if (!f) return -ERR_BADF;
    struct usock *u = file_sock(f);
    long rc;
    struct raddr a;
    if (!u) rc = -ERR_NOTSOCK;
    else if ((rc = read_addr(r->rsi, (uint32_t)r->rdx, &a)) < 0) { }
    else if (u->kind == K_UNIX) rc = a.family == AF_UNIX ? unix_bind(u, &a) : -ERR_INVAL;
    else rc = a.family == AF_INET ? inet_bind(u, &a) : -ERR_AFNOSUPPORT;
    sfile_put(f);
    return rc;
}

SC(s_listen)
{
    struct sfile *f = sv_fd_get(sv_curgrp(), (int)r->rdi);
    if (!f) return -ERR_BADF;
    struct usock *u = file_sock(f);
    long rc = 0;
    int backlog = (int)r->rsi;
    if (!u) rc = -ERR_NOTSOCK;
    else if (u->type != SOCK_STREAM && u->type != SOCK_SEQPACKET) rc = -ERR_OPNOTSUPP;
    else if (u->state == SS_CONNECTED || u->state == SS_CONNECTING) rc = -ERR_INVAL;
    else {
        if (backlog < 1) backlog = 1;
        if (backlog > 128) backlog = 128;
        if (u->kind == K_UNIX && !u->bound) {
            struct raddr a = { .family = AF_UNIX };
            rc = unix_bind(u, &a);
        } else if (u->kind == K_INET_STREAM && !u->bound) {
            struct raddr a = { .family = AF_INET };
            rc = inet_bind(u, &a);
        }
        if (rc == 0) {
            if (u->kind == K_INET_STREAM && !u->tcpl) {
                /* connections from other hosts arrive through the stack; those from this host come straight here */
                u->tcpl = tcp_listen(u->lport, backlog);
            }
            spin_lock(&sk_lock);
            u->state = SS_LISTEN;
            u->backlog = backlog;
            spin_unlock(&sk_lock);
        }
    }
    sfile_put(f);
    return rc;
}

/* a connection made inside the host: build the server's end and queue it on the listener */
static long local_connect(struct sfile *f, struct usock *c, struct usock *l, int flags)
{
    UNUSED(flags);
    struct usock *s = kzalloc(sizeof(*s));
    if (!s) return -ERR_NOMEM;
    s->kind = c->kind;
    s->type = c->type;
    s->rcvbuf = s->sndbuf = 212992;
    s->pid = l->pid; s->uid = l->uid; s->gid = l->gid;
    s->loop = c->kind == K_INET_STREAM;
    s->accepted = true;
    if (c->type == SOCK_STREAM && (!ring_alloc(s) || (!c->ring && !ring_alloc(c)))) { kfree(s->ring); kfree(s); return -ERR_NOMEM; }
    uint64_t deadline = make_deadline(c->sndtimeo_us);
    for (;;) {
        uint64_t gen = sv_poll_gen();
        spin_lock(&sk_lock);
        bool alive = false;
        for (struct usock *x = all_socks; x; x = x->all_next) if (x == l) { alive = true; break; }
        if (!alive || l->state != SS_LISTEN) { spin_unlock(&sk_lock); kfree(s->ring); kfree(s); return -ERR_CONNREFUSED; }
        if (l->nacc < l->backlog) {
            s->uname = l->uname;
            s->bound = l->bound;
            s->lip = l->lip ? l->lip : (c->kind == K_INET_STREAM ? 0x7F000001 : 0);
            s->lport = l->lport;
            s->rip = c->lip ? c->lip : 0x7F000001;
            s->rport = c->lport;
            link_pair(c, s);
            s->all_next = all_socks;
            all_socks = s;
            if (c->kind == K_INET_STREAM) c->loop = true;
            c->rip = s->lip;
            c->rport = s->lport;
            c->connected_addr = true;
            if (l->acc_tail) l->acc_tail->acc_next = s; else l->acc_head = s;
            l->acc_tail = s;
            l->nacc++;
            spin_unlock(&sk_lock);
            sv_poll_wake();
            return 0;
        }
        spin_unlock(&sk_lock);
        if (nonblocking(f, 0)) { kfree(s->ring); kfree(s); return -ERR_AGAIN; }
        long w = sk_sleep(gen, deadline);
        if (w < 0) { kfree(s->ring); kfree(s); return w == -ERR_AGAIN ? -ERR_TIMEDOUT : w; }
    }
}

static struct usock *find_inet_listener(uint32_t ip, uint16_t port)
{
    for (struct usock *u = all_socks; u; u = u->all_next)
        if (u->kind == K_INET_STREAM && u->state == SS_LISTEN && !u->accepted && u->lport == port &&
            (u->lip == 0 || u->lip == ip || ((u->lip >> 24) == 127 && (ip >> 24) == 127)))
            return u;
    return NULL;
}

SC(s_connect)
{
    struct sfile *f = sv_fd_get(sv_curgrp(), (int)r->rdi);
    if (!f) return -ERR_BADF;
    struct usock *u = file_sock(f);
    struct raddr a;
    long rc;
    if (!u) { sfile_put(f); return -ERR_NOTSOCK; }
    rc = read_addr(r->rsi, (uint32_t)r->rdx, &a);
    if (rc < 0) { sfile_put(f); return rc; }

    if (a.family == AF_UNSPEC && u->kind == K_INET_DGRAM) {              /* disconnect */
        u->rip = 0; u->rport = 0; u->connected_addr = false;
        sfile_put(f);
        return 0;
    }
    if (u->kind == K_UNIX) {
        if (a.family != AF_UNIX || !a.ulen) { sfile_put(f); return -ERR_INVAL; }
        if (u->state == SS_CONNECTED && u->type != SOCK_DGRAM) { sfile_put(f); return -ERR_ISCONN; }
        if (u->state == SS_LISTEN) { sfile_put(f); return -ERR_INVAL; }
        if (a.path[0] != 0) {
            char path[VFS_PATH_MAX];
            if (!vfs_resolve(sv_curgrp()->cwd, a.path, path, sizeof(path))) { sfile_put(f); return -ERR_NAMETOOLONG; }
            if (!vfs_lookup(path)) { sfile_put(f); return -ERR_NOENT; }
            memset(a.path, 0, UNIX_PATH);
            size_t n = MIN(strlen(path), (size_t)UNIX_PATH - 1);
            memcpy(a.path, path, n);
            a.ulen = (uint8_t)(n + 1);
        }
        spin_lock(&sk_lock);
        struct usock *l = find_unix(&a, u->type);
        spin_unlock(&sk_lock);
        if (!l) { sfile_put(f); return -ERR_CONNREFUSED; }
        if (u->type == SOCK_DGRAM) {
            spin_lock(&sk_lock);
            u->peer = l;
            u->state = SS_CONNECTED;
            spin_unlock(&sk_lock);
            rc = 0;
        } else {
            if (l->state != SS_LISTEN) rc = -ERR_CONNREFUSED;
            else rc = local_connect(f, u, l, 0);
        }
        sfile_put(f);
        return rc;
    }

    if (a.family != AF_INET) { sfile_put(f); return -ERR_AFNOSUPPORT; }
    if (u->kind == K_INET_DGRAM) {
        if (!u->bound) {
            struct raddr any = { .family = AF_INET };
            rc = inet_bind(u, &any);
            if (rc < 0) { sfile_put(f); return rc; }
        }
        u->rip = a.ip ? a.ip : 0x7F000001;
        u->rport = a.port;
        u->connected_addr = true;
        sfile_put(f);
        return 0;
    }
    /* TCP */
    if (u->state == SS_CONNECTED) { sfile_put(f); return -ERR_ISCONN; }
    if (u->state == SS_LISTEN) { sfile_put(f); return -ERR_INVAL; }
    if (u->state == SS_CONNECTING) {
        uint32_t t = u->tcp ? tcp_flags(u->tcp) : 0;
        if (t & TCPF_WRITE) { u->state = SS_CONNECTED; sfile_put(f); return -ERR_ISCONN; }
        if (t & (TCPF_ERR | TCPF_CLOSED)) { int e = (t & TCPF_TIMEDOUT) ? ERR_TIMEDOUT : ERR_CONNREFUSED; sfile_put(f); return -e; }
        sfile_put(f);
        return -ERR_ALREADY;
    }
    uint32_t ip = a.ip ? a.ip : 0x7F000001;
    if (net_is_local_ip(ip)) {
        spin_lock(&sk_lock);
        struct usock *l = find_inet_listener(ip, a.port);
        spin_unlock(&sk_lock);
        if (!l) { sfile_put(f); return -ERR_CONNREFUSED; }
        if (!u->bound) {
            spin_lock(&sk_lock);
            u->lport = pick_port(u, K_INET_STREAM);
            u->lip = ip;
            u->bound = true;
            spin_unlock(&sk_lock);
        }
        rc = local_connect(f, u, l, 0);
        sfile_put(f);
        return rc;
    }
    if (!net_src_ip(ip)) { sfile_put(f); return -ERR_NETUNREACH; }
    u->tcp = tcp_connect_start(ip, a.port);
    if (!u->tcp) { sfile_put(f); return -ERR_HOSTUNREACH; }
    u->rip = ip;
    u->rport = a.port;
    u->connected_addr = true;
    u->state = SS_CONNECTING;
    if (nonblocking(f, 0)) { sfile_put(f); return -ERR_INPROGRESS; }
    for (;;) {
        uint64_t gen = sv_poll_gen();
        uint32_t t = tcp_flags(u->tcp);
        if (t & TCPF_WRITE) { u->state = SS_CONNECTED; rc = 0; break; }
        if (t & (TCPF_ERR | TCPF_CLOSED)) { rc = (t & TCPF_TIMEDOUT) ? -ERR_TIMEDOUT : -ERR_CONNREFUSED; u->state = SS_NEW; tcp_release(u->tcp); u->tcp = NULL; break; }
        long w = sk_sleep(gen, 0);
        if (w < 0) { rc = w; break; }                       /* the attempt goes on; the caller may poll for it */
    }
    sfile_put(f);
    return rc;
}

static long do_accept(int fd, uint64_t uaddr, uint64_t ulen, int flags)
{
    struct sfile *f = sv_fd_get(sv_curgrp(), fd);
    if (!f) return -ERR_BADF;
    struct usock *l = file_sock(f);
    if (!l) { sfile_put(f); return -ERR_NOTSOCK; }
    if (l->state != SS_LISTEN) { sfile_put(f); return -ERR_INVAL; }
    struct usock *s = NULL;
    uint64_t deadline = make_deadline(l->rcvtimeo_us);
    long rc = 0;
    for (;;) {
        uint64_t gen = sv_poll_gen();
        spin_lock(&sk_lock);
        if (l->acc_head) {
            s = l->acc_head;
            l->acc_head = s->acc_next;
            if (!l->acc_head) l->acc_tail = NULL;
            s->acc_next = NULL;
            l->nacc--;
            spin_unlock(&sk_lock);
            sv_poll_wake();
            break;
        }
        spin_unlock(&sk_lock);
        if (l->tcpl) {
            tcp_sock_t *t = tcp_accept_nb(l->tcpl);
            if (t) {
                s = kzalloc(sizeof(*s));
                if (!s) { tcp_release(t); rc = -ERR_NOMEM; break; }
                s->kind = K_INET_STREAM;
                s->type = SOCK_STREAM;
                s->tcp = t;
                s->state = SS_CONNECTED;
                s->rcvbuf = s->sndbuf = 212992;
                s->pid = l->pid; s->uid = l->uid; s->gid = l->gid;
                s->bound = true;
                s->accepted = true;
                tcp_addrs(t, &s->lip, &s->lport, &s->rip, &s->rport);
                s->connected_addr = true;
                spin_lock(&sk_lock);
                s->all_next = all_socks;
                all_socks = s;
                spin_unlock(&sk_lock);
                break;
            }
        }
        if (nonblocking(f, flags)) { rc = -ERR_AGAIN; break; }
        long w = sk_sleep(gen, deadline);
        if (w < 0) { rc = w; break; }
    }
    if (rc < 0) { sfile_put(f); return rc; }
    struct sfile *nf = NULL;
    int nfd = install_sock(s, flags, &nf);
    if (nfd < 0) { sfile_put(f); return nfd; }
    if (uaddr) {
        uint8_t sa[2 + UNIX_PATH];
        uint32_t salen;
        if (s->kind == K_UNIX) salen = unix_sockaddr(&s->uname, sa);
        else { ipport_to_sockaddr(s->rip, s->rport, sa); salen = 16; }
        if (s->kind == K_UNIX && s->peer) { struct raddr_u *pn = &s->peer->uname; salen = pn->len ? unix_sockaddr(pn, sa) : 2; }
        if (write_addr(uaddr, ulen, sa, salen) < 0) { sv_fd_close(sv_curgrp(), nfd); sfile_put(f); return -ERR_FAULT; }
    }
    sfile_put(f);
    return nfd;
}

SC(s_accept) { return do_accept((int)r->rdi, r->rsi, r->rdx, 0); }
SC(s_accept4) { return do_accept((int)r->rdi, r->rsi, r->rdx, (int)r->r10); }

/* ------------------------------------------------------------------------
 * getsockname, getpeername, shutdown
 * ---------------------------------------------------------------------- */

SC(s_getsockname)
{
    struct sfile *f = sv_fd_get(sv_curgrp(), (int)r->rdi);
    if (!f) return -ERR_BADF;
    struct usock *u = file_sock(f);
    long rc;
    uint8_t sa[2 + UNIX_PATH];
    uint32_t n;
    if (!u) rc = -ERR_NOTSOCK;
    else {
        if (u->kind == K_UNIX) n = u->bound ? unix_sockaddr(&u->uname, sa) : (memset(sa, 0, 4), sa[0] = AF_UNIX, 2);
        else {
            uint32_t ip = u->lip;
            uint16_t port = u->lport;
            if (u->tcp && !u->loop) tcp_addrs(u->tcp, &ip, &port, NULL, NULL);
            ipport_to_sockaddr(ip, port, sa);
            n = 16;
        }
        rc = write_addr(r->rsi, r->rdx, sa, n);
    }
    sfile_put(f);
    return rc;
}

SC(s_getpeername)
{
    struct sfile *f = sv_fd_get(sv_curgrp(), (int)r->rdi);
    if (!f) return -ERR_BADF;
    struct usock *u = file_sock(f);
    long rc;
    uint8_t sa[2 + UNIX_PATH];
    uint32_t n;
    if (!u) rc = -ERR_NOTSOCK;
    else if (u->kind == K_UNIX) {
        spin_lock(&sk_lock);
        struct usock *p = u->peer;
        bool ok = p || u->state == SS_CONNECTED;
        if (p && p->bound) n = unix_sockaddr(&p->uname, sa);
        else { memset(sa, 0, 4); sa[0] = AF_UNIX; n = 2; }
        spin_unlock(&sk_lock);
        rc = ok ? write_addr(r->rsi, r->rdx, sa, n) : -ERR_NOTCONN;
    } else if (!u->connected_addr || u->state == SS_CONNECTING) rc = -ERR_NOTCONN;
    else {
        ipport_to_sockaddr(u->rip, u->rport, sa);
        rc = write_addr(r->rsi, r->rdx, sa, 16);
    }
    sfile_put(f);
    return rc;
}

SC(s_shutdown)
{
    struct sfile *f = sv_fd_get(sv_curgrp(), (int)r->rdi);
    if (!f) return -ERR_BADF;
    struct usock *u = file_sock(f);
    int how = (int)r->rsi;
    long rc = 0;
    if (!u) rc = -ERR_NOTSOCK;
    else if (how < 0 || how > 2) rc = -ERR_INVAL;
    else if (u->state != SS_CONNECTED && !(u->kind == K_INET_DGRAM)) rc = -ERR_NOTCONN;
    else {
        spin_lock(&sk_lock);
        if (how == 0 || how == 2) u->rd_shut = true;
        if (how == 1 || how == 2) {
            u->wr_shut = true;
            if (u->peer) u->peer->peer_wr_shut = true;
        }
        spin_unlock(&sk_lock);
        if (u->tcp && !u->loop && (how == 1 || how == 2)) tcp_shutdown_write(u->tcp);
        sv_poll_wake();
    }
    sfile_put(f);
    return rc;
}

/* ------------------------------------------------------------------------
 * options
 * ---------------------------------------------------------------------- */

#define SO_DEBUG 1
#define SO_REUSEADDR 2
#define SO_TYPE 3
#define SO_ERROR 4
#define SO_DONTROUTE 5
#define SO_BROADCAST 6
#define SO_SNDBUF 7
#define SO_RCVBUF 8
#define SO_KEEPALIVE 9
#define SO_OOBINLINE 10
#define SO_LINGER 13
#define SO_REUSEPORT 15
#define SO_PASSCRED 16
#define SO_PEERCRED 17
#define SO_RCVLOWAT 18
#define SO_SNDLOWAT 19
#define SO_RCVTIMEO 20
#define SO_SNDTIMEO 21
#define SO_SNDBUFFORCE 32
#define SO_RCVBUFFORCE 33
#define SO_ACCEPTCONN 30
#define SO_PROTOCOL 38
#define SO_DOMAIN 39
#define SO_TIMESTAMP 29
#define SO_PEERSEC 31
#define SO_BUSY_POLL 46
#define SO_INCOMING_CPU 49

SC(s_setsockopt)
{
    struct sfile *f = sv_fd_get(sv_curgrp(), (int)r->rdi);
    if (!f) return -ERR_BADF;
    struct usock *u = file_sock(f);
    int level = (int)r->rsi, opt = (int)r->rdx;
    uint32_t len = (uint32_t)r->r8;
    long rc = 0;
    if (!u) { sfile_put(f); return -ERR_NOTSOCK; }
    uint8_t buf[32] = { 0 };
    if (len > sizeof(buf)) len = sizeof(buf);
    if (len && copy_from_user(buf, r->r10, len)) { sfile_put(f); return -ERR_FAULT; }
    int iv = len >= 4 ? *(int *)buf : 0;
    if (level == SOL_SOCKET) {
        switch (opt) {
        case SO_REUSEADDR: u->reuseaddr = iv != 0; break;
        case SO_REUSEPORT: u->reuseport = iv != 0; break;
        case SO_KEEPALIVE: u->keepalive = iv != 0; break;
        case SO_BROADCAST: u->broadcast = iv != 0; break;
        case SO_PASSCRED: u->passcred = iv != 0; break;
        case SO_SNDBUF: case SO_SNDBUFFORCE: u->sndbuf = MAX(iv, 4096) * 2; break;
        case SO_RCVBUF: case SO_RCVBUFFORCE: u->rcvbuf = MAX(iv, 2048) * 2; break;
        case SO_RCVTIMEO: case SO_SNDTIMEO: {
            if (len < 16) { rc = -ERR_INVAL; break; }
            int64_t sec = *(int64_t *)buf, usec = *(int64_t *)(buf + 8);
            uint64_t us = sec < 0 ? 0 : (uint64_t)sec * 1000000 + (uint64_t)usec;
            if (opt == SO_RCVTIMEO) u->rcvtimeo_us = us; else u->sndtimeo_us = us;
            break;
        }
        case SO_DEBUG: case SO_DONTROUTE: case SO_OOBINLINE: case SO_LINGER: case SO_RCVLOWAT: case SO_SNDLOWAT:
        case SO_TIMESTAMP: case SO_BUSY_POLL: case SO_INCOMING_CPU:
            break;
        default: rc = -ERR_NOPROTOOPT;
        }
    } else if (level == 6) {                                /* IPPROTO_TCP */
        if (u->kind != K_INET_STREAM) rc = -ERR_OPNOTSUPP;
        else if (opt == 1) u->nodelay = iv != 0;
        else if (opt == 4 || opt == 5 || opt == 6 || opt == 7 || opt == 9 || opt == 12 || opt == 13 || opt == 18 ||
                 opt == 2 || opt == 3 || opt == 8 || opt == 23 || opt == 24)
            rc = 0;                                         /* keepalive tuning, MSS, cork, quick ack, fast open ... accepted */
        else rc = -ERR_NOPROTOOPT;
    } else if (level == 0) {                                /* IPPROTO_IP */
        if (u->kind == K_UNIX) rc = -ERR_OPNOTSUPP;
        /* TOS, TTL, packet info, multicast membership: accepted; nothing here routes multicast */
    } else if (level == 41) {                               /* IPPROTO_IPV6 */
        rc = -ERR_NOPROTOOPT;
    } else {
        rc = -ERR_NOPROTOOPT;
    }
    sfile_put(f);
    return rc;
}

SC(s_getsockopt)
{
    struct sfile *f = sv_fd_get(sv_curgrp(), (int)r->rdi);
    if (!f) return -ERR_BADF;
    struct usock *u = file_sock(f);
    int level = (int)r->rsi, opt = (int)r->rdx;
    if (!u) { sfile_put(f); return -ERR_NOTSOCK; }
    uint32_t have;
    if (copy_from_user(&have, r->r8, 4)) { sfile_put(f); return -ERR_FAULT; }
    uint8_t out[32] = { 0 };
    uint32_t n = 4;
    long rc = 0;
    int *iv = (int *)out;
    if (level == SOL_SOCKET) {
        switch (opt) {
        case SO_TYPE: *iv = u->type; break;
        case SO_ERROR:
            if (u->kind == K_INET_STREAM && !u->loop && u->state == SS_CONNECTING && u->tcp) {
                uint32_t t = tcp_flags(u->tcp);
                if (t & TCPF_WRITE) u->state = SS_CONNECTED;
                else if (t & (TCPF_ERR | TCPF_CLOSED)) u->err = (t & TCPF_TIMEDOUT) ? ERR_TIMEDOUT : ERR_CONNREFUSED;
            }
            *iv = u->err;
            u->err = 0;
            break;
        case SO_REUSEADDR: *iv = u->reuseaddr; break;
        case SO_REUSEPORT: *iv = u->reuseport; break;
        case SO_KEEPALIVE: *iv = u->keepalive; break;
        case SO_BROADCAST: *iv = u->broadcast; break;
        case SO_PASSCRED: *iv = u->passcred; break;
        case SO_SNDBUF: *iv = u->sndbuf; break;
        case SO_RCVBUF: *iv = u->rcvbuf; break;
        case SO_ACCEPTCONN: *iv = u->state == SS_LISTEN; break;
        case SO_DOMAIN: *iv = u->kind == K_UNIX ? AF_UNIX : AF_INET; break;
        case SO_PROTOCOL: *iv = u->kind == K_INET_STREAM ? 6 : u->kind == K_INET_DGRAM ? 17 : 0; break;
        case SO_RCVLOWAT: *iv = 1; break;
        case SO_LINGER: n = 8; break;                       /* off */
        case SO_DEBUG: case SO_DONTROUTE: case SO_OOBINLINE: case SO_TIMESTAMP: break;
        case SO_RCVTIMEO: case SO_SNDTIMEO: {
            uint64_t us = opt == SO_RCVTIMEO ? u->rcvtimeo_us : u->sndtimeo_us;
            *(int64_t *)out = (int64_t)(us / 1000000);
            *(int64_t *)(out + 8) = (int64_t)(us % 1000000);
            n = 16;
            break;
        }
        case SO_PEERCRED: {
            if (u->kind != K_UNIX) { rc = -ERR_NOPROTOOPT; break; }
            int pid = u->peer_pid;
            uint32_t uid = u->peer_uid, gid = u->peer_gid;
            if (!u->peer && !u->peer_pid) { pid = 0; uid = gid = 65534; }
            *(int *)out = pid;
            *(uint32_t *)(out + 4) = uid;
            *(uint32_t *)(out + 8) = gid;
            n = 12;
            break;
        }
        default: rc = -ERR_NOPROTOOPT;
        }
    } else if (level == 6 && u->kind == K_INET_STREAM) {
        if (opt == 1) *iv = u->nodelay;
        else if (opt == 2) *iv = 1460;
        else rc = -ERR_NOPROTOOPT;
    } else if (level == 0 && u->kind != K_UNIX) {
        if (opt == 2 || opt == 1) *iv = 0; else if (opt == 3) *iv = 64; else rc = -ERR_NOPROTOOPT;
    } else {
        rc = -ERR_NOPROTOOPT;
    }
    if (rc == 0) {
        uint32_t c = MIN(have, n);
        if ((c && copy_to_user(r->r10, out, c)) || copy_to_user(r->r8, &c, 4)) rc = -ERR_FAULT;
    }
    sfile_put(f);
    return rc;
}

/* ------------------------------------------------------------------------
 * sendmsg, recvmsg and the datagram forms
 * ---------------------------------------------------------------------- */

struct iovec { uint64_t base, len; };

struct msghdr {
    uint64_t name;
    uint32_t namelen, pad0;
    uint64_t iov, iovlen;
    uint64_t control, controllen;
    int32_t flags, pad1;
};

struct cmsghdr { uint64_t len; int32_t level, type; };

#define CMSG_ALIGN(n) (((n) + 7) & ~(size_t)7)

static long iov_load(uint64_t uiov, uint64_t cnt, struct iovec **out, size_t *total)
{
    if (cnt > 1024) return -ERR_INVAL;
    struct iovec *iov = kmalloc(cnt ? cnt * sizeof(*iov) : 1);
    if (!iov) return -ERR_NOMEM;
    if (cnt && copy_from_user(iov, uiov, cnt * sizeof(*iov))) { kfree(iov); return -ERR_FAULT; }
    size_t t = 0;
    for (uint64_t i = 0; i < cnt; i++) {
        if (iov[i].len > 0x7FFFFFFF || t + iov[i].len > 0x7FFFFFFF) { kfree(iov); return -ERR_INVAL; }
        t += iov[i].len;
    }
    *out = iov;
    *total = t;
    return 0;
}

static long iov_gather(const struct iovec *iov, uint64_t cnt, size_t skip, uint8_t *dst, size_t n)
{
    size_t done = 0;
    for (uint64_t i = 0; i < cnt && done < n; i++) {
        if (skip >= iov[i].len) { skip -= iov[i].len; continue; }
        size_t c = MIN((size_t)iov[i].len - skip, n - done);
        if (copy_from_user(dst + done, iov[i].base + skip, c)) return -ERR_FAULT;
        done += c;
        skip = 0;
    }
    return (long)done;
}

static long iov_scatter(const struct iovec *iov, uint64_t cnt, const uint8_t *src, size_t n)
{
    size_t done = 0;
    for (uint64_t i = 0; i < cnt && done < n; i++) {
        size_t c = MIN((size_t)iov[i].len, n - done);
        if (c && copy_to_user(iov[i].base, src + done, c)) return -ERR_FAULT;
        done += c;
    }
    return (long)done;
}

/* descriptors a sender wants to pass */
static long parse_control(uint64_t ctl, uint64_t ctllen, struct scm **out)
{
    *out = NULL;
    if (!ctl || ctllen < sizeof(struct cmsghdr)) return 0;
    if (ctllen > 4096) return -ERR_INVAL;
    uint8_t *k = kmalloc(ctllen);
    if (!k) return -ERR_NOMEM;
    if (copy_from_user(k, ctl, ctllen)) { kfree(k); return -ERR_FAULT; }
    struct scm *s = NULL;
    long rc = 0;
    for (size_t off = 0; off + sizeof(struct cmsghdr) <= ctllen;) {
        struct cmsghdr *h = (struct cmsghdr *)(k + off);
        if (h->len < sizeof(*h) || off + h->len > ctllen) { rc = -ERR_INVAL; break; }
        if (h->level == SOL_SOCKET && h->type == SCM_RIGHTS) {
            int cnt = (int)((h->len - sizeof(*h)) / 4);
            const int *fds = (const int *)(k + off + sizeof(*h));
            if (cnt > SCM_MAX_FDS) { rc = -ERR_INVAL; break; }
            if (!s) { s = kzalloc(sizeof(*s)); if (!s) { rc = -ERR_NOMEM; break; } s->f = kzalloc(SCM_MAX_FDS * sizeof(*s->f)); if (!s->f) { rc = -ERR_NOMEM; break; } }
            for (int i = 0; i < cnt && s->nfds < SCM_MAX_FDS; i++) {
                struct sfile *sf = sv_fd_get(sv_curgrp(), fds[i]);
                if (!sf) { rc = -ERR_BADF; break; }
                s->f[s->nfds++] = sf;
            }
            if (rc) break;
        }
        off += CMSG_ALIGN(h->len);
    }
    kfree(k);
    if (rc) { scm_free(s); return rc; }
    *out = s;
    return 0;
}

static long do_sendmsg(int fd, const struct msghdr *mh, int flags, struct iovec *iov, size_t total)
{
    struct sfile *f = sv_fd_get(sv_curgrp(), fd);
    if (!f) return -ERR_BADF;
    struct usock *u = file_sock(f);
    if (!u) { sfile_put(f); return -ERR_NOTSOCK; }
    struct raddr to;
    struct raddr *top = NULL;
    long rc = 0;
    if (mh->name && mh->namelen) {
        rc = read_addr(mh->name, mh->namelen, &to);
        if (rc < 0) { sfile_put(f); return rc; }
        if (to.family == AF_UNIX && to.ulen && to.path[0] != 0) {
            char path[VFS_PATH_MAX];
            if (!vfs_resolve(sv_curgrp()->cwd, to.path, path, sizeof(path))) { sfile_put(f); return -ERR_NAMETOOLONG; }
            memset(to.path, 0, UNIX_PATH);
            size_t n = MIN(strlen(path), (size_t)UNIX_PATH - 1);
            memcpy(to.path, path, n);
            to.ulen = (uint8_t)(n + 1);
        }
        top = &to;
    }
    struct scm *scm = NULL;
    rc = parse_control(mh->control, mh->controllen, &scm);
    if (rc < 0) { sfile_put(f); return rc; }
    bool whole = is_msg(u) || u->kind == K_INET_DGRAM;
    if (whole && total > MSGQ_BYTES) { scm_free(scm); sfile_put(f); return -ERR_MSGSIZE; }
    size_t chunk = whole ? total : MIN(total, (size_t)CHUNK);
    uint8_t *kb = kmalloc(chunk ? chunk : 1);
    if (!kb) { scm_free(scm); sfile_put(f); return -ERR_NOMEM; }
    size_t sent = 0;
    do {
        size_t c = MIN(total - sent, chunk);
        long g = iov_gather(iov, mh->iovlen, sent, kb, c);
        if (g < 0) { rc = g; break; }
        long k = do_send(f, u, kb, c, flags, top, &scm);
        if (k < 0) { rc = sent ? (long)sent : k; goto out; }
        sent += (size_t)k;
        if ((size_t)k < c) break;
    } while (sent < total);
    rc = (long)sent;
out:
    kfree(kb);
    scm_free(scm);                                           /* still ours if nothing carried it away */
    sfile_put(f);
    return rc;
}

static long do_recvmsg(int fd, struct msghdr *mh, int flags, struct iovec *iov, size_t total)
{
    struct sfile *f = sv_fd_get(sv_curgrp(), fd);
    if (!f) return -ERR_BADF;
    struct usock *u = file_sock(f);
    if (!u) { sfile_put(f); return -ERR_NOTSOCK; }
    bool whole = is_msg(u) || u->kind == K_INET_DGRAM;
    size_t cap = whole ? MIN(total, (size_t)MSGQ_BYTES) : MIN(total, (size_t)CHUNK);
    uint8_t *kb = kmalloc(cap ? cap : 1);
    if (!kb) { sfile_put(f); return -ERR_NOMEM; }
    struct raddr from;
    memset(&from, 0, sizeof(from));
    struct scm *scm = NULL;
    bool trunc = false;
    long n = do_recv(f, u, kb, cap, flags, &from, &scm, &trunc);
    long rc = n;
    int out_flags = 0;
    if (n >= 0) {
        size_t got = MIN((size_t)n, cap);
        long s = iov_scatter(iov, mh->iovlen, kb, got);
        if (s < 0) rc = s;
        else {
            if (trunc) out_flags |= MSG_TRUNC;
            if (flags & MSG_TRUNC && whole) rc = n; else rc = s;
        }
        /* the sender's address */
        if (rc >= 0 && mh->name) {
            uint8_t sa[2 + UNIX_PATH];
            uint32_t salen = 0;
            if (from.family == AF_INET) { ipport_to_sockaddr(from.ip, from.port, sa); salen = 16; }
            else if (from.family == AF_UNIX) {
                struct raddr_u nm = { .len = from.ulen };
                memcpy(nm.path, from.path, UNIX_PATH);
                salen = nm.len ? unix_sockaddr(&nm, sa) : 2;
                if (!nm.len) { memset(sa, 0, 4); sa[0] = AF_UNIX; }
            } else if (u->kind == K_INET_STREAM) { ipport_to_sockaddr(u->rip, u->rport, sa); salen = 16; }
            uint32_t give = MIN(mh->namelen, salen);
            if (give && copy_to_user(mh->name, sa, give)) rc = -ERR_FAULT;
            mh->namelen = salen;
        } else if (rc >= 0) {
            mh->namelen = 0;
        }
    }
    /* ancillary data: passed descriptors and, when asked, the sender's credentials */
    size_t used = 0;
    if (rc >= 0 && mh->control) {
        uint8_t *cb = kzalloc(mh->controllen ? MIN(mh->controllen, (uint64_t)4096) : 1);
        size_t cl = MIN(mh->controllen, (uint64_t)4096);
        if (cb) {
            if (scm && scm->nfds) {
                size_t fit = cl > sizeof(struct cmsghdr) ? (cl - sizeof(struct cmsghdr)) / 4 : 0;
                int give = (int)MIN((size_t)scm->nfds, fit);
                if (give < scm->nfds) out_flags |= MSG_CTRUNC;
                if (give > 0) {
                    struct cmsghdr *h = (struct cmsghdr *)cb;
                    h->len = sizeof(*h) + (uint64_t)give * 4;
                    h->level = SOL_SOCKET;
                    h->type = SCM_RIGHTS;
                    for (int i = 0; i < give; i++) {
                        int nfd = sv_fd_alloc(sv_curgrp(), sfile_get(scm->f[i]), 0, (flags & MSG_CMSG_CLOEXEC) != 0);
                        if (nfd < 0) { sfile_put(scm->f[i]); nfd = -1; }
                        ((int *)(cb + sizeof(*h)))[i] = nfd;
                    }
                    used = CMSG_ALIGN(h->len);
                }
            }
            if (u->passcred && u->kind == K_UNIX && used + CMSG_ALIGN(sizeof(struct cmsghdr) + 12) <= cl) {
                struct cmsghdr *h = (struct cmsghdr *)(cb + used);
                h->len = sizeof(*h) + 12;
                h->level = SOL_SOCKET;
                h->type = SCM_CREDENTIALS;
                *(int *)(cb + used + sizeof(*h)) = u->peer_pid;
                *(uint32_t *)(cb + used + sizeof(*h) + 4) = u->peer_uid;
                *(uint32_t *)(cb + used + sizeof(*h) + 8) = u->peer_gid;
                used += CMSG_ALIGN(h->len);
            }
            if (used && copy_to_user(mh->control, cb, used)) rc = -ERR_FAULT;
            kfree(cb);
        }
    } else if (scm && scm->nfds) {
        out_flags |= MSG_CTRUNC;
    }
    mh->controllen = used;
    mh->flags = out_flags;
    scm_free(scm);
    kfree(kb);
    sfile_put(f);
    return rc;
}

static long msg_syscall(int fd, uint64_t umh, int flags, bool recv)
{
    struct msghdr mh;
    if (copy_from_user(&mh, umh, sizeof(mh))) return -ERR_FAULT;
    struct iovec *iov;
    size_t total;
    long rc = iov_load(mh.iov, mh.iovlen, &iov, &total);
    if (rc < 0) return rc;
    rc = recv ? do_recvmsg(fd, &mh, flags, iov, total) : do_sendmsg(fd, &mh, flags, iov, total);
    kfree(iov);
    if (recv && copy_to_user(umh, &mh, sizeof(mh)) && rc >= 0) rc = -ERR_FAULT;
    return rc;
}

SC(s_sendmsg) { return msg_syscall((int)r->rdi, r->rsi, (int)r->rdx, false); }
SC(s_recvmsg) { return msg_syscall((int)r->rdi, r->rsi, (int)r->rdx, true); }

SC(s_sendto)
{
    struct iovec iov = { r->rsi, r->rdx };
    struct msghdr mh = { .name = r->r8, .namelen = (uint32_t)r->r9, .iov = 0, .iovlen = 1 };
    if (r->rdx > 0x7FFFFFFF) return -ERR_INVAL;
    return do_sendmsg((int)r->rdi, &mh, (int)r->r10, &iov, (size_t)r->rdx);
}

SC(s_recvfrom)
{
    struct iovec iov = { r->rsi, r->rdx };
    struct msghdr mh = { .name = r->r8, .namelen = 0, .iovlen = 1 };
    if (r->rdx > 0x7FFFFFFF) return -ERR_INVAL;
    if (r->r8) {
        uint32_t have = 0;
        if (r->r9 && copy_from_user(&have, r->r9, 4)) return -ERR_FAULT;
        mh.namelen = have;
    }
    long rc = do_recvmsg((int)r->rdi, &mh, (int)r->r10, &iov, (size_t)r->rdx);
    if (rc >= 0 && r->r8 && r->r9 && copy_to_user(r->r9, &mh.namelen, 4)) return -ERR_FAULT;
    return rc;
}

struct mmsghdr { struct msghdr hdr; uint32_t len, pad; };

static long mmsg(struct regs *r, bool recv)
{
    uint64_t base = r->rsi;
    unsigned vlen = (unsigned)r->rdx;
    if (vlen > 1024) vlen = 1024;
    unsigned done = 0;
    for (; done < vlen; done++) {
        uint64_t one = base + (uint64_t)done * sizeof(struct mmsghdr);
        long n = msg_syscall((int)r->rdi, one, (int)r->r10, recv);
        if (n < 0) return done ? (long)done : n;
        uint32_t len = (uint32_t)n;
        if (copy_to_user(one + sizeof(struct msghdr), &len, 4)) return -ERR_FAULT;
        if (recv) r->r10 |= MSG_DONTWAIT;                        /* after the first, take only what is already there */
    }
    return (long)done;
}

SC(s_sendmmsg) { return mmsg(r, false); }
SC(s_recvmmsg) { return mmsg(r, true); }

/* ------------------------------------------------------------------------
 * interface queries (SIOCGIF*), from the real adapters
 * ---------------------------------------------------------------------- */

static bool iface_by_name(const char *name, struct net_info *ni, int *index)
{
    if (!strcmp(name, "lo")) {
        memset(ni, 0, sizeof(*ni));
        strlcpy(ni->ifname, "lo", sizeof(ni->ifname));
        ni->link = true;
        ni->ip = 0x7F000001;
        ni->netmask = 0xFF000000;
        *index = 1;
        return true;
    }
    for (int i = 0; i < net_iface_count(); i++) {
        if (net_iface_get(i, ni) && !strcmp(ni->ifname, name)) { *index = i + 2; return true; }
    }
    return false;
}

static void fill_in(uint8_t *sa, uint32_t ip)
{
    memset(sa, 0, 16);
    sa[0] = AF_INET;
    sa[4] = (uint8_t)(ip >> 24); sa[5] = (uint8_t)(ip >> 16); sa[6] = (uint8_t)(ip >> 8); sa[7] = (uint8_t)ip;
}

static long iface_ioctl(unsigned long cmd, uint64_t arg)
{
    uint8_t ifr[40];
    if (cmd == 0x8912) {                                    /* SIOCGIFCONF */
        struct { int32_t len, pad; uint64_t buf; } ic;
        if (copy_from_user(&ic, arg, sizeof(ic))) return -ERR_FAULT;
        int count = 1 + net_iface_count();
        int fit = ic.len / 40, put = 0;
        for (int i = 0; i < count && put < fit; i++) {
            struct net_info ni;
            int idx;
            const char *nm = i == 0 ? "lo" : NULL;
            if (i > 0) { if (!net_iface_get(i - 1, &ni)) continue; nm = ni.ifname; }
            if (!iface_by_name(nm, &ni, &idx)) continue;
            if (!ni.ip) continue;
            memset(ifr, 0, sizeof(ifr));
            strlcpy((char *)ifr, nm, 16);
            fill_in(ifr + 16, ni.ip);
            if (copy_to_user(ic.buf + (uint64_t)put * 40, ifr, 40)) return -ERR_FAULT;
            put++;
        }
        ic.len = put * 40;
        return copy_to_user(arg, &ic, sizeof(ic)) ? -ERR_FAULT : 0;
    }
    if (copy_from_user(ifr, arg, sizeof(ifr))) return -ERR_FAULT;
    if (cmd == 0x8910) {                                    /* SIOCGIFNAME: index -> name */
        int want = *(int *)(ifr + 16);
        struct net_info ni;
        memset(ifr, 0, 16);
        if (want == 1) strlcpy((char *)ifr, "lo", 16);
        else if (want >= 2 && net_iface_get(want - 2, &ni)) strlcpy((char *)ifr, ni.ifname, 16);
        else return -ERR_NODEV;
        return copy_to_user(arg, ifr, sizeof(ifr)) ? -ERR_FAULT : 0;
    }
    struct net_info ni;
    int idx;
    char name[16];
    memcpy(name, ifr, 16);
    name[15] = 0;
    if (!iface_by_name(name, &ni, &idx)) return -ERR_NODEV;
    memset(ifr + 16, 0, 24);
    switch (cmd) {
    case 0x8915: fill_in(ifr + 16, ni.ip); break;                                   /* SIOCGIFADDR */
    case 0x891b: fill_in(ifr + 16, ni.netmask); break;                              /* SIOCGIFNETMASK */
    case 0x8919: fill_in(ifr + 16, ni.ip | ~ni.netmask); break;                     /* SIOCGIFBRDADDR */
    case 0x8913: {                                                                  /* SIOCGIFFLAGS */
        uint16_t fl = 0x1 | 0x80;                                                   /* UP, RUNNING style bits below */
        if (ni.link) fl |= 0x40;
        if (!strcmp(name, "lo")) fl |= 0x8; else fl |= 0x2 | 0x1000;                /* LOOPBACK / BROADCAST, MULTICAST */
        *(uint16_t *)(ifr + 16) = fl;
        break;
    }
    case 0x8921: *(int *)(ifr + 16) = !strcmp(name, "lo") ? 65536 : 1500; break;    /* SIOCGIFMTU */
    case 0x8933: *(int *)(ifr + 16) = idx; break;                                   /* SIOCGIFINDEX */
    case 0x8927:                                                                    /* SIOCGIFHWADDR */
        *(uint16_t *)(ifr + 16) = !strcmp(name, "lo") ? 772 : 1;                    /* ARPHRD_LOOPBACK, ARPHRD_ETHER */
        memcpy(ifr + 18, ni.mac, 6);
        break;
    default: return -ERR_NOTTY;
    }
    return copy_to_user(arg, ifr, sizeof(ifr)) ? -ERR_FAULT : 0;
}
