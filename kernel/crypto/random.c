/*
 * Cryptographic random numbers: a ChaCha20 generator keyed from RDRAND
 * (when the CPU has it) and timestamp-counter jitter, re-keyed after
 * every request so earlier output cannot be recovered.
 */
#include <crypto.h>
#include <spinlock.h>
#include <cpu.h>
#include <x86.h>
#include <dev.h>

static uint8_t key[32];
static uint64_t counter;
static bool seeded;
static spinlock_t rng_lock = SPINLOCK_INIT("rng");

static bool rdrand64(uint64_t *v)
{
    uint8_t ok;
    for (int i = 0; i < 10; i++) {
        __asm__ volatile("rdrand %0; setc %1" : "=r"(*v), "=qm"(ok));
        if (ok) return true;
    }
    return false;
}

static void mix_in(bool full)
{
    struct sha256 c;
    sha256_init(&c);
    sha256_update(&c, key, sizeof(key));
    if (cpu_info.rdrand) {
        for (int i = 0; i < (full ? 32 : 4); i++) {
            uint64_t v;
            if (rdrand64(&v)) sha256_update(&c, &v, sizeof(v));
        }
    }
    /* timing jitter between reads of the timestamp counter */
    for (int i = 0; i < (full ? 256 : 16); i++) {
        uint64_t t = rdtsc();
        for (volatile int k = 0; k < (int)(t & 31); k++) {}
        sha256_update(&c, &t, sizeof(t));
    }
    uint64_t up = uptime_ms();
    sha256_update(&c, &up, sizeof(up));
    sha256_final(&c, key);
}

void random_bytes(void *out, size_t len)
{
    uint8_t *o = out;
    spin_lock(&rng_lock);
    if (!seeded) {
        mix_in(true);
        seeded = true;
    } else {
        mix_in(false);
    }
    static const uint8_t nonce[12] = { 'Z', 'e', 'n', 'i', 't', 'h', 'R', 'N', 'G', 0, 0, 1 };
    uint8_t block[64];
    while (len) {
        chacha20_block(key, (uint32_t)counter++, nonce, block);
        size_t k = MIN(len, (size_t)64);
        memcpy(o, block, k);
        o += k;
        len -= k;
    }
    /* fast key erasure */
    chacha20_block(key, (uint32_t)counter++, nonce, block);
    memcpy(key, block, 32);
    memset(block, 0, sizeof(block));
    spin_unlock(&rng_lock);
}
