#ifndef ZENITH_PKG_H
#define ZENITH_PKG_H

/*
 * Packages: apps and drivers that are downloaded, checked and installed at
 * run time. A source is a folder of static files (a GitHub repository works):
 *
 *     Packages        stanzas "Key: value" describing every package
 *     Packages.sig    "zenith-sig-v1 <base64 ECDSA P-256 signature of Packages>"
 *     pkg/NAME.zpk   gzip-compressed tar archives with a MANIFEST
 *
 * Sources are listed in /etc/zenith/sources.list, the keys that may sign them
 * in /etc/zenith/keys. A package lands in /opt/<name>; /var/lib/zenith/pkg
 * remembers what is installed. Installing is all-or-nothing: if any package
 * of a transaction fails, everything that was downloaded or unpacked for it
 * is removed again.
 */
#include <kernel.h>

#define PKG_MAX_DEPS 8
#define PKG_MAX_PCI 8

struct pkg {
    char name[48];
    char version[24];
    char type[12];                  /* app, driver, library, data */
    char arch[12];
    char category[24];
    char desc[200];
    char homepage[96];
    char depends[PKG_MAX_DEPS][64]; /* "name" or "name (>= 1.2)" */
    int ndeps;
    char provides[4][64];
    int nprov;
    uint32_t pci[PKG_MAX_PCI];      /* vendor << 16 | device */
    int npci;
    char filename[160];
    uint8_t sha256[32];
    uint64_t size;                  /* download size */
    uint64_t installed_size;
    char module[48];                /* drivers: the .zko inside the package */
    char personality[16];           /* apps: which system call interface they use ("zenith", "linux") */
    char format[12];                /* where it came from: "zpk" (native) or "apk" */
    int source;
    bool verified;                  /* the index it came from carries a valid signature */
    bool installed;
    char installed_version[24];
};

struct pkg_source {
    char name[24];
    char url[200];
    bool require_signature;
    bool ok;                        /* the last update worked */
    char error[100];
    int count;
};

/* progress of a running install, polled by the store app */
struct pkg_job {
    volatile int percent;           /* 0..100 */
    char stage[120];
    char log[16][110];
    volatile int nlog;
    volatile bool finished;
    volatile int result;            /* 0 ok */
    char error[200];
    volatile bool cancel;
};

void pkg_init(void);
void pkg_set_root(const char *prefix);                   /* install onto another volume: its zenith folder; "" for this system */
int  pkg_update(struct pkg_job *job);                    /* refresh every source; returns the number of packages */
int  pkg_count(void);
const struct pkg *pkg_at(int i);
const struct pkg *pkg_lookup(const char *name);
int  pkg_search(const char *query, int *out, int max);  /* indexes into pkg_at(), best first */
int  pkg_sources(const struct pkg_source **out);
int  pkg_for_pci(uint16_t vendor, uint16_t device, int *out, int max);   /* driver packages for a PCI device */
bool pkg_is_installed(const char *name);
int  pkg_install(const char *name, struct pkg_job *job);
int  pkg_remove(const char *name, struct pkg_job *job);
int  pkg_installed_list(const struct pkg **out, int max);
int  pkg_plan(const char *name, const struct pkg **order, int max, char *err, size_t errn);   /* what would be installed, deps first */
int  ver_cmp(const char *a, const char *b);
void pkg_log(struct pkg_job *job, const char *fmt, ...);

/* base64 and hex helpers shared with the account code */
int  base64_decode(const char *in, uint8_t *out, size_t max);
void base64_encode(const uint8_t *in, size_t n, char *out);

#endif
