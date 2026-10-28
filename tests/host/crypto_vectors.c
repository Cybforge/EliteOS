/* Hashes, HMAC, HKDF, AES, GCM, ChaCha20-Poly1305 against RFC and NIST test vectors. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "crypto.h"
static int fails;
static void hex(const char *h, uint8_t *o, size_t *n) { size_t l = strlen(h) / 2; for (size_t i = 0; i < l; i++) sscanf(h + 2 * i, "%2hhx", &o[i]); if (n) *n = l; }
static void check(const char *name, const uint8_t *got, const char *want) {
    uint8_t w[512]; size_t n; hex(want, w, &n);
    if (memcmp(got, w, n)) { printf("FAIL %s\n  got  ", name); for (size_t i = 0; i < n; i++) printf("%02x", got[i]); printf("\n  want %s\n", want); fails++; }
    else printf("ok   %s\n", name);
}
int main(void) {
    uint8_t out[128], key[64], iv[16], pt[256], aad[64], tag[16], ct[256]; size_t kl, pl, al;
    sha256("abc", 3, out); check("sha256 abc", out, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char *m2 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256(m2, strlen(m2), out); check("sha256 448", out, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    static char mil[1000000]; memset(mil, 'a', sizeof(mil));
    sha256(mil, sizeof(mil), out); check("sha256 million a", out, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    sha384("abc", 3, out); check("sha384 abc", out, "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7");
    sha512("abc", 3, out); check("sha512 abc", out, "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    const char *m3 = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    sha512(m3, strlen(m3), out); check("sha512 896", out, "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909");
    /* HMAC RFC 4231 case 2 */
    hmac(HASH_SHA256, (const uint8_t *)"Jefe", 4, "what do ya want for nothing?", 28, out);
    check("hmac-sha256 rfc4231#2", out, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    hmac(HASH_SHA384, (const uint8_t *)"Jefe", 4, "what do ya want for nothing?", 28, out);
    check("hmac-sha384 rfc4231#2", out, "af45d2e376484031617f78d2b58a6b1b9c7ef464f5a01b47e42ec3736322445e8e2240ca5e69e2c78b3239ecfab21649");
    /* HMAC with a key longer than the block: RFC 4231 case 6 */
    memset(key, 0xaa, 64); uint8_t k131[131]; memset(k131, 0xaa, 131);
    hmac(HASH_SHA256, k131, 131, "Test Using Larger Than Block-Size Key - Hash Key First", 54, out);
    check("hmac-sha256 rfc4231#6", out, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    /* HKDF RFC 5869 case 1 */
    uint8_t ikm[22], salt[13], info[10], prk[64], okm[42]; size_t il, sl, nl;
    hex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", ikm, &il); hex("000102030405060708090a0b0c", salt, &sl); hex("f0f1f2f3f4f5f6f7f8f9", info, &nl);
    hkdf_extract(HASH_SHA256, salt, sl, ikm, il, prk); check("hkdf prk", prk, "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5");
    hkdf_expand(HASH_SHA256, prk, info, nl, okm, 42); check("hkdf okm", okm, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");
    /* PBKDF2-HMAC-SHA256 (RFC 7914 section 11 and the widely published vectors) */
    pbkdf2_sha256((const uint8_t *)"password", 8, (const uint8_t *)"salt", 4, 1, out, 32);
    check("pbkdf2 c=1", out, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    pbkdf2_sha256((const uint8_t *)"password", 8, (const uint8_t *)"salt", 4, 2, out, 32);
    check("pbkdf2 c=2", out, "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");
    pbkdf2_sha256((const uint8_t *)"password", 8, (const uint8_t *)"salt", 4, 4096, out, 32);
    check("pbkdf2 c=4096", out, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
    pbkdf2_sha256((const uint8_t *)"passwd", 6, (const uint8_t *)"salt", 4, 1, out, 64);
    check("pbkdf2 rfc7914 c=1 dk64", out, "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783");
    /* AES FIPS-197 */
    struct aes a;
    hex("000102030405060708090a0b0c0d0e0f", key, &kl); hex("00112233445566778899aabbccddeeff", pt, &pl);
    aes_setkey(&a, key, 16); aes_encrypt_block(&a, pt, out); check("aes-128 fips197", out, "69c4e0d86a7b0430d8cdb78070b4c55a");
    hex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", key, &kl);
    aes_setkey(&a, key, 32); aes_encrypt_block(&a, pt, out); check("aes-256 fips197", out, "8ea2b7ca516745bfeafc49904b496089");
    /* GCM: McGrew/Viega test case 4 (AES-128, 60-byte pt, 20-byte aad) */
    struct gcm g;
    hex("feffe9928665731c6d6a8f9467308308", key, &kl); gcm_setkey(&g, key, 16);
    hex("cafebabefacedbaddecaf888", iv, NULL);
    hex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39", pt, &pl);
    hex("feedfacedeadbeeffeedfacedeadbeefabaddad2", aad, &al);
    gcm_seal(&g, iv, aad, al, pt, pl, ct, tag);
    check("gcm-128 tc4 ct", ct, "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091");
    check("gcm-128 tc4 tag", tag, "5bc94fbc3221a5db94fae95ae7121a47");
    uint8_t back[256]; printf("%s gcm open\n", gcm_open(&g, iv, aad, al, ct, pl, back, tag) && !memcmp(back, pt, pl) ? "ok  " : (fails++, "FAIL"));
    tag[0] ^= 1; printf("%s gcm rejects bad tag\n", !gcm_open(&g, iv, aad, al, ct, pl, back, tag) ? "ok  " : (fails++, "FAIL"));
    /* GCM test case 16 (AES-256) */
    hex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308", key, &kl); gcm_setkey(&g, key, 32);
    gcm_seal(&g, iv, aad, al, pt, pl, ct, tag);
    check("gcm-256 tc16 ct", ct, "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662");
    check("gcm-256 tc16 tag", tag, "76fc6ece0f4e1768cddf8853bb2d551b");
    /* ChaCha20-Poly1305 RFC 8439 2.8.2 */
    hex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f", key, &kl);
    uint8_t nonce[12]; hex("070000004041424344454647", nonce, NULL);
    hex("50515253c0c1c2c3c4c5c6c7", aad, &al);
    const char *sun = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
    pl = strlen(sun);
    chachapoly_seal(key, nonce, aad, al, (const uint8_t *)sun, pl, ct, tag);
    check("chacha20-poly1305 ct", ct, "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc3ff4def08e4b7a9de576d26586cec64b6116");
    check("chacha20-poly1305 tag", tag, "1ae10b594f09e26a7e902ecbd0600691");
    printf("%s chachapoly open\n", chachapoly_open(key, nonce, aad, al, ct, pl, back, tag) && !memcmp(back, sun, pl) ? "ok  " : (fails++, "FAIL"));
    /* Poly1305 RFC 8439 2.5.2 */
    hex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b", key, &kl);
    poly1305(key, (const uint8_t *)"Cryptographic Forum Research Group", 34, tag); check("poly1305", tag, "a8061dc1305136c6c22b8baf0c0127a9");
    printf("\n%d failures\n", fails);
    return fails != 0;
}
