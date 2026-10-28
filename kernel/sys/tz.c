/*
 * Time zones from the system's own tz database: TZif files in
 * /usr/share/zoneinfo (the same format every Unix uses, produced from the
 * public-domain IANA data). Past transitions come from the file, dates
 * beyond the last transition from the POSIX rule in its footer, so daylight
 * saving time is computed, not hard-coded.
 */
#include <kernel.h>
#include <mm.h>
#include <vfs.h>
#include <dev.h>
#include <sys.h>

bool rtc_is_local;

struct posix_rule {
    int kind;                       /* 0 = Mm.w.d, 1 = Jn (no leap day), 2 = n (zero based) */
    int m, w, d;                    /* Mm.w.d */
    int n;                          /* Jn / n */
    int time;                       /* seconds after local midnight (may be negative or > 24 h) */
};

struct posix_tz {
    bool valid, has_dst;
    char std[8], dst[8];
    int std_off, dst_off;           /* seconds east of UTC */
    struct posix_rule start, end;
};

static struct {
    char name[48];
    bool loaded;
    int64_t *times;
    uint8_t *idx;
    int ntimes;
    struct { int off; bool dst; int abbr; } types[256];
    int ntypes;
    char chars[256];
    struct posix_tz foot;
} tz;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static int64_t be64(const uint8_t *p) { return (int64_t)((uint64_t)be32(p) << 32 | be32(p + 4)); }

/* ------------------------------------------------------------------------
 * POSIX TZ strings: "CET-1CEST,M3.5.0,M10.5.0/3"
 * ---------------------------------------------------------------------- */

static bool pz_name(const char **s, char *out)
{
    const char *p = *s;
    int n = 0;
    if (*p == '<') {
        p++;
        while (*p && *p != '>' && n < 7) out[n++] = *p++;
        if (*p != '>') return false;
        p++;
    } else {
        while (isalpha(*p) && n < 7) out[n++] = *p++;
        if (n < 3) return false;
    }
    out[n] = 0;
    *s = p;
    return true;
}

static bool pz_time(const char **s, int *out)          /* [+-]hh[:mm[:ss]] in seconds */
{
    const char *p = *s;
    int sign = 1;
    if (*p == '-') { sign = -1; p++; }
    else if (*p == '+') p++;
    if (!isdigit(*p)) return false;
    int v[3] = { 0, 0, 0 };
    for (int i = 0; i < 3; i++) {
        while (isdigit(*p)) v[i] = v[i] * 10 + (*p++ - '0');
        if (*p != ':' || i == 2) break;
        p++;
    }
    *out = sign * (v[0] * 3600 + v[1] * 60 + v[2]);
    *s = p;
    return true;
}

static bool pz_rule(const char **s, struct posix_rule *r)
{
    const char *p = *s;
    r->time = 7200;
    if (*p == 'M') {
        p++;
        r->kind = 0;
        r->m = r->w = r->d = 0;
        while (isdigit(*p)) r->m = r->m * 10 + (*p++ - '0');
        if (*p++ != '.') return false;
        while (isdigit(*p)) r->w = r->w * 10 + (*p++ - '0');
        if (*p++ != '.') return false;
        while (isdigit(*p)) r->d = r->d * 10 + (*p++ - '0');
    } else if (*p == 'J') {
        p++;
        r->kind = 1;
        r->n = 0;
        while (isdigit(*p)) r->n = r->n * 10 + (*p++ - '0');
    } else if (isdigit(*p)) {
        r->kind = 2;
        r->n = 0;
        while (isdigit(*p)) r->n = r->n * 10 + (*p++ - '0');
    } else {
        return false;
    }
    if (*p == '/') {
        p++;
        if (!pz_time(&p, &r->time)) return false;
    }
    *s = p;
    return true;
}

static bool pz_parse(const char *s, struct posix_tz *z)
{
    memset(z, 0, sizeof(*z));
    int off;
    if (!pz_name(&s, z->std) || !pz_time(&s, &off)) return false;
    z->std_off = -off;
    z->valid = true;
    if (!*s) return true;
    if (!pz_name(&s, z->dst)) return false;
    z->has_dst = true;
    z->dst_off = z->std_off + 3600;
    if (*s && *s != ',') {
        if (!pz_time(&s, &off)) return false;
        z->dst_off = -off;
    }
    if (*s == ',') {
        s++;
        if (!pz_rule(&s, &z->start) || *s++ != ',' || !pz_rule(&s, &z->end)) return false;
    } else {
        /* no rule given: the US rules of the time */
        z->start = (struct posix_rule){ 0, 3, 2, 0, 0, 7200 };
        z->end = (struct posix_rule){ 0, 11, 1, 0, 0, 7200 };
    }
    return true;
}

static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

/* the day (as days since 1970) a rule falls on in year y */
static int64_t rule_day(const struct posix_rule *r, int y)
{
    if (r->kind == 0) {
        int64_t first = days_from_civil(y, r->m, 1);
        int wd = (int)((first + 4) % 7);                       /* 0 = Sunday */
        if (wd < 0) wd += 7;
        int day = 1 + (r->d - wd + 7) % 7 + (r->w - 1) * 7;
        static const int mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        int dim = mdays[r->m - 1] + (r->m == 2 && leap(y));
        while (day > dim) day -= 7;
        return first + day - 1;
    }
    int64_t jan1 = days_from_civil(y, 1, 1);
    if (r->kind == 1) return jan1 + r->n - 1 + (leap(y) && r->n >= 60);   /* J60 = March 1 */
    return jan1 + r->n;
}

static int year_of(int64_t utc)
{
    int64_t days = utc / 86400;
    if (utc % 86400 < 0) days--;
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    int64_t doe = days - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int m = (int)(mp < 10 ? mp + 3 : mp - 9);
    return (int)(yoe + era * 400 + (m <= 2));
}

static int pz_offset(const struct posix_tz *z, int64_t utc, bool *dst, const char **abbr)
{
    if (!z->has_dst) {
        *dst = false;
        *abbr = z->std;
        return z->std_off;
    }
    int y = year_of(utc + z->std_off);
    int64_t start = rule_day(&z->start, y) * 86400 + z->start.time - z->std_off;
    int64_t end = rule_day(&z->end, y) * 86400 + z->end.time - z->dst_off;
    bool in = start < end ? (utc >= start && utc < end) : !(utc >= end && utc < start);
    *dst = in;
    *abbr = in ? z->dst : z->std;
    return in ? z->dst_off : z->std_off;
}

/* ------------------------------------------------------------------------
 * TZif
 * ---------------------------------------------------------------------- */

static void tz_clear(void)
{
    kfree(tz.times);
    kfree(tz.idx);
    memset(&tz, 0, sizeof(tz));
}

static bool tz_parse_file(const uint8_t *d, size_t n)
{
    if (n < 44 || memcmp(d, "TZif", 4)) return false;
    int ver = d[4] ? d[4] - '0' : 1;
    size_t off = 0;
    int tsz = 4;
    uint32_t isut = be32(d + 20), isstd = be32(d + 24), leapc = be32(d + 28), timec = be32(d + 32),
             typec = be32(d + 36), charc = be32(d + 40);
    if (ver >= 2) {
        /* skip the version 1 block */
        off = 44 + timec * 4 + timec + typec * 6 + charc + leapc * 8 + isstd + isut;
        if (off + 44 > n || memcmp(d + off, "TZif", 4)) return false;
        isut = be32(d + off + 20); isstd = be32(d + off + 24); leapc = be32(d + off + 28);
        timec = be32(d + off + 32); typec = be32(d + off + 36); charc = be32(d + off + 40);
        tsz = 8;
    }
    size_t body = 44 + off;
    size_t need = timec * (size_t)tsz + timec + typec * 6 + charc + leapc * (size_t)(tsz + 4) + isstd + isut;
    if (body + need > n || typec == 0 || typec > 256 || charc > sizeof(tz.chars)) return false;
    const uint8_t *p = d + body;
    tz.times = kmalloc((timec + 1) * sizeof(int64_t));
    tz.idx = kmalloc(timec + 1);
    if (!tz.times || !tz.idx) return false;
    for (uint32_t i = 0; i < timec; i++) tz.times[i] = tsz == 8 ? be64(p + i * 8) : (int32_t)be32(p + i * 4);
    p += timec * (size_t)tsz;
    for (uint32_t i = 0; i < timec; i++) tz.idx[i] = p[i];
    p += timec;
    for (uint32_t i = 0; i < typec; i++, p += 6) {
        tz.types[i].off = (int32_t)be32(p);
        tz.types[i].dst = p[4];
        tz.types[i].abbr = p[5];
    }
    memcpy(tz.chars, p, charc);
    tz.chars[charc < sizeof(tz.chars) ? charc : sizeof(tz.chars) - 1] = 0;
    p += charc + leapc * (size_t)(tsz + 4) + isstd + isut;
    tz.ntimes = (int)timec;
    tz.ntypes = (int)typec;
    if (ver >= 2 && p < d + n && *p == '\n') {
        const uint8_t *e = p + 1;
        const uint8_t *q = memchr(e, '\n', (size_t)(d + n - e));
        if (q && q - e < 80) {
            char foot[88];
            memcpy(foot, e, (size_t)(q - e));
            foot[q - e] = 0;
            pz_parse(foot, &tz.foot);
        }
    }
    return true;
}

bool tz_set(const char *name)
{
    if (!name || !*name || strstr(name, "..") || name[0] == '/') return false;
    char path[96];
    snprintf(path, sizeof(path), "/usr/share/zoneinfo/%s", name);
    size_t n;
    uint8_t *d = (uint8_t *)vfs_read_file(path, &n);
    if (!d) return false;
    tz_clear();
    bool ok = tz_parse_file(d, n);
    kfree(d);
    if (!ok) { tz_clear(); return false; }
    strlcpy(tz.name, name, sizeof(tz.name));
    tz.loaded = true;
    return true;
}

const char *tz_name(void) { return tz.loaded ? tz.name : "UTC"; }

int tz_offset(int64_t utc, bool *dst, const char **abbr)
{
    static bool d0;
    static const char *a0;
    if (!dst) dst = &d0;
    if (!abbr) abbr = &a0;
    if (!tz.loaded) { *dst = false; *abbr = "UTC"; return 0; }
    int t = -1;
    if (tz.ntimes == 0 || utc < tz.times[0]) {
        /* before the first transition: the first standard-time type */
        t = 0;
        for (int i = 0; i < tz.ntypes; i++)
            if (!tz.types[i].dst) { t = i; break; }
    } else if (utc >= tz.times[tz.ntimes - 1] && tz.foot.valid) {
        return pz_offset(&tz.foot, utc, dst, abbr);
    } else {
        int lo = 0, hi = tz.ntimes - 1;
        while (lo < hi) {
            int mid = (lo + hi + 1) / 2;
            if (tz.times[mid] <= utc) lo = mid;
            else hi = mid - 1;
        }
        t = tz.idx[lo];
    }
    if (t >= tz.ntypes) t = 0;
    *dst = tz.types[t].dst;
    *abbr = tz.chars + (tz.types[t].abbr < (int)sizeof(tz.chars) ? tz.types[t].abbr : 0);
    return tz.types[t].off;
}

int64_t tz_local(int64_t utc)
{
    return utc + tz_offset(utc, NULL, NULL);
}

/* ------------------------------------------------------------------------
 * zone list for the settings and the installer
 * ---------------------------------------------------------------------- */

#define MAX_ZONES 512
static struct tz_zone *zones;
static int nzones;

static int zone_cmp(const void *a, const void *b)
{
    return strcmp(((const struct tz_zone *)a)->name, ((const struct tz_zone *)b)->name);
}

int tz_zones(const struct tz_zone **out)
{
    if (!zones) {
        size_t n;
        char *d = vfs_read_file("/usr/share/zoneinfo/zone1970.tab", &n);
        if (!d) { *out = NULL; return 0; }
        zones = kzalloc(MAX_ZONES * sizeof(*zones));
        for (char *line = d; line && *line && nzones < MAX_ZONES;) {
            char *next = strchr(line, '\n');
            if (next) *next++ = 0;
            if (*line != '#') {
                char *cc = line, *coord = strchr(cc, '\t');
                if (coord) {
                    *coord++ = 0;
                    char *name = strchr(coord, '\t');
                    if (name) {
                        *name++ = 0;
                        char *comment = strchr(name, '\t');
                        if (comment) *comment++ = 0;
                        struct tz_zone *z = &zones[nzones++];
                        strlcpy(z->name, name, sizeof(z->name));
                        char *comma = strchr(cc, ',');
                        if (comma) *comma = 0;
                        strlcpy(z->cc, cc, sizeof(z->cc));
                        if (comment) strlcpy(z->comment, comment, sizeof(z->comment));
                    }
                }
            }
            line = next;
        }
        kfree(d);
        qsort(zones, (size_t)nzones, sizeof(*zones), zone_cmp);
    }
    *out = zones;
    return nzones;
}

int tz_country_name(const char *cc, char *out, size_t n)
{
    size_t sz;
    char *d = vfs_read_file("/usr/share/zoneinfo/iso3166.tab", &sz);
    if (!d) return -1;
    int r = -1;
    for (char *line = d; line && *line;) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (*line != '#' && line[0] == cc[0] && line[1] == cc[1] && line[2] == '\t') {
            strlcpy(out, line + 3, n);
            r = 0;
            break;
        }
        line = next;
    }
    kfree(d);
    return r;
}
