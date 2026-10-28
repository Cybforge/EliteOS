/* Certificate chain checks on a test PKI made by mkpki.sh. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "crypto.h"
#include "x509.h"
#include "vfs.h"
void *kmalloc(size_t n) { return malloc(n); }
void *kzalloc(size_t n) { return calloc(1, n); }
void *krealloc(void *p, size_t n) { return realloc(p, n); }
void kfree(void *p) { free(p); }
void klog(const char *f, ...) {}
void random_bytes(void *o, size_t n) {}
static char *slurp(const char *p, size_t *n) { FILE *f = fopen(p, "rb"); if (!f) return NULL; char *b = malloc(1 << 20); *n = fread(b, 1, 1 << 20, f); fclose(f); b[*n] = 0; return b; }
struct fs_mount *vfs_mounts(void) { return NULL; }
int vfs_list(const char *p, struct vfs_dirent *d, int m) { strcpy(d[0].name, "zenith-test-root.pem"); return 1; }
char *vfs_read_file(const char *p, size_t *n) { return slurp("root.pem", n); }
static int fails;
static struct x509 chain[4];
static uint8_t *ders[8]; static size_t lens[8];
static int load(const char **files, int k) {
    for (int i = 0; i < k; i++) { size_t n; char *pem = slurp(files[i], &n); pem_decode(pem, n, &ders[i], &lens[i], 1); x509_parse(ders[i], lens[i], &chain[i]); }
    return k;
}
static void expect(const char *name, int want_ok, const char **files, int k, const char *host, long now) {
    char err[128] = "";
    load(files, k);
    int r = x509_verify_chain(chain, k, host, now, err, sizeof(err));
    int ok = (r == 0) == want_ok;
    printf("%s %-44s %s\n", ok ? "ok  " : "FAIL", name, r ? err : "trusted");
    if (!ok) fails++;
}
int main(void) {
    long now = (long)time(NULL);
    const char *good[] = { "leaf.pem", "int.pem" }, *pss[] = { "leaf_pss.pem", "int.pem" };
    const char *noint[] = { "leaf.pem" }, *evil[] = { "leaf_evil.pem", "evil.pem" }, *withroot[] = { "leaf.pem", "int.pem", "root.pem" };
    printf("trust store: %d roots\n", trust_count());
    expect("valid chain for localhost", 1, good, 2, "localhost", now);
    expect("valid chain with root included", 1, withroot, 3, "localhost", now);
    expect("wildcard *.zenith.test", 1, good, 2, "www.zenith.test", now);
    expect("IP address SAN 10.0.2.2", 1, good, 2, "10.0.2.2", now);
    expect("RSA-PSS signed leaf", 1, pss, 2, "localhost", now);
    expect("wrong host name", 0, good, 2, "example.com", now);
    expect("wildcard does not match two labels", 0, good, 2, "a.b.zenith.test", now);
    expect("missing intermediate", 0, noint, 1, "localhost", now);
    expect("untrusted root", 0, evil, 2, "localhost", now);
    expect("expired (clock 2 years ahead)", 0, good, 2, "localhost", now + 2 * 365 * 86400L);
    expect("not yet valid (clock in 2001)", 0, good, 2, "localhost", 1000000000L);
    /* tamper with the leaf signature */
    load(good, 2); ((uint8_t *)chain[0].sig)[10] ^= 1;
    char err[128]; int r = x509_verify_chain(chain, 2, "localhost", now, err, sizeof(err));
    printf("%s %-44s %s\n", r ? "ok  " : "FAIL", "tampered leaf signature", r ? err : "trusted"); if (!r) fails++;
    printf("\n%d failures\n", fails);
    return fails;
}
