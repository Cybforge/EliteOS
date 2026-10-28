/*
 * Procedural vector icons: every icon is drawn from primitives at the
 * requested size, so they stay sharp at 16 px and at 64 px.
 */
#include <wm.h>

static void tile(surface_t *s, int x, int y, int sz, uint32_t c1, uint32_t c2)
{
    int r = sz * 26 / 100;
    gfx_round_rect_grad(s, x, y, sz, sz, r, HEX(c1), HEX(c2));
    /* subtle top highlight */
    gfx_round_rect_grad(s, x, y, sz, sz / 2, r, ALPHA(0xFFFFFF, 34), ALPHA(0xFFFFFF, 0));
}

#define W 0xFFFFFFFFu
#define F(v) ((float)(v))

static void gear(surface_t *s, float cx, float cy, float r, color_t c, color_t hole)
{
    for (int i = 0; i < 8; i++) {
        float a = (float)i * (float)K_PI / 4.0f;
        float dx = (float)k_sin(a), dy = -(float)k_cos(a);
        gfx_line_w(s, cx + dx * r * 0.5f, cy + dy * r * 0.5f, cx + dx * r * 1.02f, cy + dy * r * 1.02f,
                   r * 0.42f, c);
    }
    gfx_circle(s, cx, cy, r * 0.78f, c);
    gfx_circle(s, cx, cy, r * 0.34f, hole);
}

void icon_draw(surface_t *s, int icon, int x, int y, int sz)
{
    float fx = F(x), fy = F(y), fs = F(sz);
    float lw = MAX(1.4f, fs * 0.075f);
#define P(px, py) fx + fs * (px), fy + fs * (py)
    switch (icon) {
    case ICON_TERMINAL:
        tile(s, x, y, sz, 0x363B58, 0x14172A);
        gfx_line_w(s, P(0.24f, 0.34f), P(0.42f, 0.50f), lw, HEX(0x5CF2A8));
        gfx_line_w(s, P(0.42f, 0.50f), P(0.24f, 0.66f), lw, HEX(0x5CF2A8));
        gfx_line_w(s, P(0.50f, 0.68f), P(0.74f, 0.68f), lw, W);
        break;
    case ICON_FILES:
    case ICON_FOLDER:
    case ICON_HOME:
        if (icon == ICON_FILES || icon == ICON_HOME) tile(s, x, y, sz, 0xFFC04D, 0xFF8A3D);
        {
            bool bare = icon == ICON_FOLDER;
            float m = bare ? 0.06f : 0.20f;
            color_t back = bare ? HEX(0xE8A33A) : ALPHA(0xFFFFFF, 150);
            color_t front = bare ? HEX(0xFFC857) : W;
            int r = MAX(1, sz / 16);
            gfx_round_rect(s, (int)(fx + fs * m), (int)(fy + fs * (bare ? 0.18f : 0.28f)),
                           (int)(fs * 0.34f), (int)(fs * 0.16f), r, back);
            gfx_round_rect(s, (int)(fx + fs * m), (int)(fy + fs * (bare ? 0.26f : 0.34f)),
                           (int)(fs * (1 - 2 * m)), (int)(fs * (bare ? 0.60f : 0.44f)), r, back);
            gfx_round_rect_grad(s, (int)(fx + fs * m), (int)(fy + fs * (bare ? 0.34f : 0.40f)),
                                (int)(fs * (1 - 2 * m)), (int)(fs * (bare ? 0.52f : 0.38f)), r, front,
                                bare ? HEX(0xF5B041) : ALPHA(0xFFFFFF, 230));
            if (icon == ICON_HOME) {
                gfx_triangle(s, P(0.36f, 0.62f), P(0.50f, 0.50f), P(0.64f, 0.62f), HEX(0xFF9A40));
                gfx_fill(s, (int)(fx + fs * 0.40f), (int)(fy + fs * 0.61f), (int)(fs * 0.20f), (int)(fs * 0.12f), HEX(0xFF9A40));
            }
        }
        break;
    case ICON_EDITOR:
    case ICON_TEXT:
        if (icon == ICON_EDITOR) tile(s, x, y, sz, 0x5B9BFF, 0x2F5BD8);
        {
            float m = icon == ICON_EDITOR ? 0.26f : 0.18f;
            gfx_round_rect(s, (int)(fx + fs * m), (int)(fy + fs * 0.16f), (int)(fs * (1 - 2 * m)),
                           (int)(fs * 0.68f), MAX(1, sz / 14), icon == ICON_EDITOR ? W : HEX(0xF4F6FF));
            color_t lc = icon == ICON_EDITOR ? HEX(0x2F5BD8) : HEX(0x7C8DB5);
            for (int i = 0; i < 4; i++) {
                float ly = 0.30f + i * 0.12f;
                float lx1 = i == 3 ? 0.52f : 1 - m - 0.10f;
                gfx_line_w(s, P(m + 0.10f, ly), P(lx1, ly), MAX(1.0f, fs * 0.045f), lc);
            }
        }
        break;
    case ICON_MONITOR:
        tile(s, x, y, sz, 0x3EE0A4, 0x10A070);
        gfx_line_w(s, P(0.18f, 0.62f), P(0.36f, 0.46f), lw, W);
        gfx_line_w(s, P(0.36f, 0.46f), P(0.50f, 0.58f), lw, W);
        gfx_line_w(s, P(0.50f, 0.58f), P(0.66f, 0.30f), lw, W);
        gfx_line_w(s, P(0.66f, 0.30f), P(0.82f, 0.40f), lw, W);
        gfx_line_w(s, P(0.18f, 0.78f), P(0.82f, 0.78f), MAX(1.0f, lw * 0.6f), ALPHA(0xFFFFFF, 140));
        break;
    case ICON_CUBE: {
        tile(s, x, y, sz, 0xB06BFF, 0x5B3DF0);
        float cx = 0.5f, t = 0.22f, m = 0.47f, b = 0.80f, l = 0.24f, r = 0.76f;
        gfx_triangle(s, P(cx, t), P(r, 0.35f), P(cx, m), ALPHA(0xFFFFFF, 235));
        gfx_triangle(s, P(cx, t), P(l, 0.35f), P(cx, m), ALPHA(0xFFFFFF, 235));
        gfx_triangle(s, P(l, 0.35f), P(cx, m), P(l, 0.66f), ALPHA(0xFFFFFF, 150));
        gfx_triangle(s, P(cx, m), P(l, 0.66f), P(cx, b), ALPHA(0xFFFFFF, 150));
        gfx_triangle(s, P(r, 0.35f), P(cx, m), P(r, 0.66f), ALPHA(0xFFFFFF, 90));
        gfx_triangle(s, P(cx, m), P(r, 0.66f), P(cx, b), ALPHA(0xFFFFFF, 90));
        break;
    }
    case ICON_FRACTAL:
        tile(s, x, y, sz, 0xFF5E9A, 0x9C27B0);
        gfx_circle(s, P(0.56f, 0.52f), fs * 0.22f, ALPHA(0xFFFFFF, 230));
        gfx_circle(s, P(0.30f, 0.52f), fs * 0.11f, ALPHA(0xFFFFFF, 230));
        gfx_circle(s, P(0.56f, 0.24f), fs * 0.07f, ALPHA(0xFFFFFF, 200));
        gfx_circle(s, P(0.56f, 0.80f), fs * 0.07f, ALPHA(0xFFFFFF, 200));
        gfx_circle(s, P(0.17f, 0.52f), fs * 0.05f, ALPHA(0xFFFFFF, 180));
        break;
    case ICON_TETRIS:
    case ICON_GAME: {
        tile(s, x, y, sz, 0x2BC8FF, 0x0068D9);
        float b = 0.17f;
        static const float cells[][2] = { { 0.25f, 0.30f }, { 0.42f, 0.30f }, { 0.59f, 0.30f }, { 0.42f, 0.47f },
                                          { 0.25f, 0.64f }, { 0.42f, 0.64f }, { 0.59f, 0.64f }, { 0.59f, 0.47f } };
        static const uint32_t cols[] = { 0xFFD54F, 0xFFD54F, 0xFFD54F, 0xFFD54F, 0xFF6E9C, 0xFF6E9C, 0x7CFFB2, 0x7CFFB2 };
        for (int i = 0; i < 8; i++)
            gfx_round_rect(s, (int)(fx + fs * cells[i][0]), (int)(fy + fs * cells[i][1]), (int)(fs * b) - 1,
                           (int)(fs * b) - 1, MAX(1, sz / 24), HEX(cols[i]));
        break;
    }
    case ICON_PAINT:
        tile(s, x, y, sz, 0xFF8A50, 0xF0436E);
        gfx_circle(s, P(0.46f, 0.52f), fs * 0.30f, W);
        gfx_circle(s, P(0.36f, 0.40f), fs * 0.055f, HEX(0xFF5C7A));
        gfx_circle(s, P(0.52f, 0.34f), fs * 0.055f, HEX(0xFFC233));
        gfx_circle(s, P(0.64f, 0.46f), fs * 0.055f, HEX(0x35D69A));
        gfx_circle(s, P(0.34f, 0.58f), fs * 0.055f, HEX(0x4F8CFF));
        gfx_circle(s, P(0.56f, 0.66f), fs * 0.07f, HEX(0xF0436E));
        break;
    case ICON_CALC:
        tile(s, x, y, sz, 0x626B8C, 0x363C58);
        gfx_round_rect(s, (int)(fx + fs * 0.24f), (int)(fy + fs * 0.20f), (int)(fs * 0.52f), (int)(fs * 0.16f),
                       MAX(1, sz / 20), HEX(0xB8F5D9));
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                gfx_round_rect(s, (int)(fx + fs * (0.24f + c * 0.19f)), (int)(fy + fs * (0.43f + r * 0.13f)),
                               (int)(fs * 0.14f), (int)(fs * 0.09f), MAX(1, sz / 30),
                               (c == 2 && r == 2) ? HEX(0xFF9A40) : ALPHA(0xFFFFFF, 220));
        break;
    case ICON_CLOCK:
        tile(s, x, y, sz, 0x2A2F4A, 0x101325);
        gfx_circle(s, P(0.5f, 0.5f), fs * 0.32f, W);
        gfx_line_w(s, P(0.5f, 0.5f), P(0.5f, 0.28f), MAX(1.2f, fs * 0.05f), HEX(0x1E2238));
        gfx_line_w(s, P(0.5f, 0.5f), P(0.66f, 0.58f), MAX(1.2f, fs * 0.05f), HEX(0x1E2238));
        gfx_circle(s, P(0.5f, 0.5f), fs * 0.04f, HEX(0xFF5C7A));
        break;
    case ICON_SETTINGS:
        tile(s, x, y, sz, 0x8C93AD, 0x4E556F);
        gear(s, fx + fs * 0.5f, fy + fs * 0.5f, fs * 0.28f, W, HEX(0x6D748E));
        break;
    case ICON_INFO:
        tile(s, x, y, sz, 0x7C5CFF, 0x00A8FF);
        gfx_circle(s, P(0.5f, 0.30f), fs * 0.07f, W);
        gfx_round_rect(s, (int)(fx + fs * 0.43f), (int)(fy + fs * 0.43f), (int)(fs * 0.14f), (int)(fs * 0.34f),
                       MAX(1, sz / 20), W);
        break;
    case ICON_IMAGE:
        tile(s, x, y, sz, 0x38D9A9, 0x1A7FA8);
        gfx_circle(s, P(0.66f, 0.32f), fs * 0.09f, HEX(0xFFE082));
        gfx_triangle(s, P(0.14f, 0.80f), P(0.40f, 0.42f), P(0.66f, 0.80f), W);
        gfx_triangle(s, P(0.44f, 0.80f), P(0.64f, 0.54f), P(0.86f, 0.80f), ALPHA(0xFFFFFF, 190));
        break;
    case ICON_LOG:
        tile(s, x, y, sz, 0xFFC233, 0xE08A00);
        for (int i = 0; i < 4; i++) {
            float ly = 0.30f + i * 0.14f;
            gfx_circle(s, P(0.28f, ly), fs * 0.035f, W);
            gfx_line_w(s, P(0.38f, ly), P(i % 2 ? 0.62f : 0.74f, ly), MAX(1.0f, fs * 0.05f), W);
        }
        break;
    case ICON_SNAKE:
        tile(s, x, y, sz, 0x6BE06F, 0x23913A);
        gfx_line_w(s, P(0.24f, 0.70f), P(0.24f, 0.36f), fs * 0.12f, W);
        gfx_line_w(s, P(0.24f, 0.36f), P(0.56f, 0.36f), fs * 0.12f, W);
        gfx_line_w(s, P(0.56f, 0.36f), P(0.56f, 0.62f), fs * 0.12f, W);
        gfx_line_w(s, P(0.56f, 0.62f), P(0.76f, 0.62f), fs * 0.12f, W);
        gfx_circle(s, P(0.80f, 0.30f), fs * 0.06f, HEX(0xFF5C7A));
        break;
    case ICON_PLASMA:
        tile(s, x, y, sz, 0xFF6EC4, 0x7873F5);
        gfx_ring(s, P(0.5f, 0.5f), fs * 0.24f, fs * 0.08f, ALPHA(0xFFFFFF, 220));
        gfx_ring(s, P(0.5f, 0.5f), fs * 0.10f, fs * 0.06f, ALPHA(0xFFFFFF, 160));
        break;
    case ICON_BROWSER:
    case ICON_NETWORK:
        if (icon == ICON_BROWSER) tile(s, x, y, sz, 0x4FC3F7, 0x1565C0);
        if (icon == ICON_NETWORK && sz >= 28) tile(s, x, y, sz, 0x38C6F4, 0x2563EB);
        {
            color_t c = icon == ICON_BROWSER || sz >= 28 ? W : 0xFFE9EBF8u;
            if (icon == ICON_NETWORK && sz >= 28) {
                /* shrink the glyph inside the tile */
                fx += fs * 0.12f; fy += fs * 0.08f; fs *= 0.76f;
            }
            float cx = 0.5f, cy = icon == ICON_BROWSER ? 0.5f : 0.62f;
            if (icon == ICON_BROWSER) {
                gfx_ring(s, P(cx, cy), fs * 0.28f, MAX(1.2f, fs * 0.05f), c);
                gfx_line_w(s, P(0.22f, 0.5f), P(0.78f, 0.5f), MAX(1.0f, fs * 0.045f), c);
                gfx_arc(s, P(0.5f, 0.5f), fs * 0.28f, MAX(1.0f, fs * 0.045f), 0, (float)(2 * K_PI), c);
                gfx_line_w(s, P(0.5f, 0.22f), P(0.5f, 0.78f), MAX(1.0f, fs * 0.045f), c);
            } else {
                gfx_arc(s, P(cx, cy), fs * 0.40f, fs * 0.08f, (float)(K_PI * 1.70), (float)(K_PI * 2.0), c);
                gfx_arc(s, P(cx, cy), fs * 0.40f, fs * 0.08f, 0, (float)(K_PI * 0.30), c);
                gfx_arc(s, P(cx, cy), fs * 0.24f, fs * 0.08f, (float)(K_PI * 1.70), (float)(K_PI * 2.0), c);
                gfx_arc(s, P(cx, cy), fs * 0.24f, fs * 0.08f, 0, (float)(K_PI * 0.30), c);
                gfx_circle(s, P(cx, cy), fs * 0.07f, c);
            }
        }
        break;
    case ICON_FILE:
    case ICON_APP: {
        int m = sz * 20 / 100;
        color_t pc = icon == ICON_APP ? HEX(0x7C5CFF) : HEX(0xF4F6FF);
        gfx_round_rect(s, x + m, y + sz / 10, sz - 2 * m, sz * 8 / 10, MAX(1, sz / 14), pc);
        gfx_triangle(s, F(x + sz - m) - fs * 0.22f, F(y + sz / 10), F(x + sz - m), F(y + sz / 10) + fs * 0.22f,
                     F(x + sz - m) - fs * 0.22f, F(y + sz / 10) + fs * 0.22f, icon == ICON_APP ? HEX(0xB3A1FF) : HEX(0xC5CCE6));
        if (icon == ICON_APP) {
            gfx_line_w(s, P(0.38f, 0.48f), P(0.48f, 0.58f), MAX(1.2f, fs * 0.05f), W);
            gfx_line_w(s, P(0.48f, 0.58f), P(0.38f, 0.68f), MAX(1.2f, fs * 0.05f), W);
            gfx_line_w(s, P(0.52f, 0.70f), P(0.64f, 0.70f), MAX(1.2f, fs * 0.05f), W);
        }
        break;
    }
    case ICON_POWER:
        gfx_arc(s, P(0.5f, 0.54f), fs * 0.30f, MAX(1.4f, fs * 0.09f), (float)(K_PI * 0.22), (float)(K_PI * 1.78),
                0xFFE9EBF8u);
        gfx_line_w(s, P(0.5f, 0.14f), P(0.5f, 0.50f), MAX(1.4f, fs * 0.09f), 0xFFE9EBF8u);
        break;
    case ICON_RESTART:
        gfx_arc(s, P(0.5f, 0.52f), fs * 0.30f, MAX(1.4f, fs * 0.09f), (float)(K_PI * 0.30), (float)(K_PI * 1.85),
                0xFFE9EBF8u);
        gfx_triangle(s, P(0.50f, 0.08f), P(0.72f, 0.20f), P(0.50f, 0.34f), 0xFFE9EBF8u);
        break;
    case ICON_LOGOUT:
        gfx_line_w(s, P(0.40f, 0.20f), P(0.20f, 0.20f), MAX(1.4f, fs * 0.08f), 0xFFE9EBF8u);
        gfx_line_w(s, P(0.20f, 0.20f), P(0.20f, 0.80f), MAX(1.4f, fs * 0.08f), 0xFFE9EBF8u);
        gfx_line_w(s, P(0.20f, 0.80f), P(0.40f, 0.80f), MAX(1.4f, fs * 0.08f), 0xFFE9EBF8u);
        gfx_line_w(s, P(0.40f, 0.50f), P(0.84f, 0.50f), MAX(1.4f, fs * 0.08f), 0xFFE9EBF8u);
        gfx_line_w(s, P(0.68f, 0.34f), P(0.84f, 0.50f), MAX(1.4f, fs * 0.08f), 0xFFE9EBF8u);
        gfx_line_w(s, P(0.68f, 0.66f), P(0.84f, 0.50f), MAX(1.4f, fs * 0.08f), 0xFFE9EBF8u);
        break;
    case ICON_SEARCH:
        gfx_ring(s, P(0.43f, 0.43f), fs * 0.24f, MAX(1.4f, fs * 0.09f), 0xFFE9EBF8u);
        gfx_line_w(s, P(0.60f, 0.60f), P(0.84f, 0.84f), MAX(1.6f, fs * 0.11f), 0xFFE9EBF8u);
        break;
    case ICON_TRASH:
        gfx_round_rect(s, (int)(fx + fs * 0.26f), (int)(fy + fs * 0.30f), (int)(fs * 0.48f), (int)(fs * 0.56f),
                       MAX(1, sz / 12), HEX(0xC5CCE6));
        gfx_fill(s, (int)(fx + fs * 0.18f), (int)(fy + fs * 0.20f), (int)(fs * 0.64f), MAX(2, sz / 12), HEX(0xC5CCE6));
        break;
    case ICON_DISK:
    case ICON_INSTALL: {
        bool tiled = icon == ICON_INSTALL || sz >= 28;
        if (icon == ICON_INSTALL) tile(s, x, y, sz, 0x60A5FA, 0x4F46E5);
        else if (tiled) tile(s, x, y, sz, 0x64748B, 0x1E293B);
        float top = icon == ICON_INSTALL ? 0.52f : tiled ? 0.30f : 0.24f;
        float h = icon == ICON_INSTALL ? 0.30f : tiled ? 0.40f : 0.52f;
        float m = tiled ? 0.18f : 0.06f;
        int r = MAX(1, sz / 10);
        gfx_round_rect_grad(s, (int)(fx + fs * m), (int)(fy + fs * top), (int)(fs * (1 - 2 * m)), (int)(fs * h), r,
                            HEX(0xE2E8F0), HEX(0x94A3B8));
        gfx_fill(s, (int)(fx + fs * (m + 0.06f)), (int)(fy + fs * (top + h * 0.55f)), (int)(fs * (1 - 2 * m - 0.12f)),
                 MAX(1, sz / 28), HEX(0x64748B));
        gfx_circle(s, P(1 - m - 0.12f, top + h * 0.78f), MAX(1.0f, fs * 0.045f), HEX(0x22C55E));
        if (icon == ICON_INSTALL) {
            gfx_line_w(s, P(0.5f, 0.14f), P(0.5f, 0.42f), MAX(1.4f, fs * 0.08f), W);
            gfx_line_w(s, P(0.36f, 0.30f), P(0.5f, 0.44f), MAX(1.4f, fs * 0.08f), W);
            gfx_line_w(s, P(0.64f, 0.30f), P(0.5f, 0.44f), MAX(1.4f, fs * 0.08f), W);
        }
        break;
    }
    case ICON_PIANO: {
        tile(s, x, y, sz, 0xF472B6, 0x7C3AED);
        float kx0 = fx + fs * 0.16f, kw = fs * 0.68f / 5.0f, ky = fy + fs * 0.24f, kh = fs * 0.54f;
        for (int k = 0; k < 5; k++)
            gfx_round_rect(s, (int)(kx0 + kw * (float)k + 0.5f), (int)ky, MAX(1, (int)kw - 1), (int)kh, MAX(1, sz / 24), W);
        static const int blacks[] = { 1, 2, 4 };
        for (int k = 0; k < 3; k++)
            gfx_round_rect(s, (int)(kx0 + kw * (float)blacks[k] - kw * 0.32f), (int)ky, MAX(1, (int)(kw * 0.64f)),
                           (int)(kh * 0.58f), MAX(1, sz / 32), HEX(0x2A1B4A));
        break;
    }
    case ICON_SOUND: {
        color_t c = sz >= 28 ? W : 0xFFE9EBF8u;
        if (sz >= 28) tile(s, x, y, sz, 0x34D399, 0x0EA5E9);
        gfx_fill(s, (int)(fx + fs * 0.18f), (int)(fy + fs * 0.38f), (int)(fs * 0.16f), (int)(fs * 0.24f), c);
        gfx_triangle(s, P(0.30f, 0.38f), P(0.52f, 0.18f), P(0.52f, 0.82f), c);
        gfx_triangle(s, P(0.30f, 0.38f), P(0.52f, 0.82f), P(0.30f, 0.62f), c);
        gfx_arc(s, P(0.52f, 0.50f), fs * 0.16f, MAX(1.2f, fs * 0.07f), (float)(-K_PI * 0.30), (float)(K_PI * 0.30), c);
        gfx_arc(s, P(0.52f, 0.50f), fs * 0.30f, MAX(1.2f, fs * 0.07f), (float)(-K_PI * 0.30), (float)(K_PI * 0.30), c);
        break;
    }
    case ICON_CHESS: {
        tile(s, x, y, sz, 0xA78BFA, 0x4C1D95);
        /* a knight: base, neck and head */
        color_t c = HEX(0xFBF7EE);
        gfx_round_rect(s, (int)(fx + fs * 0.26f), (int)(fy + fs * 0.72f), (int)(fs * 0.48f), (int)(fs * 0.11f), MAX(1, sz / 24), c);
        gfx_triangle(s, P(0.32f, 0.73f), P(0.66f, 0.73f), P(0.62f, 0.40f), c);
        gfx_triangle(s, P(0.32f, 0.73f), P(0.62f, 0.40f), P(0.40f, 0.30f), c);
        gfx_triangle(s, P(0.40f, 0.30f), P(0.62f, 0.40f), P(0.55f, 0.17f), c);
        gfx_triangle(s, P(0.40f, 0.30f), P(0.24f, 0.46f), P(0.35f, 0.53f), c);
        gfx_circle(s, P(0.47f, 0.33f), MAX(1.0f, fs * 0.035f), HEX(0x4C1D95));
        break;
    }
    case ICON_MINES: {
        tile(s, x, y, sz, 0x94A3B8, 0x334155);
        color_t c = HEX(0x111827);
        for (int k = 0; k < 4; k++) {
            float ang = (float)k * (float)K_PI / 4.0f;
            float dx = (float)k_cos(ang) * fs * 0.30f, dy = (float)k_sin(ang) * fs * 0.30f;
            gfx_line_w(s, fx + fs * 0.5f - dx, fy + fs * 0.5f - dy, fx + fs * 0.5f + dx, fy + fs * 0.5f + dy,
                       MAX(1.2f, fs * 0.06f), c);
        }
        gfx_circle(s, P(0.5f, 0.5f), fs * 0.20f, c);
        gfx_circle(s, P(0.44f, 0.44f), fs * 0.05f, W);
        break;
    }
    case ICON_2048: {
        tile(s, x, y, sz, 0xFCD34D, 0xF97316);
        float g = fs * 0.06f, c = (fs * 0.64f - g) / 2.0f;
        static const uint32_t cols[4] = { 0xFFFBEB, 0xFDE68A, 0xFFFFFF, 0xFEF3C7 };
        for (int k = 0; k < 4; k++)
            gfx_round_rect(s, (int)(fx + fs * 0.18f + (float)(k % 2) * (c + g)), (int)(fy + fs * 0.18f + (float)(k / 2) * (c + g)),
                           (int)c, (int)c, MAX(1, sz / 16), HEX(cols[k]));
        break;
    }
    case ICON_STORE:
        tile(s, x, y, sz, 0x35D07F, 0x0F8A5F);
        /* a shopping bag */
        gfx_round_rect(s, (int)(fx + fs * 0.24f), (int)(fy + fs * 0.38f), (int)(fs * 0.52f), (int)(fs * 0.40f), MAX(1, sz / 14), W);
        gfx_arc(s, P(0.5f, 0.38f), fs * 0.13f, MAX(1.3f, fs * 0.055f), (float)K_PI, (float)(2 * K_PI), W);
        gfx_line_w(s, P(0.5f, 0.50f), P(0.5f, 0.66f), MAX(1.3f, fs * 0.06f), HEX(0x0F8A5F));
        gfx_line_w(s, P(0.43f, 0.58f), P(0.57f, 0.58f), MAX(1.3f, fs * 0.06f), HEX(0x0F8A5F));
        break;
    case ICON_USER:
        tile(s, x, y, sz, 0x9D7CFF, 0x5B3FD8);
        gfx_circle(s, P(0.5f, 0.38f), fs * 0.15f, W);
        gfx_arc(s, P(0.5f, 0.84f), fs * 0.26f, fs * 0.26f, (float)K_PI, (float)(2 * K_PI), W);
        break;
    case ICON_LOCK:
        gfx_arc(s, P(0.5f, 0.40f), fs * 0.18f, MAX(1.5f, fs * 0.08f), (float)K_PI, (float)(2 * K_PI), W);
        gfx_line_w(s, P(0.32f, 0.40f), P(0.32f, 0.48f), MAX(1.5f, fs * 0.08f), W);
        gfx_line_w(s, P(0.68f, 0.40f), P(0.68f, 0.48f), MAX(1.5f, fs * 0.08f), W);
        gfx_round_rect(s, (int)(fx + fs * 0.22f), (int)(fy + fs * 0.46f), (int)(fs * 0.56f), (int)(fs * 0.38f), MAX(1, sz / 12), W);
        gfx_circle(s, P(0.5f, 0.64f), fs * 0.055f, HEX(0x3D2FB8));
        break;
    case ICON_ETHERNET: {
        /* a network plug */
        color_t c = 0xFFE9EBF8u;
        gfx_round_rect(s, (int)(fx + fs * 0.20f), (int)(fy + fs * 0.22f), (int)(fs * 0.60f), (int)(fs * 0.46f), MAX(1, sz / 10), c);
        gfx_round_rect(s, (int)(fx + fs * 0.32f), (int)(fy + fs * 0.30f), (int)(fs * 0.36f), (int)(fs * 0.22f), MAX(1, sz / 16), HEX(0x2A2F4C));
        for (int k = 0; k < 4; k++)
            gfx_line_w(s, P(0.36f + 0.09f * (float)k, 0.52f), P(0.36f + 0.09f * (float)k, 0.60f), MAX(1.0f, fs * 0.04f), HEX(0x2A2F4C));
        gfx_line_w(s, P(0.5f, 0.68f), P(0.5f, 0.82f), MAX(1.5f, fs * 0.08f), c);
        break;
    }
    default:
        tile(s, x, y, sz, 0x7C5CFF, 0x3D2FB8);
        break;
    }
#undef P
}

static const struct { const char *name; int icon; } icon_names[] = {
    { "terminal", ICON_TERMINAL }, { "files", ICON_FILES }, { "editor", ICON_EDITOR }, { "monitor", ICON_MONITOR },
    { "cube", ICON_CUBE }, { "fractal", ICON_FRACTAL }, { "tetris", ICON_TETRIS }, { "paint", ICON_PAINT },
    { "calc", ICON_CALC }, { "clock", ICON_CLOCK }, { "settings", ICON_SETTINGS }, { "info", ICON_INFO },
    { "image", ICON_IMAGE }, { "log", ICON_LOG }, { "snake", ICON_SNAKE }, { "folder", ICON_FOLDER },
    { "file", ICON_FILE }, { "text", ICON_TEXT }, { "app", ICON_APP }, { "power", ICON_POWER },
    { "restart", ICON_RESTART }, { "search", ICON_SEARCH }, { "network", ICON_NETWORK }, { "home", ICON_HOME },
    { "trash", ICON_TRASH }, { "game", ICON_GAME }, { "plasma", ICON_PLASMA }, { "browser", ICON_BROWSER },
    { "logout", ICON_LOGOUT }, { "disk", ICON_DISK }, { "install", ICON_INSTALL }, { "piano", ICON_PIANO },
    { "sound", ICON_SOUND }, { "chess", ICON_CHESS }, { "mines", ICON_MINES }, { "2048", ICON_2048 },
    { "store", ICON_STORE }, { "user", ICON_USER }, { "lock", ICON_LOCK }, { "ethernet", ICON_ETHERNET },
};

int icon_by_name(const char *name)
{
    for (unsigned i = 0; i < ARRAY_SIZE(icon_names); i++)
        if (!strcmp(icon_names[i].name, name)) return icon_names[i].icon;
    return ICON_NONE;
}
