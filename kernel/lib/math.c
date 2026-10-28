/*
 * Small freestanding libm: enough precision for graphics and the calculator.
 */
#include <kernel.h>

double k_fabs(double x) { return x < 0 ? -x : x; }

double k_sqrt(double x)
{
    if (x <= 0) return 0;
    double r;
    __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}

double k_floor(double x)
{
    if (x >= 9.2e18 || x <= -9.2e18) return x;
    int64_t i = (int64_t)x;
    double d = (double)i;
    return (d > x) ? d - 1.0 : d;
}

double k_fmod(double a, double b)
{
    if (b == 0) return 0;
    double q = a / b;
    q = q < 0 ? -k_floor(-q) : k_floor(q);
    return a - q * b;
}

/* sin on [-pi, pi] via reduction to [-pi/2, pi/2] and a degree-13 series */
double k_sin(double x)
{
    const double two_pi = 2 * K_PI;
    x = k_fmod(x, two_pi);
    if (x > K_PI) x -= two_pi;
    if (x < -K_PI) x += two_pi;
    if (x > K_PI / 2) x = K_PI - x;
    else if (x < -K_PI / 2) x = -K_PI - x;
    double x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110 * (1 - x2 / 156))))));
}

double k_cos(double x) { return k_sin(x + K_PI / 2); }

double k_tan(double x)
{
    double c = k_cos(x);
    return c == 0 ? 1e300 : k_sin(x) / c;
}

static double atan_01(double x)
{
    /* atan for |x| <= 1, via argument halving + series */
    int halvings = 0;
    while (k_fabs(x) > 0.2) {
        x = x / (1 + k_sqrt(1 + x * x));
        halvings++;
    }
    double x2 = x * x;
    double r = x * (1 - x2 * (1.0 / 3 - x2 * (1.0 / 5 - x2 * (1.0 / 7 - x2 * (1.0 / 9 - x2 / 11)))));
    return r * (double)(1 << halvings);
}

double k_atan2(double y, double x)
{
    if (x == 0 && y == 0) return 0;
    double ax = k_fabs(x), ay = k_fabs(y);
    double a = ay <= ax ? atan_01(ay / ax) : K_PI / 2 - atan_01(ax / ay);
    if (x < 0) a = K_PI - a;
    return y < 0 ? -a : a;
}

double k_exp(double x)
{
    if (x > 709) return 1e308;
    if (x < -745) return 0;
    /* e^x = 2^k * e^r */
    double k = k_floor(x / 0.6931471805599453 + 0.5);
    double r = x - k * 0.6931471805599453;
    double term = 1, sum = 1;
    for (int i = 1; i < 20; i++) {
        term *= r / i;
        sum += term;
    }
    int64_t ki = (int64_t)k;
    while (ki > 0) { sum *= 2; ki--; }
    while (ki < 0) { sum *= 0.5; ki++; }
    return sum;
}

double k_log(double x)
{
    if (x <= 0) return -1e308;
    int e = 0;
    while (x > 2) { x *= 0.5; e++; }
    while (x < 1) { x *= 2; e--; }
    /* ln(x) = 2 atanh((x-1)/(x+1)) */
    double y = (x - 1) / (x + 1), y2 = y * y, term = y, sum = 0;
    for (int i = 1; i < 40; i += 2) {
        sum += term / i;
        term *= y2;
    }
    return 2 * sum + e * 0.6931471805599453;
}

double k_pow(double b, double e)
{
    if (e == 0) return 1;
    if (b == 0) return 0;
    double ie = k_floor(e);
    if (ie == e && k_fabs(e) < 1e9) {
        int64_t n = (int64_t)(e < 0 ? -e : e);
        double r = 1, p = b;
        while (n) { if (n & 1) r *= p; p *= p; n >>= 1; }
        return e < 0 ? 1 / r : r;
    }
    if (b < 0) return 0;
    return k_exp(e * k_log(b));
}

/* xorshift PRNG */
static uint64_t rng_state = 0x9E3779B97F4A7C15ull;

void ksrand(uint64_t seed) { rng_state = seed ? seed : 0x9E3779B97F4A7C15ull; }

uint32_t krand(void)
{
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return (uint32_t)(x >> 16);
}
