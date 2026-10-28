/*
 * HTTP/1.1 client for http:// and https:// URLs: one request per
 * connection, chunked transfer coding, gzip content coding, redirects are
 * reported to the caller.
 */
#include <net.h>
#include <tls.h>
#include <mm.h>
#include <dev.h>
#include <archive.h>
#include <vfs.h>

#define BODY_MAX (16u << 20)

struct conn {
    tcp_sock_t *sock;
    struct tls *tls;
};

static long conn_send(struct conn *c, const void *d, size_t n)
{
    return c->tls ? tls_send(c->tls, d, n) : tcp_send(c->sock, d, n);
}

static long conn_recv(struct conn *c, void *d, size_t n, int timeout)
{
    return c->tls ? tls_recv(c->tls, d, n, timeout) : tcp_recv(c->sock, d, n, timeout);
}

static bool parse_url(const char *url, bool *https, char *host, size_t hn, uint16_t *port, char *path, size_t pn)
{
    *https = false;
    if (!strncasecmp(url, "https://", 8)) { *https = true; url += 8; }
    else if (!strncasecmp(url, "http://", 7)) url += 7;
    const char *slash = strchr(url, '/');
    size_t hl = slash ? (size_t)(slash - url) : strlen(url);
    if (!hl || hl >= hn) return false;
    char hp[256];
    strlcpy(hp, url, MIN(hl + 1, sizeof(hp)));
    char *colon = strchr(hp, ':');
    *port = *https ? 443 : 80;
    if (colon) {
        *colon = 0;
        *port = (uint16_t)atoi(colon + 1);
    }
    strlcpy(host, hp, hn);
    strlcpy(path, slash ? slash : "/", pn);
    char *frag = strchr(path, '#');
    if (frag) *frag = 0;
    return true;
}

static bool header(const char *hdr, size_t hlen, const char *name, char *out, size_t n)
{
    size_t nl = strlen(name);
    for (size_t i = 0; i + nl + 1 < hlen; i++) {
        if ((i == 0 || hdr[i - 1] == '\n') && !strncasecmp(hdr + i, name, nl) && hdr[i + nl] == ':') {
            size_t j = i + nl + 1, k = 0;
            while (j < hlen && (hdr[j] == ' ' || hdr[j] == '\t')) j++;
            while (j < hlen && hdr[j] != '\r' && hdr[j] != '\n' && k + 1 < n) out[k++] = hdr[j++];
            out[k] = 0;
            return true;
        }
    }
    return false;
}

/* decodes chunked data in place; returns false while the terminating chunk is missing */
static bool unchunk(uint8_t *b, size_t len, size_t *out_len)
{
    size_t in = 0, out = 0;
    for (;;) {
        size_t size = 0;
        bool any = false;
        while (in < len) {
            char c = (char)b[in];
            int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (v < 0) break;
            size = size * 16 + (size_t)v;
            any = true;
            in++;
        }
        while (in < len && b[in] != '\n') in++;          /* chunk extensions */
        if (in >= len || !any) return false;
        in++;
        if (!size) { *out_len = out; return true; }
        if (in + size + 2 > len) return false;
        memmove(b + out, b + in, size);
        out += size;
        in += size + 2;
    }
}

static uint8_t *gunzip(const uint8_t *d, size_t n, size_t *out_len)
{
    return gunzip_alloc(d, n, out_len, BODY_MAX);
}

int net_http_request(const char *url, struct http_result *r, int timeout_ms)
{
    memset(r, 0, sizeof(*r));
    r->status = -1;
    bool https;
    char host[128], path[1024];
    uint16_t port;
    if (!parse_url(url, &https, host, sizeof(host), &port, path, sizeof(path))) {
        strlcpy(r->error, "that is not a valid address", sizeof(r->error));
        return -1;
    }
    struct net_info ni;
    if (!net_get_info(&ni) || !ni.ip) {
        strlcpy(r->error, "no network connection", sizeof(r->error));
        return -1;
    }
    uint32_t ip;
    if (!str_to_ip(host, &ip) && !net_resolve(host, &ip, 4000)) {
        snprintf(r->error, sizeof(r->error), "could not find the server '%s'", host);
        return -1;
    }
    uint64_t t0 = uptime_ms();
    struct conn c = { 0 };
    c.sock = tcp_connect(ip, port, MIN(timeout_ms, 8000));
    if (!c.sock) {
        snprintf(r->error, sizeof(r->error), "%s did not accept a connection on port %u", host, port);
        return -1;
    }
    if (https) {
        c.tls = tls_connect(c.sock, host, MIN(timeout_ms, 10000), r->error, sizeof(r->error));
        if (!c.tls) {
            tcp_close(c.sock);
            return -1;
        }
        strlcpy(r->tls, tls_description(c.tls), sizeof(r->tls));
    }
    char *req = kmalloc(2048);
    char hostport[140];
    if ((https && port == 443) || (!https && port == 80)) strlcpy(hostport, host, sizeof(hostport));
    else snprintf(hostport, sizeof(hostport), "%s:%u", host, port);
    int n = snprintf(req, 2048,
                     "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Mozilla/5.0 (ZenithOS " ZENITH_VERSION
                     ") ZenithWeb/1.0\r\nAccept: text/html,application/xhtml+xml,text/plain,image/png,image/jpeg,*/*;q=0.8\r\n"
                     "Accept-Encoding: gzip\r\nAccept-Language: de,en;q=0.8\r\nConnection: close\r\n\r\n",
                     path, hostport);
    long sent = conn_send(&c, req, (size_t)n);
    kfree(req);
    if (sent != n) {
        strlcpy(r->error, "sending the request failed", sizeof(r->error));
        goto done;
    }

    size_t cap = 65536, len = 0, hdr_end = 0, want = 0;
    bool chunked = false, have_len = false;
    uint8_t *buf = kmalloc(cap);
    uint64_t deadline = uptime_ms() + (uint64_t)timeout_ms;
    for (;;) {
        if (len + 16384 + 1 > cap) {
            if (cap >= BODY_MAX) break;
            cap *= 2;
            uint8_t *nb = krealloc(buf, cap);
            if (!nb) break;
            buf = nb;
        }
        long got = conn_recv(&c, buf + len, cap - len - 1, 2000);
        if (got == -2) {
            if (uptime_ms() > deadline) { strlcpy(r->error, "the server took too long", sizeof(r->error)); break; }
            continue;
        }
        if (got <= 0) break;
        len += (size_t)got;
        if (!hdr_end) {
            for (size_t i = 0; i + 3 < len; i++)
                if (!memcmp(buf + i, "\r\n\r\n", 4)) { hdr_end = i + 4; break; }
            if (hdr_end) {
                char v[64];
                if (header((char *)buf, hdr_end, "Content-Length", v, sizeof(v))) { want = (size_t)atoi(v); have_len = true; }
                if (header((char *)buf, hdr_end, "Transfer-Encoding", v, sizeof(v)) && strstr_ci(v, "chunked")) chunked = true;
            }
        }
        if (hdr_end && have_len && !chunked && len >= hdr_end + want) break;
        if (hdr_end && chunked && len >= hdr_end + 5 && !memcmp(buf + len - 5, "0\r\n\r\n", 5)) break;
    }
    buf[len] = 0;
    if (len > 12 && !memcmp(buf, "HTTP/", 5)) {
        r->status = atoi((char *)buf + 9);
        if (!hdr_end) hdr_end = len;
        header((char *)buf, hdr_end, "Location", r->location, sizeof(r->location));
        header((char *)buf, hdr_end, "Content-Type", r->content_type, sizeof(r->content_type));
        size_t bl = len - hdr_end;
        uint8_t *body = buf + hdr_end;
        if (chunked) {
            size_t out;
            if (unchunk(body, bl, &out)) bl = out;
        }
        if (have_len && !chunked) bl = MIN(bl, want);
        char enc[32];
        uint8_t *final = NULL;
        size_t fl = 0;
        if (header((char *)buf, hdr_end, "Content-Encoding", enc, sizeof(enc)) && strstr_ci(enc, "gzip"))
            final = gunzip(body, bl, &fl);
        if (!final) {
            final = kmalloc(bl + 1);
            memcpy(final, body, bl);
            final[bl] = 0;
            fl = bl;
        }
        r->body = (char *)final;
        r->len = fl;
    } else if (!r->error[0]) {
        strlcpy(r->error, len ? "the server's answer was not HTTP" : "the server sent nothing back", sizeof(r->error));
    }
    kfree(buf);
    /* the log keeps the path but not the query string (search terms, tokens) */
    const char *q = strchr(path, '?');
    klog("http: GET %s%s:%u%.*s -> %d, %zu bytes in %lu ms", https ? "https://" : "http://", host, port,
         q ? (int)(q - path) : (int)strlen(path), path, r->status, r->len, uptime_ms() - t0);
done:
    if (c.tls) tls_close(c.tls);
    tcp_close(c.sock);
    return r->status;
}

int net_http_get_ex(const char *url, char **body, size_t *blen, char *location, size_t locn, int timeout_ms)
{
    struct http_result *r = kmalloc(sizeof(*r));
    int st = net_http_request(url, r, timeout_ms);
    if (location && locn) strlcpy(location, r->location, locn);
    *body = r->body;
    *blen = r->len;
    kfree(r);
    return st;
}

int net_http_get(const char *url, char **body, size_t *blen, int timeout_ms)
{
    return net_http_get_ex(url, body, blen, NULL, 0, timeout_ms);
}

/* ------------------------------------------------------------------------
 * downloads to a file: the body is written as it arrives, so the size is
 * limited by the disk, not by memory
 * ---------------------------------------------------------------------- */

struct dl {
    vnode_t *vn;
    uint64_t off, total, max;
    bool chunked, have_len, done;
    /* chunked decoding */
    int cstate;                 /* 0 size line, 1 data, 2 CR LF after data, 3 trailer */
    uint64_t csize;
    int csize_digits;
    bool csize_ext;
    net_progress_fn cb;
    void *ctx;
    bool failed;
};

static void dl_write(struct dl *d, const uint8_t *p, size_t n)
{
    if (!n || d->failed) return;
    if (d->off + n > d->max) { d->failed = true; return; }
    if (vfs_write(d->vn, d->off, p, n) != (long)n) { d->failed = true; return; }
    d->off += n;
    if (d->cb) d->cb(d->ctx, d->off, d->have_len ? d->total : 0);
}

static void dl_feed(struct dl *d, const uint8_t *p, size_t n)
{
    if (!d->chunked) {
        if (d->have_len && d->off + n > d->total) n = (size_t)(d->total - d->off);
        dl_write(d, p, n);
        if (d->have_len && d->off >= d->total) d->done = true;
        return;
    }
    while (n && !d->done && !d->failed) {
        switch (d->cstate) {
        case 0: {                                           /* hex size, extensions, LF */
            uint8_t c = *p++;
            n--;
            int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (c == '\n') {
                if (!d->csize_digits) break;                /* blank line before the size */
                d->cstate = d->csize ? 1 : 3;
                d->csize_ext = false;
                if (!d->csize) d->done = true;
            } else if (v >= 0 && !d->csize_ext) {
                d->csize = d->csize * 16 + (uint64_t)v;
                d->csize_digits++;
            } else if (c != '\r') {
                d->csize_ext = true;
            }
            break;
        }
        case 1: {
            size_t take = (size_t)MIN((uint64_t)n, d->csize);
            dl_write(d, p, take);
            p += take;
            n -= take;
            d->csize -= take;
            if (!d->csize) { d->cstate = 2; }
            break;
        }
        case 2:                                             /* the CR LF after the data */
            if (*p == '\n') { d->cstate = 0; d->csize = 0; d->csize_digits = 0; }
            p++;
            n--;
            break;
        default:
            n = 0;
        }
    }
}

int net_http_download(const char *start_url, const char *dest, uint64_t max_bytes, net_progress_fn cb, void *ctx,
                      char *err, size_t errn)
{
#define FAIL(...) do { if (err) snprintf(err, errn, __VA_ARGS__); rc = -1; goto out; } while (0)
    char url[512];
    strlcpy(url, start_url, sizeof(url));
    int rc = 0;
    struct conn c = { 0 };
    uint8_t *buf = kmalloc(65536 + 1);
    struct dl d = { 0 };
    d.cb = cb;
    d.ctx = ctx;
    d.max = max_bytes ? max_bytes : (1ull << 40);
    if (!buf) { if (err) snprintf(err, errn, "out of memory"); return -1; }
    for (int hop = 0; hop < 6; hop++) {
        bool https;
        char host[128], path[1024];
        uint16_t port;
        if (!parse_url(url, &https, host, sizeof(host), &port, path, sizeof(path))) FAIL("that is not a valid address");
        struct net_info ni;
        if (!net_get_info(&ni) || !ni.ip) FAIL("no network connection");
        uint32_t ip;
        if (!str_to_ip(host, &ip) && !net_resolve(host, &ip, 4000)) FAIL("could not find the server '%s'", host);
        c.sock = tcp_connect(ip, port, 8000);
        if (!c.sock) FAIL("%s did not accept a connection on port %u", host, port);
        if (https) {
            char terr[160] = "";
            c.tls = tls_connect(c.sock, host, 10000, terr, sizeof(terr));
            if (!c.tls) { tcp_close(c.sock); c.sock = NULL; FAIL("%s", terr[0] ? terr : "the secure connection failed"); }
        }
        char hostport[140];
        if ((https && port == 443) || (!https && port == 80)) strlcpy(hostport, host, sizeof(hostport));
        else snprintf(hostport, sizeof(hostport), "%s:%u", host, port);
        char req[1500];
        int n = snprintf(req, sizeof(req),
                         "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: ZenithOS/" ZENITH_VERSION " pkg\r\n"
                         "Accept: */*\r\nConnection: close\r\n\r\n", path, hostport);
        if (conn_send(&c, req, (size_t)n) != n) FAIL("sending the request failed");

        /* headers */
        size_t len = 0, hdr_end = 0;
        uint64_t deadline = uptime_ms() + 30000;
        while (!hdr_end && len < 16384) {
            long got = conn_recv(&c, buf + len, 16384 - len, 2000);
            if (got == -2) { if (uptime_ms() > deadline) FAIL("the server took too long"); continue; }
            if (got <= 0) break;
            len += (size_t)got;
            for (size_t i = 0; i + 3 < len && !hdr_end; i++)
                if (!memcmp(buf + i, "\r\n\r\n", 4)) hdr_end = i + 4;
        }
        if (!hdr_end || len < 12 || memcmp(buf, "HTTP/", 5)) FAIL("the server's answer was not HTTP");
        uint8_t first_body_byte = buf[hdr_end];              /* the header parser wants a terminated string */
        buf[hdr_end] = 0;
        int status = atoi((char *)buf + 9);
        if (status >= 300 && status < 400) {
            char loc[512];
            if (!header((char *)buf, hdr_end, "Location", loc, sizeof(loc))) FAIL("the server redirected without saying where");
            char next[512];
            const char *h = strstr(url, "://");
            if (strstr(loc, "://")) strlcpy(next, loc, sizeof(next));
            else {
                const char *slash = h ? strchr(h + 3, '/') : NULL;
                size_t keep = slash ? (size_t)(slash - url) : strlen(url);
                snprintf(next, sizeof(next), "%.*s%s%s", (int)keep, url, loc[0] == '/' ? "" : "/", loc);
            }
            if (c.tls) tls_close(c.tls);
            tcp_close(c.sock);
            c.tls = NULL;
            c.sock = NULL;
            strlcpy(url, next, sizeof(url));
            continue;
        }
        if (status < 200 || status >= 300) FAIL("the server answered %d", status);
        char v[64];
        if (header((char *)buf, hdr_end, "Content-Length", v, sizeof(v))) {
            d.total = (uint64_t)strtol(v, NULL, 10);
            d.have_len = true;
            if (d.total > d.max) FAIL("the file is larger than the allowed %lu MiB", d.max >> 20);
        }
        if (header((char *)buf, hdr_end, "Transfer-Encoding", v, sizeof(v)) && strstr_ci(v, "chunked")) d.chunked = true;
        buf[hdr_end] = first_body_byte;
        d.vn = vfs_open(dest, true);
        if (!d.vn) FAIL("cannot create %s", dest);
        vfs_truncate(d.vn, 0);
        dl_feed(&d, buf + hdr_end, len - hdr_end);
        uint64_t last = uptime_ms();
        while (!d.done && !d.failed) {
            long got = conn_recv(&c, buf, 65536, 2000);
            if (got == -2) { if (uptime_ms() - last > 30000) FAIL("the download stalled"); continue; }
            if (got <= 0) break;
            last = uptime_ms();
            dl_feed(&d, buf, (size_t)got);
        }
        if (d.failed) FAIL("could not write %s (disk full?)", dest);
        if (d.have_len && d.off < d.total) FAIL("the download was cut off at %lu of %lu bytes", d.off, d.total);
        if (d.chunked && !d.done) FAIL("the download was cut off");
        if (cb) cb(ctx, d.off, d.off);
        goto out;
    }
    FAIL("too many redirects");
out:
    if (d.vn) vfs_close(d.vn);
    if (c.tls) tls_close(c.tls);
    if (c.sock) tcp_close(c.sock);
    kfree(buf);
    if (rc && d.vn) vfs_unlink(dest, false);              /* never leave half a file behind */
    return rc;
#undef FAIL
}
