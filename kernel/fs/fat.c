/*
 * FAT32 file system.
 *
 * Mounted volumes live inside the VFS tree: directories and file contents
 * are read the first time they are touched, changes are kept in memory and
 * written back by the VFS syncer (every couple of seconds, on `sync` and
 * before power-off). Directories are always rewritten as a whole, with long
 * file names (VFAT) and unique 8.3 aliases. Also contains mkfs.
 */
#include <block.h>
#include <vfs.h>
#include <mm.h>
#include <dev.h>
#include <gfx.h>
#include <x86.h>

#define FAT_EOC      0x0FFFFFFFu
#define FAT_BAD      0x0FFFFFF7u
#define FAT_CACHE    256
#define ENTRIES_PER_SECTOR (SECTOR_SIZE / 4)

struct fat_cache_ent {
    uint32_t sec;
    bool valid, dirty;
    uint8_t data[SECTOR_SIZE];
};

struct fat {
    struct blockdev *dev;
    uint32_t spc, csize;
    uint32_t reserved, nfats, fatsz;
    uint32_t root_clus, fsinfo;
    uint64_t data_start;
    uint32_t nclusters;             /* valid clusters are 2 .. nclusters + 1 */
    uint32_t free_count, next_free;
    bool meta_dirty;                /* FSInfo needs an update */
    bool wrote;                     /* something was written since the last flush */
    struct fat_cache_ent cache[FAT_CACHE];
    uint8_t *clbuf;
    struct fs_mount mnt;
};

/* ------------------------------------------------------------------------
 * FAT table access through a small sector cache
 * ---------------------------------------------------------------------- */

static bool valid_cluster(struct fat *f, uint32_t cl) { return cl >= 2 && cl <= f->nclusters + 1; }
static uint64_t cluster_lba(struct fat *f, uint32_t cl) { return f->data_start + (uint64_t)(cl - 2) * f->spc; }

static int cache_writeback(struct fat *f, struct fat_cache_ent *e)
{
    if (!e->valid || !e->dirty) return 0;
    for (uint32_t i = 0; i < f->nfats; i++) {
        int r = blk_write(f->dev, f->reserved + (uint64_t)i * f->fatsz + e->sec, 1, e->data);
        if (r) return r;
    }
    e->dirty = false;
    f->wrote = true;
    return 0;
}

static struct fat_cache_ent *cache_get(struct fat *f, uint32_t sec)
{
    struct fat_cache_ent *e = &f->cache[sec % FAT_CACHE];
    if (e->valid && e->sec == sec) return e;
    if (cache_writeback(f, e)) return NULL;
    if (blk_read(f->dev, f->reserved + sec, 1, e->data)) { e->valid = false; return NULL; }
    e->sec = sec;
    e->valid = true;
    e->dirty = false;
    return e;
}

static uint32_t fat_get(struct fat *f, uint32_t cl)
{
    struct fat_cache_ent *e = cache_get(f, cl / ENTRIES_PER_SECTOR);
    if (!e) return FAT_EOC;
    return ((uint32_t *)e->data)[cl % ENTRIES_PER_SECTOR] & 0x0FFFFFFF;
}

static void fat_set(struct fat *f, uint32_t cl, uint32_t v)
{
    struct fat_cache_ent *e = cache_get(f, cl / ENTRIES_PER_SECTOR);
    if (!e) return;
    uint32_t *p = &((uint32_t *)e->data)[cl % ENTRIES_PER_SECTOR];
    *p = (*p & 0xF0000000u) | (v & 0x0FFFFFFF);
    e->dirty = true;
}

static uint32_t fat_alloc(struct fat *f)
{
    uint32_t end = f->nclusters + 2;
    uint32_t start = valid_cluster(f, f->next_free) ? f->next_free : 2;
    uint32_t cl = start;
    do {
        if (fat_get(f, cl) == 0) {
            fat_set(f, cl, FAT_EOC);
            f->next_free = cl + 1;
            if (f->free_count && f->free_count != 0xFFFFFFFF) f->free_count--;
            f->meta_dirty = true;
            return cl;
        }
        if (++cl >= end) cl = 2;
    } while (cl != start);
    return 0;
}

static void fat_free_chain(struct fat *f, uint32_t cl)
{
    uint32_t guard = 0;
    while (valid_cluster(f, cl) && guard++ <= f->nclusters) {
        uint32_t next = fat_get(f, cl);
        fat_set(f, cl, 0);
        f->free_count++;
        if (cl < f->next_free) f->next_free = cl;
        cl = next;
    }
    f->meta_dirty = true;
}

static uint32_t *chain_list(struct fat *f, uint32_t first, uint32_t *count)
{
    uint32_t cap = 16, n = 0;
    uint32_t *v = kmalloc(cap * sizeof(uint32_t));
    for (uint32_t cl = first; valid_cluster(f, cl) && n <= f->nclusters; cl = fat_get(f, cl)) {
        if (n == cap) {
            cap *= 2;
            v = krealloc(v, cap * sizeof(uint32_t));
        }
        v[n++] = cl;
    }
    *count = n;
    return v;
}

/* read or write whole clusters, merging physically contiguous runs */
static int cluster_io(struct fat *f, const uint32_t *v, uint32_t n, uint8_t *buf, bool write)
{
    uint32_t i = 0;
    while (i < n) {
        uint32_t j = i + 1;
        while (j < n && v[j] == v[j - 1] + 1 && j - i < 256) j++;
        uint64_t lba = cluster_lba(f, v[i]);
        uint32_t secs = (j - i) * f->spc;
        uint8_t *p = buf + (size_t)i * f->csize;
        int r = write ? blk_write(f->dev, lba, secs, p) : blk_read(f->dev, lba, secs, p);
        if (r) return r;
        i = j;
    }
    if (write && n) f->wrote = true;
    return 0;
}

/* store len bytes in the chain starting at *first, growing or shrinking it */
static int write_chain(struct fat *f, uint32_t *first, const uint8_t *data, size_t len, uint32_t min_clusters)
{
    uint32_t need = (uint32_t)((len + f->csize - 1) / f->csize);
    if (need < min_clusters) need = min_clusters;
    uint32_t n;
    uint32_t *v = chain_list(f, *first, &n);
    if (n > need) {
        if (need) fat_set(f, v[need - 1], FAT_EOC);
        fat_free_chain(f, v[need]);
        n = need;
    }
    int r = 0;
    while (n < need) {
        uint32_t c = fat_alloc(f);
        if (!c) { r = E_NOMEM; break; }
        if (n) fat_set(f, v[n - 1], c);
        v = krealloc(v, (n + 1) * sizeof(uint32_t));
        v[n++] = c;
    }
    *first = n ? v[0] : 0;
    if (r) {
        klog("fat: %s is full", f->mnt.dev);
        kfree(v);
        return r;
    }
    uint32_t full = (uint32_t)MIN((size_t)need, len / f->csize);
    r = cluster_io(f, v, full, (uint8_t *)data, true);
    for (uint32_t i = full; i < need && !r; i++) {
        memset(f->clbuf, 0, f->csize);
        size_t off = (size_t)i * f->csize;
        if (off < len) memcpy(f->clbuf, data + off, MIN((size_t)f->csize, len - off));
        r = cluster_io(f, &v[i], 1, f->clbuf, true);
    }
    kfree(v);
    return r;
}

/* ------------------------------------------------------------------------
 * names and timestamps
 * ---------------------------------------------------------------------- */

static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int64_t fat_to_epoch(uint16_t date, uint16_t time)
{
    if (!date) return 0;
    int y = 1980 + (date >> 9), m = (date >> 5) & 15, d = date & 31;
    if (m < 1 || m > 12 || d < 1) return 0;
    return days_from_civil(y, m, d) * 86400 + (time >> 11) * 3600 + ((time >> 5) & 63) * 60 + (time & 31) * 2;
}

static void epoch_to_fat(int64_t t, uint16_t *date, uint16_t *time)
{
    struct datetime dt;
    epoch_to_datetime(t, &dt);
    if (dt.year < 1980) { *date = (0 << 9) | (1 << 5) | 1; *time = 0; return; }
    *date = (uint16_t)(((dt.year - 1980) << 9) | (dt.month << 5) | dt.day);
    *time = (uint16_t)((dt.hour << 11) | (dt.minute << 5) | (dt.second / 2));
}

static uint8_t sfn_checksum(const uint8_t *n)
{
    uint8_t s = 0;
    for (int i = 0; i < 11; i++) s = (uint8_t)(((s & 1) << 7) + (s >> 1) + n[i]);
    return s;
}

static bool sfn_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c && strchr("!#$%&'()-@^_`{}~", c));
}

/* does the name fit 8.3 exactly (allowing all-lowercase parts via the NT flags)? */
static bool exact_sfn(const char *name, uint8_t *out, uint8_t *nt)
{
    const char *dot = strrchr(name, '.');
    size_t bl = dot ? (size_t)(dot - name) : strlen(name);
    size_t el = dot ? strlen(dot + 1) : 0;
    if (!bl || bl > 8 || el > 3 || (dot && !el)) return false;
    memset(out, ' ', 11);
    *nt = 0;
    for (int part = 0; part < 2; part++) {
        const char *s = part ? dot + 1 : name;
        size_t l = part ? el : bl;
        bool lower = false, upper = false;
        for (size_t i = 0; i < l; i++) {
            char c = s[i];
            if (c >= 'a' && c <= 'z') { lower = true; c = (char)(c - 32); }
            else if (c >= 'A' && c <= 'Z') upper = true;
            if (!sfn_char(c)) return false;
            out[(part ? 8 : 0) + i] = (uint8_t)c;
        }
        if (lower && upper) return false;
        if (lower) *nt |= part ? 0x10 : 0x08;
    }
    return true;
}

static void make_alias(const char *name, int k, uint8_t *out)
{
    char base[9], ext[4];
    size_t bn = 0, en = 0;
    const char *dot = strrchr(name, '.');
    if (dot == name) dot = NULL;
    for (const char *s = name; *s && (!dot || s < dot) && bn < 8; s++) {
        char c = *s;
        if (c == ' ' || c == '.') continue;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        base[bn++] = sfn_char(c) ? c : '_';
    }
    if (dot)
        for (const char *s = dot + 1; *s && en < 3; s++) {
            char c = *s;
            if (c == ' ' || c == '.') continue;
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            ext[en++] = sfn_char(c) ? c : '_';
        }
    if (!bn) base[bn++] = '_';
    char tail[12];
    int tl = snprintf(tail, sizeof(tail), "~%d", k);
    size_t keep = MIN(bn, (size_t)(8 - tl));
    memset(out, ' ', 11);
    memcpy(out, base, keep);
    memcpy(out + keep, tail, (size_t)tl);
    memcpy(out + 8, ext, en);
}

static void put_short(uint8_t *e, const uint8_t *name11, uint8_t attr, uint8_t nt, uint32_t cl, uint32_t size,
                      int64_t mtime)
{
    memset(e, 0, 32);
    memcpy(e, name11, 11);
    e[11] = attr;
    e[12] = nt;
    uint16_t d, t;
    epoch_to_fat(mtime, &d, &t);
    *(uint16_t *)(e + 14) = t;
    *(uint16_t *)(e + 16) = d;
    *(uint16_t *)(e + 18) = d;
    *(uint16_t *)(e + 20) = (uint16_t)(cl >> 16);
    *(uint16_t *)(e + 22) = t;
    *(uint16_t *)(e + 24) = d;
    *(uint16_t *)(e + 26) = (uint16_t)cl;
    *(uint32_t *)(e + 28) = size;
}

static const int lfn_pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

/* long name entries for name, in on-disk order; returns the number written */
static int put_long(uint8_t *out, const char *name, const uint8_t *alias)
{
    uint16_t u[64];
    int n = 0;
    const char *s = name;
    while (*s && n < 63) {
        uint32_t cp = utf8_next(&s);
        u[n++] = cp > 0xFFFF ? '_' : (uint16_t)cp;
    }
    int count = (n + 12) / 13;
    uint8_t sum = sfn_checksum(alias);
    for (int k = count; k >= 1; k--) {
        uint8_t *e = out + (size_t)(count - k) * 32;
        memset(e, 0, 32);
        e[0] = (uint8_t)(k | (k == count ? 0x40 : 0));
        e[11] = 0x0F;
        e[13] = sum;
        for (int i = 0; i < 13; i++) {
            int idx = (k - 1) * 13 + i;
            uint16_t ch = idx < n ? u[idx] : idx == n ? 0 : 0xFFFF;
            *(uint16_t *)(e + lfn_pos[i]) = ch;
        }
    }
    return count;
}

/* ------------------------------------------------------------------------
 * loading
 * ---------------------------------------------------------------------- */

static int load_dir(struct fat *f, vnode_t *d)
{
    uint32_t n;
    uint32_t *v = chain_list(f, d->ino, &n);
    if (!n) { kfree(v); return 0; }
    uint8_t *buf = kmalloc((size_t)n * f->csize);
    int r = cluster_io(f, v, n, buf, false);
    kfree(v);
    if (r) { kfree(buf); return r; }

    uint16_t lfn[260];
    int lfn_count = 0;
    uint8_t lfn_sum = 0;
    bool have_lfn = false;
    size_t total = (size_t)n * f->csize / 32;
    for (size_t i = 0; i < total; i++) {
        uint8_t *e = buf + i * 32;
        if (e[0] == 0) break;
        if (e[0] == 0xE5) { have_lfn = false; continue; }
        uint8_t attr = e[11];
        if (attr == 0x0F) {
            int ord = e[0] & 0x1F;
            if (e[0] & 0x40) {
                lfn_count = ord;
                lfn_sum = e[13];
                have_lfn = true;
                memset(lfn, 0, sizeof(lfn));
            }
            if (!have_lfn || ord < 1 || ord > 20 || e[13] != lfn_sum) { have_lfn = false; continue; }
            for (int k = 0; k < 13; k++) lfn[(ord - 1) * 13 + k] = *(uint16_t *)(e + lfn_pos[k]);
            continue;
        }
        if (attr & 0x08) {
            if (d == f->mnt.root && !(attr & 0x10)) {
                char label[12];
                memcpy(label, e, 11);
                label[11] = 0;
                for (int k = 10; k >= 0 && label[k] == ' '; k--) label[k] = 0;
                strlcpy(f->mnt.label, label, sizeof(f->mnt.label));
            }
            have_lfn = false;
            continue;
        }
        if (e[0] == '.' && (e[1] == ' ' || e[1] == '.')) { have_lfn = false; continue; }

        char name[64];
        size_t o = 0;
        if (have_lfn && sfn_checksum(e) == lfn_sum) {
            for (int k = 0; k < lfn_count * 13 && lfn[k] && lfn[k] != 0xFFFF; k++) {
                char enc[4];
                int l = utf8_encode(lfn[k], enc);
                if (o + (size_t)l >= sizeof(name)) break;
                memcpy(name + o, enc, (size_t)l);
                o += (size_t)l;
            }
        } else {
            uint8_t nt = e[12];
            for (int k = 0; k < 8 && e[k] != ' '; k++) {
                char c = (char)(k == 0 && e[k] == 0x05 ? 0xE5 : e[k]);
                if ((nt & 0x08) && c >= 'A' && c <= 'Z') c = (char)(c + 32);
                name[o++] = c;
            }
            if (e[8] != ' ') {
                name[o++] = '.';
                for (int k = 8; k < 11 && e[k] != ' '; k++) {
                    char c = (char)e[k];
                    if ((nt & 0x10) && c >= 'A' && c <= 'Z') c = (char)(c + 32);
                    name[o++] = c;
                }
            }
        }
        name[o] = 0;
        have_lfn = false;
        if (!o) continue;
        uint32_t cl = ((uint32_t)*(uint16_t *)(e + 20) << 16) | *(uint16_t *)(e + 26);
        int64_t mtime = fat_to_epoch(*(uint16_t *)(e + 24), *(uint16_t *)(e + 22));
        vfs_fs_child(d, name, (attr & 0x10) ? VN_DIR : VN_FILE, *(uint32_t *)(e + 28), mtime, cl);
    }
    kfree(buf);
    return 0;
}

static int load_file(struct fat *f, vnode_t *n)
{
    if (!n->size) return 0;
    uint32_t cnt;
    uint32_t *v = chain_list(f, n->ino, &cnt);
    uint32_t need = (uint32_t)((n->size + f->csize - 1) / f->csize);
    if (cnt < need) {
        klog("fat: %s: chain of '%s' is short (%u < %u clusters)", f->mnt.dev, n->name, cnt, need);
        need = cnt;
        n->size = MIN(n->size, (size_t)cnt * f->csize);
    }
    uint8_t *buf = kmalloc(MAX((size_t)need * f->csize, 16));
    if (!buf) { kfree(v); return E_NOMEM; }
    int r = cluster_io(f, v, need, buf, false);
    kfree(v);
    if (r) { kfree(buf); return r; }
    n->data = buf;
    n->cap = (size_t)need * f->csize;
    return 0;
}

static int fat_load(struct fs_mount *m, vnode_t *n)
{
    struct fat *f = m->priv;
    return n->type == VN_DIR ? load_dir(f, n) : load_file(f, n);
}

static void fat_release(struct fs_mount *m, vnode_t *n)
{
    struct fat *f = m->priv;
    if (n->ino) fat_free_chain(f, n->ino);
    n->ino = 0;
}

/* ------------------------------------------------------------------------
 * writing back
 * ---------------------------------------------------------------------- */

static int write_dir(struct fat *f, vnode_t *d)
{
    bool is_root = d == f->mnt.root;
    int nchild = 0;
    for (vnode_t *c = d->child; c; c = c->sibling) nchild++;
    size_t cap = ((size_t)nchild * 7 + 4) * 32;
    uint8_t *buf = kzalloc(cap);
    uint8_t (*taken)[11] = kmalloc(sizeof(*taken) * (size_t)(nchild + 1));
    int ntaken = 0;
    size_t o = 0;
    if (is_root) {
        if (f->mnt.label[0]) {
            uint8_t lab[11];
            memset(lab, ' ', 11);
            for (int i = 0; i < 11 && f->mnt.label[i]; i++) {
                char c = f->mnt.label[i];
                lab[i] = (uint8_t)(c >= 'a' && c <= 'z' ? c - 32 : c);
            }
            put_short(buf + o, lab, 0x08, 0, 0, 0, d->mtime);
            o += 32;
        }
    } else {
        uint8_t dot[11], dotdot[11];
        memset(dot, ' ', 11); dot[0] = '.';
        memset(dotdot, ' ', 11); dotdot[0] = dotdot[1] = '.';
        uint32_t up = d->parent && d->parent != f->mnt.root ? d->parent->ino : 0;
        put_short(buf + o, dot, 0x10, 0, d->ino, 0, d->mtime); o += 32;
        put_short(buf + o, dotdot, 0x10, 0, up, 0, d->mtime); o += 32;
    }
    for (vnode_t *c = d->child; c; c = c->sibling) {
        if (c->type != VN_FILE && c->type != VN_DIR) continue;
        uint8_t sfn[11], nt = 0;
        bool exact = exact_sfn(c->name, sfn, &nt);
        if (exact)
            for (int i = 0; i < ntaken; i++) if (!memcmp(taken[i], sfn, 11)) { exact = false; break; }
        if (!exact) {
            nt = 0;
            for (int k = 1; k < 1000000; k++) {
                make_alias(c->name, k, sfn);
                bool clash = false;
                for (int i = 0; i < ntaken && !clash; i++) if (!memcmp(taken[i], sfn, 11)) clash = true;
                if (!clash) break;
            }
            o += (size_t)put_long(buf + o, c->name, sfn) * 32;
        }
        memcpy(taken[ntaken++], sfn, 11);
        put_short(buf + o, sfn, c->type == VN_DIR ? 0x10 : 0x20, nt, c->ino,
                  c->type == VN_DIR ? 0 : (uint32_t)c->size, c->mtime);
        o += 32;
    }
    uint32_t before = d->ino;
    int r = write_chain(f, &d->ino, buf, o, 1);
    if (is_root && d->ino != before) klog("fat: root directory moved?");
    kfree(taken);
    kfree(buf);
    return r;
}

/* new directories get their first cluster before anything refers to them */
static void prealloc_dirs(struct fat *f, vnode_t *d)
{
    if (!d->loaded) return;
    for (vnode_t *c = d->child; c; c = c->sibling) {
        if (c->type != VN_DIR) continue;
        if (!c->ino) {
            c->ino = fat_alloc(f);
            c->dirty = true;
            d->dirty = true;
        }
        prealloc_dirs(f, c);
    }
}

static int sync_node(struct fat *f, vnode_t *n)
{
    int r = 0;
    if (n->type == VN_DIR) {
        if (!n->loaded) return 0;
        for (vnode_t *c = n->child; c; c = c->sibling) {
            int e = sync_node(f, c);
            if (e) r = e;
        }
        if (n->dirty) {
            int e = write_dir(f, n);
            if (!e) n->dirty = false;
            else r = e;
        }
    } else if (n->type == VN_FILE && n->dirty) {
        int e = write_chain(f, &n->ino, n->data, n->size, 0);
        if (!e) n->dirty = false;
        else r = e;
        if (n->parent) n->parent->dirty = true;
    }
    return r;
}

static int flush_meta(struct fat *f)
{
    int r = 0;
    for (int i = 0; i < FAT_CACHE; i++) {
        int e = cache_writeback(f, &f->cache[i]);
        if (e) r = e;
    }
    if (f->meta_dirty && f->fsinfo && !r) {
        uint8_t *s = f->clbuf;
        if (!blk_read(f->dev, f->fsinfo, 1, s) && *(uint32_t *)s == 0x41615252) {
            *(uint32_t *)(s + 488) = f->free_count;
            *(uint32_t *)(s + 492) = f->next_free;
            r = blk_write(f->dev, f->fsinfo, 1, s);
            f->wrote = true;
        }
        f->meta_dirty = false;
    }
    if (f->wrote) {
        blk_flush(f->dev);
        f->wrote = false;
    }
    return r;
}

static int fat_sync(struct fs_mount *m)
{
    struct fat *f = m->priv;
    prealloc_dirs(f, m->root);
    int r = sync_node(f, m->root);
    int e = flush_meta(f);
    return r ? r : e;
}

static void fat_statfs(struct fs_mount *m, uint64_t *total, uint64_t *free)
{
    struct fat *f = m->priv;
    *total = (uint64_t)f->nclusters * f->csize;
    *free = (uint64_t)(f->free_count == 0xFFFFFFFF ? 0 : f->free_count) * f->csize;
}

static void fat_unmount(struct fs_mount *m)
{
    struct fat *f = m->priv;
    f->dev->busy = false;
    kfree(f->clbuf);
    kfree(f);
}

static const struct fs_ops fat_ops = {
    .load = fat_load,
    .release = fat_release,
    .sync = fat_sync,
    .statfs = fat_statfs,
    .unmount = fat_unmount,
};

/* ------------------------------------------------------------------------
 * mounting
 * ---------------------------------------------------------------------- */

static bool parse_bpb(const uint8_t *bs, struct fat *f)
{
    if (bs[510] != 0x55 || bs[511] != 0xAA) return false;
    uint16_t bps = *(uint16_t *)(bs + 11);
    uint8_t spc = bs[13];
    uint16_t reserved = *(uint16_t *)(bs + 14);
    uint8_t nfats = bs[16];
    uint16_t root_ents = *(uint16_t *)(bs + 17);
    uint16_t tot16 = *(uint16_t *)(bs + 19);
    uint16_t fatsz16 = *(uint16_t *)(bs + 22);
    uint32_t tot32 = *(uint32_t *)(bs + 32);
    uint32_t fatsz32 = *(uint32_t *)(bs + 36);
    if (bps != SECTOR_SIZE || !spc || (spc & (spc - 1)) || !reserved || !nfats || nfats > 4) return false;
    if (root_ents || fatsz16 || !fatsz32) return false;         /* FAT12/16 */
    uint64_t total = tot16 ? tot16 : tot32;
    if (!f) return true;
    f->spc = spc;
    f->csize = (uint32_t)spc * SECTOR_SIZE;
    f->reserved = reserved;
    f->nfats = nfats;
    f->fatsz = fatsz32;
    f->root_clus = *(uint32_t *)(bs + 44);
    f->fsinfo = *(uint16_t *)(bs + 48);
    f->data_start = reserved + (uint64_t)nfats * fatsz32;
    if (total <= f->data_start) return false;
    f->nclusters = (uint32_t)((total - f->data_start) / spc);
    uint32_t max_by_fat = fatsz32 * ENTRIES_PER_SECTOR - 2;
    if (f->nclusters > max_by_fat) f->nclusters = max_by_fat;
    return f->nclusters > 0;
}

bool fat_probe(struct blockdev *d, char *label, size_t n)
{
    if (d->sectors < 64) return false;
    uint8_t *bs = kmalloc(SECTOR_SIZE);
    bool ok = !blk_read(d, 0, 1, bs) && parse_bpb(bs, NULL);
    if (ok && label) {
        char l[12];
        memcpy(l, bs + 71, 11);
        l[11] = 0;
        for (int k = 10; k >= 0 && l[k] == ' '; k--) l[k] = 0;
        strlcpy(label, strcmp(l, "NO NAME") ? l : "", n);
    }
    kfree(bs);
    return ok;
}

static uint32_t count_free(struct fat *f)
{
    uint32_t n = 0, chunk = 64;
    uint8_t *buf = kmalloc(chunk * SECTOR_SIZE);
    for (uint32_t s = 0; s < f->fatsz; s += chunk) {
        uint32_t c = MIN(chunk, f->fatsz - s);
        if (blk_read(f->dev, f->reserved + s, c, buf)) break;
        uint32_t *e = (uint32_t *)buf;
        for (uint32_t i = 0; i < c * ENTRIES_PER_SECTOR; i++) {
            uint32_t cl = s * ENTRIES_PER_SECTOR + i;
            if (valid_cluster(f, cl) && !(e[i] & 0x0FFFFFFF)) n++;
        }
    }
    kfree(buf);
    return n;
}

int fat_mount(struct blockdev *d, const char *path)
{
    uint8_t *bs = kmalloc(SECTOR_SIZE);
    struct fat *f = kzalloc(sizeof(*f));
    if (blk_read(d, 0, 1, bs) || !parse_bpb(bs, f)) {
        kfree(bs);
        kfree(f);
        return E_INVAL;
    }
    f->dev = d;
    f->clbuf = kmalloc(f->csize);
    char l[12];
    memcpy(l, bs + 71, 11);
    l[11] = 0;
    for (int k = 10; k >= 0 && l[k] == ' '; k--) l[k] = 0;
    if (strcmp(l, "NO NAME")) strlcpy(f->mnt.label, l, sizeof(f->mnt.label));

    f->free_count = 0xFFFFFFFF;
    f->next_free = 2;
    if (f->fsinfo && f->fsinfo < f->reserved && !blk_read(d, f->fsinfo, 1, bs) &&
        *(uint32_t *)bs == 0x41615252 && *(uint32_t *)(bs + 484) == 0x61417272) {
        uint32_t fc = *(uint32_t *)(bs + 488), nf = *(uint32_t *)(bs + 492);
        if (fc <= f->nclusters) f->free_count = fc;
        if (valid_cluster(f, nf)) f->next_free = nf;
    } else {
        f->fsinfo = 0;
    }
    kfree(bs);
    if (f->free_count == 0xFFFFFFFF) f->free_count = count_free(f);

    f->mnt.ops = &fat_ops;
    f->mnt.priv = f;
    strlcpy(f->mnt.dev, d->name, sizeof(f->mnt.dev));
    strlcpy(f->mnt.fstype, "fat32", sizeof(f->mnt.fstype));
    int r = vfs_mount(path, &f->mnt, f->root_clus);
    if (r) {
        kfree(f->clbuf);
        kfree(f);
        return r;
    }
    d->busy = true;
    vfs_start_syncer();
    return 0;
}

void fat_automount(void)
{
    int k = 0;
    for (int i = 0; i < blk_count(); i++) {
        struct blockdev *d = blk_get(i);
        if (d->busy) continue;
        bool has_parts = false;
        for (int j = 0; j < blk_count(); j++)
            if (blk_get(j)->parent == d) has_parts = true;
        if (has_parts || !fat_probe(d, NULL, 0)) continue;
        char path[16];
        if (k) snprintf(path, sizeof(path), "/disk%d", k + 1);
        else strlcpy(path, "/disk", sizeof(path));
        if (!fat_mount(d, path)) k++;
    }
}

/* ------------------------------------------------------------------------
 * mkfs
 * ---------------------------------------------------------------------- */

int fat_mkfs(struct blockdev *d, const char *label)
{
    uint64_t total = d->sectors;
    if (total > 0xFFFFFFFFull) total = 0xFFFFFFFFull;
    uint64_t bytes = total * SECTOR_SIZE;
    uint32_t max_spc = bytes <= (8ull << 30) ? 8 : bytes <= (16ull << 30) ? 16 : bytes <= (32ull << 30) ? 32 : 64;
    uint32_t reserved = 32, nfats = 2;
    uint32_t spc = max_spc, fatsz = 0, clusters = 0;
    for (;; spc /= 2) {
        fatsz = 1;
        for (int it = 0; it < 8; it++) {
            clusters = (uint32_t)((total - reserved - (uint64_t)nfats * fatsz) / spc);
            fatsz = (uint32_t)(((uint64_t)clusters + 2 + ENTRIES_PER_SECTOR - 1) / ENTRIES_PER_SECTOR);
        }
        clusters = (uint32_t)((total - reserved - (uint64_t)nfats * fatsz) / spc);
        if (clusters >= 65525 || spc == 1) break;
    }
    if (clusters < 65525) return E_INVAL;        /* too small for FAT32 (< ~33 MiB) */

    uint8_t *s = kzalloc(SECTOR_SIZE);
    /* boot sector */
    s[0] = 0xEB; s[1] = 0x58; s[2] = 0x90;
    memcpy(s + 3, "ZENITHOS", 8);
    *(uint16_t *)(s + 11) = SECTOR_SIZE;
    s[13] = (uint8_t)spc;
    *(uint16_t *)(s + 14) = (uint16_t)reserved;
    s[16] = (uint8_t)nfats;
    s[21] = 0xF8;
    *(uint16_t *)(s + 24) = 63;
    *(uint16_t *)(s + 26) = 255;
    *(uint32_t *)(s + 28) = (uint32_t)d->start;
    *(uint32_t *)(s + 32) = (uint32_t)total;
    *(uint32_t *)(s + 36) = fatsz;
    *(uint32_t *)(s + 44) = 2;
    *(uint16_t *)(s + 48) = 1;
    *(uint16_t *)(s + 50) = 6;
    s[64] = 0x80;
    s[66] = 0x29;
    *(uint32_t *)(s + 67) = (uint32_t)rdtsc();
    char lab[12];
    memset(lab, ' ', 11);
    lab[11] = 0;
    for (int i = 0; i < 11 && label && label[i]; i++)
        lab[i] = (char)(label[i] >= 'a' && label[i] <= 'z' ? label[i] - 32 : label[i]);
    memcpy(s + 71, label && *label ? lab : "NO NAME    ", 11);
    memcpy(s + 82, "FAT32   ", 8);
    /* tiny real-mode stub: print a message and halt, should a BIOS ever run it */
    static const uint8_t stub[] = { 0x0E, 0x1F, 0xBE, 0x77, 0x7C, 0xAC, 0x22, 0xC0, 0x74, 0x0B, 0x56, 0xB4,
                                    0x0E, 0xBB, 0x07, 0x00, 0xCD, 0x10, 0x5E, 0xEB, 0xF0, 0xFA, 0xF4, 0xEB, 0xFD };
    memcpy(s + 90, stub, sizeof(stub));
    const char *msg = "ZenithOS needs a UEFI firmware to boot.\r\n";
    memcpy(s + 0x77, msg, strlen(msg) + 1);
    s[510] = 0x55; s[511] = 0xAA;
    int r = blk_write(d, 0, 1, s);
    if (!r) r = blk_write(d, 6, 1, s);

    /* FSInfo */
    memset(s, 0, SECTOR_SIZE);
    *(uint32_t *)s = 0x41615252;
    *(uint32_t *)(s + 484) = 0x61417272;
    *(uint32_t *)(s + 488) = clusters - 1;
    *(uint32_t *)(s + 492) = 3;
    *(uint32_t *)(s + 508) = 0xAA550000;
    if (!r) r = blk_write(d, 1, 1, s);
    if (!r) r = blk_write(d, 7, 1, s);

    /* the FATs: zero, then the reserved entries and the root directory's cluster */
    uint32_t chunk = 128;
    uint8_t *z = kzalloc(chunk * SECTOR_SIZE);
    for (uint32_t fi = 0; fi < nfats && !r; fi++) {
        uint64_t base = reserved + (uint64_t)fi * fatsz;
        for (uint32_t k = 0; k < fatsz && !r; k += chunk)
            r = blk_write(d, base + k, MIN(chunk, fatsz - k), z);
        memset(s, 0, SECTOR_SIZE);
        ((uint32_t *)s)[0] = 0x0FFFFFF8;
        ((uint32_t *)s)[1] = 0x0FFFFFFF;
        ((uint32_t *)s)[2] = 0x0FFFFFFF;
        if (!r) r = blk_write(d, base, 1, s);
    }
    /* empty root directory holding the volume label */
    uint64_t root_lba = reserved + (uint64_t)nfats * fatsz;
    for (uint32_t k = 0; k < spc && !r; k += chunk) r = blk_write(d, root_lba + k, MIN(chunk, spc - k), z);
    if (!r && label && *label) {
        memset(s, 0, SECTOR_SIZE);
        put_short(s, (const uint8_t *)lab, 0x08, 0, 0, 0, rtc_epoch());
        r = blk_write(d, root_lba, 1, s);
    }
    if (!r) r = blk_flush(d);
    kfree(z);
    kfree(s);
    char sz[24];
    blk_format_size((uint64_t)clusters * spc * SECTOR_SIZE, sz, sizeof(sz));
    klog("fat: formatted %s: %s, %u clusters of %u KiB", d->name, sz, clusters, spc / 2);
    return r;
}
