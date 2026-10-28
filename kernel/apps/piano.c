/*
 * Piano: a playable synthesizer. Mouse or computer keyboard (tracker
 * layout: two rows per octave), eight instruments, a live oscilloscope,
 * recording and playback, and a couple of public-domain demo pieces.
 */
#include <wm.h>
#include <audio.h>
#include <input.h>
#include <sched.h>
#include <mm.h>

#define LOW_NOTE   48          /* C3 */
#define NUM_WHITE  22          /* C3 .. C6 */
#define MAX_REC    512
#define BAR_H      64
#define SCOPE_H    150

struct rec_ev { uint32_t t; uint8_t note; uint16_t dur; };

struct piano {
    app_t *app;
    int wave;
    int octave;                        /* shift for the computer keyboard, -1..+1 */
    int held_voice[128];               /* voice per MIDI note, -1 = off */
    uint64_t lit_until[128];           /* highlight for sequenced notes */
    bool key_down[KEY_MAX];
    int mouse_note;
    /* recording */
    bool recording;
    uint64_t rec_start;
    struct rec_ev rec[MAX_REC];
    int nrec;
    uint64_t rec_on[128];
    /* playback */
    volatile bool playing, stop;
    volatile int play_what;            /* 0 recording, 1.. demo */
    bool closed;
};

/* tracker layout: bottom rows = lower octave, top rows = upper octave */
static const struct { int key, semi; } keymap[] = {
    { KEY_Z, 0 }, { KEY_S, 1 }, { KEY_X, 2 }, { KEY_D, 3 }, { KEY_C, 4 }, { KEY_V, 5 }, { KEY_G, 6 },
    { KEY_B, 7 }, { KEY_H, 8 }, { KEY_N, 9 }, { KEY_J, 10 }, { KEY_M, 11 }, { KEY_COMMA, 12 }, { KEY_L, 13 },
    { KEY_DOT, 14 }, { KEY_SEMICOLON, 15 }, { KEY_SLASH, 16 },
    { KEY_Q, 12 }, { KEY_2, 13 }, { KEY_W, 14 }, { KEY_3, 15 }, { KEY_E, 16 }, { KEY_R, 17 }, { KEY_5, 18 },
    { KEY_T, 19 }, { KEY_6, 20 }, { KEY_Y, 21 }, { KEY_7, 22 }, { KEY_U, 23 }, { KEY_I, 24 }, { KEY_9, 25 },
    { KEY_O, 26 }, { KEY_0, 27 }, { KEY_P, 28 },
};

static bool is_black(int midi)
{
    int n = midi % 12;
    return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
}

static void note_on(struct piano *p, int midi, float vel)
{
    if (midi < 0 || midi > 127 || p->held_voice[midi] >= 0) return;
    p->held_voice[midi] = audio_note_on(note_freq(midi), p->wave, vel, -1);
    if (p->recording) p->rec_on[midi] = uptime_ms();
}

static void note_off(struct piano *p, int midi)
{
    if (midi < 0 || midi > 127 || p->held_voice[midi] < 0) return;
    audio_note_off(p->held_voice[midi]);
    p->held_voice[midi] = -1;
    if (p->recording && p->nrec < MAX_REC && p->rec_on[midi]) {
        struct rec_ev *e = &p->rec[p->nrec++];
        e->t = (uint32_t)(p->rec_on[midi] - p->rec_start);
        e->note = (uint8_t)midi;
        e->dur = (uint16_t)MIN(uptime_ms() - p->rec_on[midi], 60000ull);
        p->rec_on[midi] = 0;
    }
}

/* ------------------------------------------------------------------------
 * demo pieces: { midi, start, length } in sixteenth notes
 * ---------------------------------------------------------------------- */

struct seq_note { uint8_t n; uint16_t at, len; };

static const struct seq_note ode[] = {
    /* Beethoven, Ode to Joy (melody in quarters = 4 sixteenths, with a simple bass) */
    { 64, 0, 4 }, { 64, 4, 4 }, { 65, 8, 4 }, { 67, 12, 4 }, { 67, 16, 4 }, { 65, 20, 4 }, { 64, 24, 4 }, { 62, 28, 4 },
    { 60, 32, 4 }, { 60, 36, 4 }, { 62, 40, 4 }, { 64, 44, 4 }, { 64, 48, 6 }, { 62, 54, 2 }, { 62, 56, 8 },
    { 64, 64, 4 }, { 64, 68, 4 }, { 65, 72, 4 }, { 67, 76, 4 }, { 67, 80, 4 }, { 65, 84, 4 }, { 64, 88, 4 }, { 62, 92, 4 },
    { 60, 96, 4 }, { 60, 100, 4 }, { 62, 104, 4 }, { 64, 108, 4 }, { 62, 112, 6 }, { 60, 118, 2 }, { 60, 120, 8 },
    { 48, 0, 16 }, { 43, 16, 16 }, { 48, 32, 16 }, { 43, 48, 16 }, { 48, 64, 16 }, { 43, 80, 16 }, { 48, 96, 8 },
    { 43, 104, 8 }, { 48, 112, 16 }, { 55, 0, 16 }, { 55, 16, 16 }, { 52, 32, 16 }, { 55, 48, 16 }, { 55, 64, 16 },
    { 55, 80, 16 }, { 52, 96, 8 }, { 50, 104, 8 }, { 52, 112, 16 },
};

static const struct seq_note elise[] = {
    /* Beethoven, Für Elise (opening) */
    { 76, 0, 1 }, { 75, 1, 1 }, { 76, 2, 1 }, { 75, 3, 1 }, { 76, 4, 1 }, { 71, 5, 1 }, { 74, 6, 1 }, { 72, 7, 1 },
    { 69, 8, 3 }, { 45, 8, 1 }, { 52, 9, 1 }, { 57, 10, 1 }, { 60, 11, 1 }, { 64, 12, 1 }, { 69, 13, 1 },
    { 71, 14, 3 }, { 40, 14, 1 }, { 52, 15, 1 }, { 56, 16, 1 }, { 64, 17, 1 }, { 68, 18, 1 }, { 71, 19, 1 },
    { 72, 20, 3 }, { 45, 20, 1 }, { 52, 21, 1 }, { 57, 22, 1 }, { 64, 23, 1 },
    { 76, 24, 1 }, { 75, 25, 1 }, { 76, 26, 1 }, { 75, 27, 1 }, { 76, 28, 1 }, { 71, 29, 1 }, { 74, 30, 1 }, { 72, 31, 1 },
    { 69, 32, 3 }, { 45, 32, 1 }, { 52, 33, 1 }, { 57, 34, 1 }, { 60, 35, 1 }, { 64, 36, 1 }, { 69, 37, 1 },
    { 71, 38, 3 }, { 40, 38, 1 }, { 52, 39, 1 }, { 56, 40, 1 }, { 64, 41, 1 }, { 72, 42, 1 }, { 71, 43, 1 },
    { 69, 44, 6 }, { 45, 44, 1 }, { 52, 45, 1 }, { 57, 46, 1 },
};

static const struct seq_note arp[] = {
    /* a little chord progression, C - Am - F - G */
    { 60, 0, 2 }, { 64, 2, 2 }, { 67, 4, 2 }, { 72, 6, 2 }, { 76, 8, 2 }, { 72, 10, 2 }, { 67, 12, 2 }, { 64, 14, 2 },
    { 57, 16, 2 }, { 60, 18, 2 }, { 64, 20, 2 }, { 69, 22, 2 }, { 72, 24, 2 }, { 69, 26, 2 }, { 64, 28, 2 }, { 60, 30, 2 },
    { 53, 32, 2 }, { 57, 34, 2 }, { 60, 36, 2 }, { 65, 38, 2 }, { 69, 40, 2 }, { 65, 42, 2 }, { 60, 44, 2 }, { 57, 46, 2 },
    { 55, 48, 2 }, { 59, 50, 2 }, { 62, 52, 2 }, { 67, 54, 2 }, { 71, 56, 2 }, { 74, 58, 2 }, { 79, 60, 4 },
    { 48, 0, 16 }, { 45, 16, 16 }, { 41, 32, 16 }, { 43, 48, 16 },
};

static const struct { const char *name; const struct seq_note *n; int count; int ms16; int wave; } demos[] = {
    { "Ode to Joy", ode, ARRAY_SIZE(ode), 125, WAVE_PIANO },
    { "Für Elise", elise, ARRAY_SIZE(elise), 140, WAVE_PIANO },
    { "Arpeggio", arp, ARRAY_SIZE(arp), 110, WAVE_BELL },
};

static void repaint(struct piano *p)
{
    if (p->closed) return;
    struct gui_event ev = { 0 };
    ev.type = EV_TIMER;
    wm_post_event(p->app->win, &ev);
}

static int player(void *arg)
{
    struct piano *p = arg;
    uint64_t t0 = uptime_ms();
    int what = p->play_what;
    /* build a flat list: start ms, length ms, note */
    int count = what ? demos[what - 1].count : p->nrec;
    struct rec_ev *evs = kmalloc(sizeof(*evs) * (size_t)MAX(count, 1));
    for (int i = 0; i < count; i++) {
        if (what) {
            const struct seq_note *s = &demos[what - 1].n[i];
            int ms = demos[what - 1].ms16;
            evs[i].t = (uint32_t)(s->at * ms);
            evs[i].dur = (uint16_t)(s->len * ms);
            evs[i].note = s->n;
        } else {
            evs[i] = p->rec[i];
        }
    }
    /* sort by start time (insertion sort, lists are short) */
    for (int i = 1; i < count; i++)
        for (int j = i; j > 0 && evs[j].t < evs[j - 1].t; j--) {
            struct rec_ev t = evs[j]; evs[j] = evs[j - 1]; evs[j - 1] = t;
        }
    int wave = what ? demos[what - 1].wave : p->wave;
    for (int i = 0; i < count && !p->stop; i++) {
        while (!p->stop && uptime_ms() - t0 < evs[i].t) sched_sleep(2);
        if (p->stop) break;
        float vel = evs[i].note < 55 ? 0.35f : 0.5f;
        audio_note_on(note_freq(evs[i].note), wave, vel, MAX(evs[i].dur, 40));
        p->lit_until[evs[i].note] = uptime_ms() + MAX(evs[i].dur, 80);
        repaint(p);
    }
    kfree(evs);
    sched_sleep(300);
    p->playing = false;
    repaint(p);
    return 0;
}

static void start_play(struct piano *p, int what)
{
    if (p->playing) return;
    if (!what && !p->nrec) return;
    p->recording = false;
    p->stop = false;
    p->play_what = what;
    p->playing = true;
    thread_create("piano-play", player, p);
}

/* ------------------------------------------------------------------------
 * drawing
 * ---------------------------------------------------------------------- */

static int key_at(rect_t kb, int mx, int my)
{
    if (!rect_has(kb, mx, my)) return -1;
    int ww = kb.w / NUM_WHITE;
    int bw = ww * 62 / 100, bh = kb.h * 62 / 100;
    /* black keys first: they sit on top */
    int white = 0;
    for (int m = LOW_NOTE; white < NUM_WHITE; m++) {
        if (is_black(m)) {
            int x = kb.x + white * ww - bw / 2;
            if (my < kb.y + bh && mx >= x && mx < x + bw) return m;
        } else {
            white++;
        }
    }
    int w = (mx - kb.x) / ww;
    white = 0;
    for (int m = LOW_NOTE;; m++) {
        if (is_black(m)) continue;
        if (white == w) return m;
        white++;
        if (white >= NUM_WHITE) return -1;
    }
}

static const char *key_hint(struct piano *p, int midi)
{
    /* the label is whatever character that physical key types in the current layout */
    static char buf[4][8];
    static int next;
    int base = 60 + p->octave * 12 - 12;
    for (unsigned i = 0; i < ARRAY_SIZE(keymap); i++) {
        if (base + keymap[i].semi != midi) continue;
        uint32_t cp = keymap_char(keymap[i].key, false);
        if (!cp) return NULL;
        if (cp >= 'a' && cp <= 'z') cp -= 32;
        char *b = buf[next++ & 3];
        b[utf8_encode(cp, b)] = 0;
        return b;
    }
    return NULL;
}

static bool lit(struct piano *p, int m)
{
    return p->held_voice[m] >= 0 || p->lit_until[m] > uptime_ms();
}

static void piano_paint(app_t *a, surface_t *s)
{
    struct piano *p = a->data;
    ui_t *u = &a->ui;
    int W = s->w, H = s->h;
    gfx_clear(s, theme.bg);

    /* toolbar: instruments */
    gfx_fill(s, 0, 0, W, BAR_H, theme.panel);
    gfx_text(s, font_ui_bold, 18, 12, "Instrument", theme.text_faint);
    int x = 18;
    for (int i = 0; i < WAVE_COUNT; i++) {
        int w = font_text_width(font_ui_md, wave_names[i]) + 22;
        rect_t r = R(x, 30, w, 26);
        bool sel = p->wave == i;
        if (ui_hover(u, r) && u->mreleased) p->wave = i;
        gfx_round_rect(s, r.x, r.y, r.w, r.h, 13, sel ? theme.accent : ui_hover(u, r) ? theme.surface2 : theme.surface);
        gfx_text(s, font_ui_md, r.x + 11, r.y + 5, wave_names[i], sel ? 0xFFFFFFFFu : theme.text);
        x += w + 6;
    }
    /* octave */
    int ox = W - 150;
    gfx_text(s, font_ui_bold, ox, 12, "Octave", theme.text_faint);
    if (ui_button(u, R(ox, 30, 30, 26), "-", BTN_NORMAL) && p->octave > -1) p->octave--;
    char ob[8];
    snprintf(ob, sizeof(ob), "%+d", p->octave);
    gfx_text(s, font_ui_bold, ox + 44, 34, ob, theme.text);
    if (ui_button(u, R(ox + 76, 30, 30, 26), "+", BTN_NORMAL) && p->octave < 1) p->octave++;

    /* oscilloscope */
    rect_t sc = R(18, BAR_H + 14, W - 36 - 190, SCOPE_H);
    gfx_round_rect(s, sc.x, sc.y, sc.w, sc.h, 12, HEX(0x0D1020));
    for (int gx = 1; gx < 8; gx++) gfx_fill(s, sc.x + gx * sc.w / 8, sc.y + 8, 1, sc.h - 16, ALPHA(0xFFFFFF, 8));
    gfx_fill(s, sc.x + 8, sc.y + sc.h / 2, sc.w - 16, 1, ALPHA(0xFFFFFF, 16));
    int16_t smp[480];
    audio_scope(smp, 480);
    /* trigger on a rising zero crossing for a stable picture */
    int start = 0;
    for (int i = 1; i < 240; i++) if (smp[i - 1] < 0 && smp[i] >= 0) { start = i; break; }
    int n = MIN(240, sc.w - 20);
    float px = 0, py = 0;
    for (int i = 0; i < n; i++) {
        float xx = (float)sc.x + 10 + (float)i * (float)(sc.w - 20) / (float)n;
        float yy = (float)sc.y + (float)sc.h / 2 - (float)smp[start + i] / 32768.0f * (float)sc.h * 0.9f;
        if (i) {
            gfx_line_w(s, px, py, xx, yy, 5.0f, ALPHA(0x7C5CFF, 40));
            gfx_line_w(s, px, py, xx, yy, 1.8f, theme.accent2);
        }
        px = xx;
        py = yy;
    }
    /* level meter + transport */
    rect_t side = R(sc.x + sc.w + 16, sc.y, 174, SCOPE_H);
    float lvl = MIN(1.0f, audio_level());
    gfx_round_rect(s, side.x, side.y, 14, side.h, 7, HEX(0x0D1020));
    int lh = (int)((float)(side.h - 6) * lvl);
    gfx_round_rect_grad(s, side.x + 3, side.y + side.h - 3 - lh, 8, lh, 4, HEX(0xFF5C7A), HEX(0x35D69A));
    int bx = side.x + 26, bw = side.w - 26;
    const char *rl = p->recording ? "Stop recording" : "Record";
    if (ui_button(u, R(bx, side.y, bw, 32), rl, p->recording ? BTN_DANGER : BTN_NORMAL)) {
        if (p->recording) p->recording = false;
        else if (!p->playing) {
            p->recording = true;
            p->nrec = 0;
            p->rec_start = uptime_ms();
        }
    }
    char pl[32];
    snprintf(pl, sizeof(pl), p->playing ? "Stop" : "Play (%d notes)", p->nrec);
    if (ui_button(u, R(bx, side.y + 38, bw, 32), pl, p->playing ? BTN_DANGER : BTN_PRIMARY)) {
        if (p->playing) p->stop = true;
        else start_play(p, 0);
    }
    for (int i = 0; i < (int)ARRAY_SIZE(demos); i++) {
        rect_t r = R(bx + (i % 2) * (bw / 2 + 2), side.y + 78 + (i / 2) * 36, bw / 2 - 2, 30);
        if (i == 2) r = R(bx, side.y + 78 + 36, bw, 30);
        if (ui_button(u, r, demos[i].name, BTN_SUBTLE)) start_play(p, i + 1);
    }
    if (p->recording) {
        gfx_circle(s, (float)sc.x + 22, (float)sc.y + 20, 5, (uptime_ms() / 400) % 2 ? theme.danger : ALPHA(0xFF5C7A, 80));
        gfx_text(s, font_ui_md, sc.x + 34, sc.y + 12, "REC", theme.danger);
    }
    if (!audio_available())
        gfx_text(s, font_ui_md, sc.x + 16, sc.y + sc.h - 30, "No sound card found - ZenithOS drives Intel HD Audio.", theme.warning);

    /* keyboard */
    rect_t kb = R(18, sc.y + sc.h + 20, W - 36, H - (sc.y + sc.h + 20) - 18);
    int ww = kb.w / NUM_WHITE;
    kb.w = ww * NUM_WHITE;
    int bwid = ww * 62 / 100, bh = kb.h * 62 / 100;
    int white = 0;
    for (int m = LOW_NOTE; white < NUM_WHITE; m++) {
        if (is_black(m)) continue;
        int kx = kb.x + white * ww;
        bool on = lit(p, m);
        gfx_round_rect_grad(s, kx + 1, kb.y, ww - 2, kb.h, 6, on ? color_lighten(theme.accent, 60) : HEX(0xFBFBFE),
                            on ? theme.accent : HEX(0xD9DCE8));
        const char *hint = key_hint(p, m);
        if (hint) gfx_text(s, font_ui, kx + (ww - font_text_width(font_ui, hint)) / 2, kb.y + kb.h - 42, hint,
                           on ? 0xFFFFFFFFu : HEX(0x8A90AA));
        if (m % 12 == 0) {
            char nb[8];
            snprintf(nb, sizeof(nb), "C%d", m / 12 - 1);
            gfx_text(s, font_ui_bold, kx + (ww - font_text_width(font_ui_bold, nb)) / 2, kb.y + kb.h - 22, nb,
                     on ? 0xFFFFFFFFu : HEX(0x5A6078));
        }
        white++;
    }
    white = 0;
    for (int m = LOW_NOTE; white < NUM_WHITE; m++) {
        if (!is_black(m)) { white++; continue; }
        int kx = kb.x + white * ww - bwid / 2;
        bool on = lit(p, m);
        gfx_round_rect_grad(s, kx, kb.y - 2, bwid, bh, 5, on ? color_lighten(theme.accent, 30) : HEX(0x3A3F58),
                            on ? theme.accent : HEX(0x12141F));
        const char *hint = key_hint(p, m);
        if (hint) gfx_text(s, font_ui, kx + (bwid - font_text_width(font_ui, hint)) / 2, kb.y + bh - 26, hint,
                           ALPHA(0xFFFFFF, on ? 255 : 150));
    }
    /* mouse playing */
    int under = u->mdown ? key_at(kb, u->mx, u->my) : -1;
    if (under != p->mouse_note) {
        if (p->mouse_note >= 0) note_off(p, p->mouse_note);
        if (under >= 0) note_on(p, under, 0.55f);
        p->mouse_note = under;
    }
    /* keep animating while something sounds */
    bool active = p->playing || audio_level() > 0.002f;
    for (int m = 0; m < 128 && !active; m++) if (lit(p, m)) active = true;
    if (active) u->want_repaint = true;
}

static int piano_event(app_t *a, struct gui_event *ev)
{
    struct piano *p = a->data;
    if (ev->type == EV_KEY_DOWN || ev->type == EV_KEY_UP) {
        if (ev->mods & (MOD_CTRL | MOD_ALT | MOD_SUPER)) return 0;
        int key = ev->key;
        if (key <= 0 || key >= KEY_MAX) return 0;
        bool down = ev->type == EV_KEY_DOWN;
        if (down && key == KEY_LEFT && p->octave > -1) { p->octave--; return 1; }
        if (down && key == KEY_RIGHT && p->octave < 1) { p->octave++; return 1; }
        for (unsigned i = 0; i < ARRAY_SIZE(keymap); i++) {
            if (keymap[i].key != key) continue;
            int midi = 60 + p->octave * 12 - 12 + keymap[i].semi;
            if (down && !p->key_down[key]) { p->key_down[key] = true; note_on(p, midi, 0.55f); }
            else if (!down) {
                p->key_down[key] = false;
                /* release whatever this key started, even if the octave changed meanwhile */
                for (int o = -1; o <= 1; o++) note_off(p, 60 + o * 12 - 12 + keymap[i].semi);
            }
            return 1;
        }
        return 0;
    }
    return ev->type == EV_TIMER;
}

static void piano_close(app_t *a)
{
    struct piano *p = a->data;
    p->stop = true;
    p->closed = true;
    while (p->playing) sched_sleep(10);
    for (int m = 0; m < 128; m++) note_off(p, m);
    a->quit = true;
}

int piano_main(void *arg)
{
    UNUSED(arg);
    struct piano *p = kzalloc(sizeof(*p));
    for (int i = 0; i < 128; i++) p->held_voice[i] = -1;
    p->mouse_note = -1;
    p->wave = WAVE_PIANO;
    app_t a = { 0 };
    p->app = &a;
    a.data = p;
    a.win = wm_create("Piano", 980, 560, WF_RESIZABLE);
    if (!a.win) { kfree(p); return 1; }
    wm_set_icon(a.win, ICON_PIANO);
    wm_set_min_size(a.win, 760, 480);
    a.on_paint = piano_paint;
    a.on_event = piano_event;
    a.on_close = piano_close;
    int r = app_run(&a);
    kfree(p);
    return r;
}
