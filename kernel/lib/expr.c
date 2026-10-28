/* Arithmetic expression evaluator (shared by the shell and the calculator). */
#include <kernel.h>
#include <tty.h>

struct P {
    const char *s;
    const char *err;
};

static double expr(struct P *p);

static void ws(struct P *p) { while (*p->s == ' ' || *p->s == '\t') p->s++; }

static bool match(struct P *p, const char *tok)
{
    ws(p);
    size_t n = strlen(tok);
    if (!strncmp(p->s, tok, n)) { p->s += n; return true; }
    return false;
}

static double number(struct P *p)
{
    double v = 0;
    const char *s = p->s;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        while (isalnum(*s)) {
            int d = isdigit(*s) ? *s - '0' : tolower(*s) - 'a' + 10;
            if (d > 15) break;
            v = v * 16 + d;
            s++;
        }
        p->s = s;
        return v;
    }
    while (isdigit(*s)) v = v * 10 + (*s++ - '0');
    if (*s == '.' || *s == ',') {
        s++;
        double f = 0.1;
        while (isdigit(*s)) { v += (*s++ - '0') * f; f /= 10; }
    }
    if (*s == 'e' || *s == 'E') {
        const char *q = s + 1;
        int sign = 1;
        if (*q == '-') { sign = -1; q++; } else if (*q == '+') q++;
        if (isdigit(*q)) {
            int e = 0;
            while (isdigit(*q)) e = e * 10 + (*q++ - '0');
            v *= k_pow(10, sign * e);
            s = q;
        }
    }
    p->s = s;
    return v;
}

static double primary(struct P *p)
{
    ws(p);
    if (match(p, "(")) {
        double v = expr(p);
        if (!match(p, ")")) p->err = "missing )";
        return v;
    }
    if (isdigit(*p->s) || *p->s == '.') return number(p);
    if (match(p, "\xCF\x80") || match(p, "pi")) return K_PI;           /* π */
    if (match(p, "\xE2\x88\x9A")) return k_sqrt(primary(p));          /* √ */
    static const struct { const char *name; int id; } fns[] = {
        { "sqrt", 1 }, { "sin", 2 }, { "cos", 3 }, { "tan", 4 }, { "abs", 5 }, { "ln", 6 },
        { "log", 7 }, { "exp", 8 }, { "floor", 9 }, { "round", 10 }, { "ceil", 11 },
    };
    for (unsigned i = 0; i < ARRAY_SIZE(fns); i++) {
        const char *save = p->s;
        if (match(p, fns[i].name)) {
            ws(p);
            if (*p->s != '(') { p->s = save; break; }
            double a = primary(p);
            switch (fns[i].id) {
            case 1: if (a < 0) p->err = "sqrt of negative"; return k_sqrt(a);
            case 2: return k_sin(a);
            case 3: return k_cos(a);
            case 4: return k_tan(a);
            case 5: return k_fabs(a);
            case 6: if (a <= 0) p->err = "ln domain"; return k_log(a);
            case 7: if (a <= 0) p->err = "log domain"; return k_log(a) / k_log(10);
            case 8: return k_exp(a);
            case 9: return k_floor(a);
            case 10: return k_floor(a + 0.5);
            case 11: return -k_floor(-a);
            }
        }
    }
    if (match(p, "e")) return 2.718281828459045;
    p->err = "syntax error";
    return 0;
}

static double postfix(struct P *p)
{
    double v = primary(p);
    for (;;) {
        ws(p);
        if (*p->s == '!') {
            p->s++;
            if (v < 0 || v > 170 || k_floor(v) != v) { p->err = "bad factorial"; return 0; }
            double r = 1;
            for (int i = 2; i <= (int)v; i++) r *= i;
            v = r;
        } else if (*p->s == '%') {
            p->s++;
            v /= 100;
        } else {
            return v;
        }
    }
}

static double unary(struct P *p)
{
    ws(p);
    if (match(p, "-")) return -unary(p);
    if (match(p, "+")) return unary(p);
    double b = postfix(p);
    if (match(p, "^") || match(p, "**")) return k_pow(b, unary(p));
    return b;
}

static double term(struct P *p)
{
    double v = unary(p);
    for (;;) {
        if (match(p, "*") || match(p, "\xC3\x97")) v *= unary(p);            /* × */
        else if (match(p, "/") || match(p, "\xC3\xB7")) {                     /* ÷ */
            double d = unary(p);
            if (d == 0) p->err = "division by zero";
            else v /= d;
        } else if (match(p, "mod")) {
            double d = unary(p);
            if (d == 0) p->err = "division by zero";
            else v = k_fmod(v, d);
        } else return v;
    }
}

static double expr(struct P *p)
{
    double v = term(p);
    for (;;) {
        if (match(p, "+")) v += term(p);
        else if (match(p, "-") || match(p, "\xE2\x88\x92")) v -= term(p);      /* − */
        else return v;
    }
}

bool expr_eval(const char *s, double *out, const char **err)
{
    struct P p = { s, NULL };
    double v = expr(&p);
    ws(&p);
    if (!p.err && *p.s) p.err = "unexpected input";
    if (err) *err = p.err;
    if (p.err) return false;
    *out = v;
    return true;
}

void expr_format(double v, char *buf, size_t n)
{
    if (v != v) { strlcpy(buf, "NaN", n); return; }
    double a = k_fabs(v);
    if (a >= 1e15 || (a < 1e-6 && a > 0)) {
        int e = 0;
        double m = a;
        while (m >= 10) { m /= 10; e++; }
        while (m < 1) { m *= 10; e--; }
        snprintf(buf, n, "%s%.6fe%d", v < 0 ? "-" : "", m, e);
        return;
    }
    snprintf(buf, n, "%.9f", v);
    /* trim trailing zeros */
    char *dot = strchr(buf, '.');
    if (dot) {
        char *e = buf + strlen(buf) - 1;
        while (e > dot && *e == '0') *e-- = 0;
        if (e == dot) *e = 0;
    }
    if (!strcmp(buf, "-0")) strlcpy(buf, "0", n);
}
