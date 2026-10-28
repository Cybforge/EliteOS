/* gzip and tar: what packages are made of. */
#include <kernel.h>
#include <mm.h>
#include <vfs.h>
#include <inflate.h>
#include <archive.h>
#include <sys.h>

uint8_t *gunzip_alloc(const uint8_t *d, size_t n, size_t *out_len, size_t max)
{
    if (n < 18 || d[0] != 0x1f || d[1] != 0x8b || d[2] != 8) return NULL;
    int flg = d[3];
    size_t p = 10;
    if (flg & 4) { if (p + 2 > n) return NULL; p += 2 + (size_t)(d[p] | d[p + 1] << 8); }
    if (flg & 8) { while (p < n && d[p]) p++; p++; }
    if (flg & 16) { while (p < n && d[p]) p++; p++; }
    if (flg & 2) p += 2;
    if (p + 8 > n) return NULL;
    uint32_t isize = (uint32_t)(d[n - 4] | d[n - 3] << 8 | d[n - 2] << 16 | (uint32_t)d[n - 1] << 24);
    if (isize > max) return NULL;
    uint8_t *out = kmalloc((size_t)isize + 1);
    if (!out) return NULL;
    size_t got = 0;
    int r = inflate_raw(d + p, n - p - 8, out, isize, &got);
    if (r && r != -2) { kfree(out); return NULL; }
    out[got] = 0;
    if (out_len) *out_len = got;
    return out;
}

static uint64_t octal(const uint8_t *p, int n)
{
    uint64_t v = 0;
    /* base-256 for sizes over 8 GiB */
    if (p[0] & 0x80) {
        for (int i = 1; i < n; i++) v = v << 8 | p[i];
        return v;
    }
    for (int i = 0; i < n && p[i] >= '0' && p[i] <= '7'; i++) v = v * 8 + (uint64_t)(p[i] - '0');
    return v;
}

static bool safe_name(const char *n)
{
    if (!*n || n[0] == '/') return false;
    for (const char *p = n; *p; p++)
        if (p[0] == '.' && p[1] == '.' && (p == n || p[-1] == '/') && (!p[2] || p[2] == '/')) return false;
    return true;
}

/* the name of the entry at h: prefix + name, or a GNU long name when one was announced */
static void entry_name(const uint8_t *h, const char *longname, char *out, size_t n)
{
    if (longname && *longname) { strlcpy(out, longname, n); return; }
    char name[101], prefix[156];
    memcpy(name, h, 100); name[100] = 0;
    memcpy(prefix, h + 345, 155); prefix[155] = 0;
    if (!memcmp(h + 257, "ustar", 5) && prefix[0]) snprintf(out, n, "%s/%s", prefix, name);
    else strlcpy(out, name, n);
}

int tar_extract(const uint8_t *tar, size_t len, const char *dest, char *err, size_t errn, int *nfiles)
{
    int files = 0;
    char longname[256] = "";
    size_t off = 0;
    sys_mkdir_p(dest);
    while (off + 512 <= len) {
        const uint8_t *h = tar + off;
        if (!h[0]) break;                                   /* end-of-archive block */
        uint64_t fsize = octal(h + 124, 12);
        char type = (char)h[156];
        if (off + 512 + fsize > len && type != '5') {
            if (err) snprintf(err, errn, "the archive is cut short");
            return E_IO;
        }
        const uint8_t *body = h + 512;
        if (type == 'L') {                                  /* GNU long file name */
            size_t l = MIN((size_t)fsize, sizeof(longname) - 1);
            memcpy(longname, body, l);
            longname[l] = 0;
        } else if (type == 'x' || type == 'g' || type == 'K') {
            /* pax / long link headers: not needed */
        } else {
            char name[300];
            entry_name(h, longname, name, sizeof(name));
            longname[0] = 0;
            size_t nl = strlen(name);
            if (nl && name[nl - 1] == '/') name[nl - 1] = 0;
            if (name[0] == '.' && name[1] == '/') memmove(name, name + 2, strlen(name + 1));
            if (name[0] && strcmp(name, ".")) {
                if (!safe_name(name)) {
                    if (err) snprintf(err, errn, "unsafe path in the archive: %.80s", name);
                    return E_INVAL;
                }
                char path[VFS_PATH_MAX];
                snprintf(path, sizeof(path), "%s/%s", dest, name);
                if (type == '5') {
                    int r = sys_mkdir_p(path);
                    if (r) { if (err) snprintf(err, errn, "cannot create %.80s", path); return r; }
                } else if (type == '0' || type == 0 || type == '7') {
                    char dir[VFS_PATH_MAX];
                    vfs_dirname(path, dir, sizeof(dir));
                    sys_mkdir_p(dir);
                    int r = vfs_write_file(path, body, (size_t)fsize);
                    if (r) { if (err) snprintf(err, errn, "cannot write %.80s", path); return r; }
                    files++;
                }
                /* links and special files are ignored */
            }
        }
        off += 512 + ((size_t)fsize + 511) / 512 * 512;
    }
    if (nfiles) *nfiles = files;
    return 0;
}

const uint8_t *tar_find(const uint8_t *tar, size_t len, const char *name, size_t *size)
{
    char longname[256] = "";
    size_t off = 0;
    while (off + 512 <= len) {
        const uint8_t *h = tar + off;
        if (!h[0]) break;
        uint64_t fsize = octal(h + 124, 12);
        char type = (char)h[156];
        if (type == 'L') {
            size_t l = MIN((size_t)fsize, sizeof(longname) - 1);
            memcpy(longname, h + 512, l);
            longname[l] = 0;
        } else if (type == '0' || type == 0) {
            char n[300];
            entry_name(h, longname, n, sizeof(n));
            longname[0] = 0;
            const char *cmp = n[0] == '.' && n[1] == '/' ? n + 2 : n;
            if (!strcmp(cmp, name) && off + 512 + fsize <= len) {
                if (size) *size = (size_t)fsize;
                return h + 512;
            }
        }
        off += 512 + ((size_t)fsize + 511) / 512 * 512;
    }
    return NULL;
}
