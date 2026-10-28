/*
 * life: Conway's Game of Life in a window, as a ring-3 program.
 * Click to toggle cells, space to pause, R to randomise, C to clear.
 */
#include <zenith.h>

#define CW 90
#define CH 60
#define CS 7

static uint8_t a[CH][CW], b[CH][CW];

static void randomize(void)
{
    for (int y = 0; y < CH; y++)
        for (int x = 0; x < CW; x++) a[y][x] = (random() % 100) < 28;
}

static void step(void)
{
    for (int y = 0; y < CH; y++)
        for (int x = 0; x < CW; x++) {
            int n = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    if (!dx && !dy) continue;
                    n += a[(y + dy + CH) % CH][(x + dx + CW) % CW] != 0;
                }
            uint8_t alive = a[y][x];
            b[y][x] = alive ? (n == 2 || n == 3 ? (uint8_t)(alive < 250 ? alive + 1 : alive) : 0) : (n == 3);
        }
    memcpy(a, b, sizeof(a));
}

int main(void)
{
    int W = CW * CS, H = CH * CS + 36;
    zwin_t *w = zwin_open(W, H, "Life (user program)");
    if (!w) return 1;
    randomize();
    int paused = 0, gen = 0;
    char label[96];
    for (;;) {
        struct z_event ev;
        while (zwin_event(w, &ev, 0)) {
            if (ev.type == ZEV_CLOSE) { zwin_close(w); return 0; }
            if (ev.type == ZEV_KEY_DOWN) {
                if (ev.ch == ' ') paused = !paused;
                if (ev.ch == 'r' || ev.ch == 'R') { randomize(); gen = 0; }
                if (ev.ch == 'c' || ev.ch == 'C') { memset(a, 0, sizeof(a)); gen = 0; }
            }
            if (ev.type == ZEV_MOUSE_DOWN && ev.y >= 36) {
                int x = ev.x / CS, y = (ev.y - 36) / CS;
                if (x >= 0 && x < CW && y >= 0 && y < CH) a[y][x] = !a[y][x];
            }
        }
        if (!paused) { step(); gen++; }
        zfill(w, 0, 0, W, 36, ZRGB(27, 31, 51));
        snprintf(label, sizeof(label), "generation %d%s   ·   space pause · R random · C clear · click to draw", gen,
                 paused ? " (paused)" : "");
        ztext(w, 12, 10, label, 0xE9EBF8, 13);
        for (int y = 0; y < CH; y++)
            for (int x = 0; x < CW; x++) {
                uint32_t c = ZRGB(16, 18, 32);
                if (a[y][x]) {
                    int age = a[y][x] > 40 ? 40 : a[y][x];
                    c = ZRGB(120 + age * 3, 90 + age * 2, 255 - age * 2);
                }
                zfill(w, x * CS, 36 + y * CS, CS - 1, CS - 1, c);
            }
        zwin_present(w);
        sleep_ms(60);
    }
}
