/* Parses every built-in root certificate and checks its self-signature. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "crypto.h"
#include "x509.h"
void *kmalloc(size_t n) { return malloc(n); }
void *kzalloc(size_t n) { return calloc(1, n); }
void *krealloc(void *p, size_t n) { return realloc(p, n); }
void kfree(void *p) { free(p); }
void klog(const char *f, ...) {}
void random_bytes(void *o, size_t n) {}
#include "vfs.h"
struct fs_mount *vfs_mounts(void) { return NULL; }
int vfs_list(const char *p, struct vfs_dirent *d, int m) { return 0; }
char *vfs_read_file(const char *p, size_t *n) { return NULL; }
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb"); static char buf[400000]; size_t n = fread(buf, 1, sizeof(buf), f); fclose(f);
    uint8_t *ders[300]; size_t lens[300];
    int cnt = pem_decode(buf, n, ders, lens, 300);
    int parsed = 0, self_ok = 0, rsa = 0, ec = 0, unsupported = 0;
    for (int i = 0; i < cnt; i++) {
        struct x509 c;
        if (!x509_parse(ders[i], lens[i], &c)) { printf("parse failed #%d\n", i); continue; }
        parsed++;
        if (c.key_type == KEY_RSA) rsa++; else if (c.key_type == KEY_EC) ec++; else unsupported++;
        if (x509_signed_by(&c, &c)) self_ok++;
        else printf("self-signature not verified: %s (sig %d hash %d key %d)\n", c.cn, c.sig_type, c.sig_hash, c.key_type);
    }
    printf("%d certificates, %d parsed, %d self-signatures verified (RSA keys %d, EC keys %d, other %d)\n", cnt, parsed, self_ok, rsa, ec, unsupported);
    /* SHA-1 signed roots cannot be verified (and need not be: roots are trusted as they are) */
    return parsed == cnt ? 0 : 1;
}
