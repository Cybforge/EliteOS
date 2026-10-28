/*
 * plasma: an animated demo running entirely in ring 3. It renders into its
 * own buffer and hands frames to the compositor with SYS_WIN_PRESENT.
 * Move the mouse over the window to stir the colours.
 */
#include <zenith.h>

#define W 520
#define H 340

static int sintab[1024];

static inline int isin(int a) { return sintab[a & 1023]; }

int main(void)
{
    for (int i = 0; i < 1024; i++) sintab[i] = (int)(sin(i * 2 * M_PI / 1024.0) * 127.0);
    uint32_t pal[256];
    for (int i = 0; i < 256; i++) {
        double t = i / 256.0 * 2 * M_PI;
        int r = (int)(128 + 127 * sin(t));
        int g = (int)(128 + 127 * sin(t + 2.1));
        int b = (int)(128 + 127 * sin(t + 4.2));
        pal[i] = ZRGB(r / 2 + 40, g / 3 + 20, b / 2 + 90);
    }
    zwin_t *w = zwin_open(W, H, "Plasma (user program)");
    if (!w) { printf("could not open a window\n"); return 1; }
    int mx = W / 2, my = H / 2;
    unsigned long t0 = uptime(), fps_t = t0;
    int frames = 0, fps = 0;
    char label[96];
    for (;;) {
        struct z_event ev;
        while (zwin_event(w, &ev, 0)) {
            if (ev.type == ZEV_CLOSE) { zwin_close(w); return 0; }
            if (ev.type == ZEV_MOUSE_MOVE) { mx = ev.x; my = ev.y; }
        }
        int t = (int)((uptime() - t0) / 8);
        for (int y = 0; y < H; y++) {
            uint32_t *row = w->px + y * W;
            int dy = y - my;
            for (int x = 0; x < W; x++) {
                int dx = x - mx;
                int d = (dx * dx + dy * dy) >> 7;
                int v = isin(x * 3 + t) + isin(y * 4 - t * 2) + isin((x + y) * 2 + t) + isin(d * 3 - t * 3);
                row[x] = pal[(v >> 1) & 255];
            }
        }
        frames++;
        if (uptime() - fps_t >= 1000) { fps = frames; frames = 0; fps_t = uptime(); }
        snprintf(label, sizeof(label), "ring 3  ·  pid %d  ·  %d fps", getpid(), fps);
        zfill(w, 12, 12, 250, 30, ZRGB(10, 10, 25));
        ztext(w, 22, 18, label, 0xFFFFFF, 13);
        zwin_present(w);
        sleep_ms(16);
    }
}
