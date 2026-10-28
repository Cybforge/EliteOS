/*
 * Boot splash drawn straight into the framebuffer, before the compositor
 * exists: logo, status line, progress bar. Also used to draw the logo
 * elsewhere (about box, start menu) via logo_draw().
 */
#include <kernel.h>
#include <dev.h>
#include <gfx.h>
#include <mm.h>

static struct boot_framebuffer bfb;
static surface_t fbs;
static bool active;
static bool swap_rb;
static int progress;

static color_t fix(color_t c)
{
    if (!swap_rb) return c;
    return (c & 0xFF00FF00u) | ((c >> 16) & 0xFF) | ((c & 0xFF) << 16);
}

/* The ZenithOS mark: a rounded square with a gradient and a stylised "Z"
 * built from three strokes, plus a small star at the zenith. */
void logo_draw(surface_t *s, int x, int y, int size, bool swap)
{
    color_t c1 = HEX(0x7C5CFF), c2 = HEX(0x00D4FF);
    if (swap) {
        c1 = (c1 & 0xFF00FF00u) | ((c1 >> 16) & 0xFF) | ((c1 & 0xFF) << 16);
        c2 = (c2 & 0xFF00FF00u) | ((c2 >> 16) & 0xFF) | ((c2 & 0xFF) << 16);
    }
    int r = size / 4;
    gfx_round_rect_grad(s, x, y, size, size, r, c1, c2);
    float m = (float)size * 0.27f, w = (float)size * 0.10f;
    float x0 = (float)x + m, x1 = (float)(x + size) - m;
    float y0 = (float)y + m * 1.05f, y1 = (float)(y + size) - m * 1.05f;
    color_t white = 0xFFFFFFFFu;
    gfx_line_w(s, x0, y0, x1, y0, w, white);
    gfx_line_w(s, x1, y0, x0, y1, w, white);
    gfx_line_w(s, x0, y1, x1, y1, w, white);
    gfx_circle(s, (float)x + (float)size * 0.78f, (float)y + (float)size * 0.20f, (float)size * 0.055f, 0xE6FFFFFFu);
}

static void draw_status(const char *msg)
{
    int w = bfb.width, h = bfb.height;
    int bw = 240, bx = (w - bw) / 2, by = h / 2 + 110;
    /* progress bar */
    gfx_fill(&fbs, bx - 2, by - 2, bw + 4, 10, fix(HEX(0x0B0E1A)));
    gfx_round_rect(&fbs, bx, by, bw, 4, 2, fix(HEX(0x23283D)));
    int pw = bw * progress / 100;
    if (pw > 0) gfx_round_rect_grad(&fbs, bx, by, MAX(pw, 4), 4, 2, fix(HEX(0x7C5CFF)), fix(HEX(0x00D4FF)));
    /* status text */
    gfx_fill(&fbs, 0, by + 16, w, 22, fix(HEX(0x0B0E1A)));
    if (msg) {
        rect_t tr = R(0, by + 16, w, 20);
        gfx_text_center(&fbs, font_ui, tr, msg, fix(HEX(0x8A90B0)));
    }
}

void bootcon_init(struct boot_framebuffer *fb)
{
    bfb = *fb;
    swap_rb = fb->format == FB_FORMAT_RGBX;
    fbs = surface_wrap((uint32_t *)P2V(fb->phys), (int)fb->width, (int)fb->height, (int)(fb->pitch / 4));
    int w = (int)fb->width, h = (int)fb->height;
    gfx_fill(&fbs, 0, 0, w, h, fix(HEX(0x0B0E1A)));
    int ls = 96;
    logo_draw(&fbs, (w - ls) / 2, h / 2 - 110, ls, swap_rb);
    gfx_text_center(&fbs, font_light, R(0, h / 2 + 2, w, 40), "ZenithOS", fix(HEX(0xEEF0FF)));
    active = true;
    draw_status("Starting");
}

void bootcon_status(const char *msg, int pct)
{
    if (!active) return;
    progress = CLAMP(pct, 0, 100);
    draw_status(msg);
}

void bootcon_enable(bool on) { active = on; }

void bootcon_write(const char *s, size_t n)
{
    /* Boot messages go to the serial port and the log only; the splash
     * shows a single status line instead of scrolling text. */
    UNUSED(s);
    UNUSED(n);
}

/* raw access for the panic screen */
surface_t *bootcon_surface(bool *swap)
{
    if (!fbs.px) return NULL;
    *swap = swap_rb;
    return &fbs;
}
