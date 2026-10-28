/* PBKDF2 with HMAC-SHA-256 (RFC 8018) for password hashing. */
#include <kernel.h>
#include <crypto.h>

void pbkdf2_sha256(const uint8_t *pass, size_t plen, const uint8_t *salt, size_t slen, uint32_t iterations,
                   uint8_t *out, size_t outlen)
{
    uint8_t block[64], u[32], t[32];
    for (uint32_t i = 1; outlen; i++) {
        if (slen + 4 > sizeof(block)) return;                  /* salts are 16 bytes here */
        memcpy(block, salt, slen);
        block[slen] = (uint8_t)(i >> 24);
        block[slen + 1] = (uint8_t)(i >> 16);
        block[slen + 2] = (uint8_t)(i >> 8);
        block[slen + 3] = (uint8_t)i;
        hmac(HASH_SHA256, pass, plen, block, slen + 4, u);
        memcpy(t, u, 32);
        for (uint32_t k = 1; k < iterations; k++) {
            hmac(HASH_SHA256, pass, plen, u, 32, u);
            for (int j = 0; j < 32; j++) t[j] ^= u[j];
        }
        size_t n = outlen < 32 ? outlen : 32;
        memcpy(out, t, n);
        out += n;
        outlen -= n;
    }
}
