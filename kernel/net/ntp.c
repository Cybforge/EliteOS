/*
 * Network time (SNTP, RFC 4330): after the network comes up, and then once
 * an hour, ask the time server from /etc/zenith/system.conf (ntp_server) and
 * set the clock, which also updates the hardware clock. The reply must echo
 * our random transmit timestamp, so an off-path attacker cannot forge it.
 */
#include <kernel.h>
#include <net.h>
#include <sched.h>
#include <sys.h>
#include <dev.h>
#include <crypto.h>

#define NTP_UNIX_DELTA 2208988800ull
static struct ntp_status st;
static char wake_chan;
static volatile bool wake_now;

void ntp_status(struct ntp_status *out)
{
    *out = st;
    out->enabled = conf_int(&sysconf, "ntp", 1) != 0;
    strlcpy(out->server, conf_get(&sysconf, "ntp_server", "pool.ntp.org"), sizeof(out->server));
}

void ntp_sync_now(void)
{
    wake_now = true;
    sched_wake(&wake_chan);
}

static bool query(const char *server)
{
    uint32_t ip;
    if (!net_resolve(server, &ip, 4000)) {
        snprintf(st.error, sizeof(st.error), "%s", "could not find the time server");
        return false;
    }
    uint8_t req[48] = { 0x23 }, resp[128];            /* LI 0, version 4, mode 3 (client) */
    uint8_t nonce[8];
    random_bytes(nonce, sizeof(nonce));
    memcpy(req + 40, nonce, 8);
    size_t n = sizeof(resp);
    uint64_t t0 = uptime_ms();
    if (!net_udp_request(ip, 123, req, sizeof(req), resp, &n, 3000)) {
        snprintf(st.error, sizeof(st.error), "%s", "no answer from the time server");
        return false;
    }
    uint64_t rtt = uptime_ms() - t0;
    if (n < 48 || (resp[0] & 7) != 4 || resp[1] == 0 || (resp[0] >> 6) == 3 || memcmp(resp + 24, nonce, 8)) {
        snprintf(st.error, sizeof(st.error), "%s", "the time server's answer was not valid");
        return false;
    }
    uint64_t secs = (uint64_t)resp[40] << 24 | (uint64_t)resp[41] << 16 | (uint64_t)resp[42] << 8 | resp[43];
    uint32_t frac = (uint32_t)resp[44] << 24 | (uint32_t)resp[45] << 16 | (uint32_t)resp[46] << 8 | resp[47];
    if (secs < 2085978496ull) secs += 1ull << 32;        /* NTP era 1 (after 2036) */
    int64_t unix_s = (int64_t)(secs - NTP_UNIX_DELTA);
    /* the reply was made about half a round trip ago */
    uint64_t ms = (uint64_t)frac * 1000 >> 32;
    int64_t now = unix_s + (int64_t)((ms + rtt / 2 + 500) / 1000);
    if (now < 1735689600) {                              /* before 2025: not a sensible answer */
        snprintf(st.error, sizeof(st.error), "%s", "the time server sent an impossible date");
        return false;
    }
    int64_t off = now - rtc_epoch();
    st.last_offset = off;
    st.last_sync = now;
    if (off >= 1 || off <= -1) {
        rtc_set_epoch(now);
        klog("ntp: %s: clock was %lds off, set", server, (long)off);
    } else {
        klog("ntp: %s: clock is right (%lds)", server, (long)off);
    }
    st.synced = true;
    st.error[0] = 0;
    return true;
}

static int ntp_thread(void *arg)
{
    UNUSED(arg);
    int backoff = 30;
    for (;;) {
        struct net_info ni;
        bool net_ok = net_get_info(&ni) && ni.ip && ni.link;
        if (net_ok && conf_int(&sysconf, "ntp", 1)) {
            wake_now = false;
            if (query(conf_get(&sysconf, "ntp_server", "pool.ntp.org"))) {
                backoff = 30;
                sched_wait(&wake_chan, NULL, 3600 * 1000);
            } else {
                klog("ntp: %s", st.error);
                sched_wait(&wake_chan, NULL, backoff * 1000);
                backoff = MIN(backoff * 2, 1800);
            }
        } else {
            sched_wait(&wake_chan, NULL, 2000);
        }
    }
    return 0;
}

void ntp_init(void)
{
    thread_create("ntp", ntp_thread, NULL);
}
