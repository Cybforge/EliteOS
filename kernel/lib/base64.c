/*
 * Standard base64 (RFC 4648), used for password hashes, signatures and keys.
 */
#include <kernel.h>
#include <pkg.h>

static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void base64_encode(const uint8_t *in, size_t n, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = chars[v >> 18];
        out[o++] = chars[(v >> 12) & 63];
        out[o++] = i + 1 < n ? chars[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? chars[v & 63] : '=';
    }
    out[o] = 0;
}

/* decodes up to the first '=' or whitespace; returns the byte count, or -1 on a bad character or overflow */
int base64_decode(const char *in, uint8_t *out, size_t max)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (; *in && *in != '=' && *in != '\n' && *in != '\r' && *in != ' '; in++) {
        const char *p = strchr(chars, *in);
        if (!p) return -1;
        acc = acc << 6 | (uint32_t)(p - chars);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= max) return -1;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return (int)n;
}
