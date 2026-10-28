/*
 * Zenith 3D: a software rasteriser (z-buffer, Gouraud shading with
 * specular highlights) that renders each frame in horizontal bands spread
 * over all CPU cores.
 */
#include <wm.h>
#include <mm.h>
#include <cpu.h>
#include <parallel.h>

typedef struct { float x, y, z; } vec3;

static inline float fabsf_(float x) { return x < 0 ? -x : x; }

static inline vec3 v3(float x, float y, float z) { vec3 v = { x, y, z }; return v; }
static inline vec3 vadd(vec3 a, vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline vec3 vsub(vec3 a, vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline vec3 vmul(vec3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline float vdot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline vec3 vcross(vec3 a, vec3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline vec3 vnorm(vec3 a)
{
    float l = (float)k_sqrt((double)vdot(a, a));
    return l > 0 ? vmul(a, 1.0f / l) : a;
}

struct mesh {
    vec3 *pos, *nrm;
    float *tex;         /* colour parameter 0..1 per vertex */
    int nv;
    int *idx;
    int nt;
};

struct sv {             /* screen-space vertex */
    float x, y, z;
    float r, g, b;
    bool visible;
};

struct scene {
    struct mesh meshes[4];
    int cur;
    struct sv *sv;
    int svcap;
    float *zbuf;
    int zw, zh;
    surface_t *target;
    float yaw, pitch, dist, spin;
    bool paused, wire;
    int drag_x, drag_y;
    bool dragging;
    uint64_t last_frame, fps_t0;
    int frames, fps;
    uint64_t render_ms;
    int bands;
    float time;
};

static void mesh_alloc(struct mesh *m, int nv, int nt)
{
    m->pos = kmalloc(sizeof(vec3) * (size_t)nv);
    m->nrm = kmalloc(sizeof(vec3) * (size_t)nv);
    m->tex = kmalloc(sizeof(float) * (size_t)nv);
    m->idx = kmalloc(sizeof(int) * 3 * (size_t)nt);
    m->nv = nv;
    m->nt = nt;
}

/* generic tube around a parametric curve */
static void make_tube(struct mesh *m, int seg, int ring, float radius, int kind)
{
    mesh_alloc(m, seg * ring, seg * ring * 2);
    for (int i = 0; i < seg; i++) {
        float t = (float)i / (float)seg * 2.0f * (float)K_PI;
        vec3 c, d;
        if (kind == 0) {            /* torus */
            c = v3((float)k_cos(t) * 1.15f, (float)k_sin(t) * 1.15f, 0);
            d = v3(-(float)k_sin(t), (float)k_cos(t), 0);
        } else {                    /* (2,3) torus knot */
            float p = 2, q = 3;
            float r = (float)k_cos(q * t) + 2.2f;
            c = v3(r * (float)k_cos(p * t) * 0.52f, r * (float)k_sin(p * t) * 0.52f, -(float)k_sin(q * t) * 0.52f);
            float t2 = t + 0.001f;
            float r2 = (float)k_cos(q * t2) + 2.2f;
            vec3 c2 = v3(r2 * (float)k_cos(p * t2) * 0.52f, r2 * (float)k_sin(p * t2) * 0.52f, -(float)k_sin(q * t2) * 0.52f);
            d = vnorm(vsub(c2, c));
        }
        vec3 up = fabsf_(d.z) > 0.9f ? v3(1, 0, 0) : v3(0, 0, 1);
        vec3 nx = vnorm(vcross(d, up));
        vec3 ny = vcross(d, nx);
        for (int j = 0; j < ring; j++) {
            float a = (float)j / (float)ring * 2.0f * (float)K_PI;
            vec3 n = vadd(vmul(nx, (float)k_cos(a)), vmul(ny, (float)k_sin(a)));
            int k = i * ring + j;
            m->pos[k] = vadd(c, vmul(n, radius));
            m->nrm[k] = n;
            m->tex[k] = (float)i / (float)seg;
        }
    }
    int t = 0;
    for (int i = 0; i < seg; i++)
        for (int j = 0; j < ring; j++) {
            int a = i * ring + j, b = ((i + 1) % seg) * ring + j;
            int c2 = ((i + 1) % seg) * ring + (j + 1) % ring, d2 = i * ring + (j + 1) % ring;
            m->idx[t++] = a; m->idx[t++] = b; m->idx[t++] = c2;
            m->idx[t++] = a; m->idx[t++] = c2; m->idx[t++] = d2;
        }
}

static void make_sphere(struct mesh *m, int st, int sl)
{
    mesh_alloc(m, (st + 1) * (sl + 1), st * sl * 2);
    for (int i = 0; i <= st; i++) {
        float phi = (float)i / (float)st * (float)K_PI;
        for (int j = 0; j <= sl; j++) {
            float th = (float)j / (float)sl * 2.0f * (float)K_PI;
            vec3 n = v3((float)k_sin(phi) * (float)k_cos(th), (float)k_cos(phi), (float)k_sin(phi) * (float)k_sin(th));
            int k = i * (sl + 1) + j;
            /* gentle bumps */
            float bump = 1.25f + 0.08f * (float)k_sin(th * 6) * (float)k_sin(phi * 5);
            m->pos[k] = vmul(n, bump);
            m->nrm[k] = n;
            m->tex[k] = (float)i / (float)st;
        }
    }
    int t = 0;
    for (int i = 0; i < st; i++)
        for (int j = 0; j < sl; j++) {
            int a = i * (sl + 1) + j, b = a + sl + 1;
            m->idx[t++] = a; m->idx[t++] = b; m->idx[t++] = b + 1;
            m->idx[t++] = a; m->idx[t++] = b + 1; m->idx[t++] = a + 1;
        }
}

static void make_cube(struct mesh *m)
{
    /* 6 faces x 4 vertices, subdivided a little so lighting varies */
    const int N = 6;
    mesh_alloc(m, 6 * (N + 1) * (N + 1), 6 * N * N * 2);
    static const vec3 fn[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    int v = 0, t = 0;
    for (int f = 0; f < 6; f++) {
        vec3 n = fn[f];
        vec3 u = fabsf_(n.y) > 0.5f ? v3(1, 0, 0) : v3(0, 1, 0);
        vec3 w = vcross(n, u);
        int base = v;
        for (int i = 0; i <= N; i++)
            for (int j = 0; j <= N; j++) {
                float a = (float)i / N * 2 - 1, b = (float)j / N * 2 - 1;
                m->pos[v] = vadd(vadd(n, vmul(u, a)), vmul(w, b));
                m->nrm[v] = n;
                m->tex[v] = (float)f / 6.0f;
                v++;
            }
        for (int i = 0; i < N; i++)
            for (int j = 0; j < N; j++) {
                int a = base + i * (N + 1) + j, b = a + N + 1;
                m->idx[t++] = a; m->idx[t++] = b + 1; m->idx[t++] = b;
                m->idx[t++] = a; m->idx[t++] = a + 1; m->idx[t++] = b + 1;
            }
    }
}

/* ------------------------------------------------------------------------
 * per-frame work
 * ---------------------------------------------------------------------- */

static void transform(struct scene *sc, int W, int H)
{
    struct mesh *m = &sc->meshes[sc->cur];
    if (sc->svcap < m->nv) {
        kfree(sc->sv);
        sc->sv = kmalloc(sizeof(struct sv) * (size_t)m->nv);
        sc->svcap = m->nv;
    }
    float cy = (float)k_cos(sc->yaw), sy = (float)k_sin(sc->yaw);
    float cp = (float)k_cos(sc->pitch), sp = (float)k_sin(sc->pitch);
    float cs = (float)k_cos(sc->spin), ss = (float)k_sin(sc->spin);
    vec3 light1 = vnorm(v3(-0.5f, 0.7f, -0.8f));
    vec3 light2 = vnorm(v3(0.8f, -0.3f, -0.4f));
    float f = (float)H * 1.1f;
    for (int i = 0; i < m->nv; i++) {
        vec3 p = m->pos[i], n = m->nrm[i];
        /* object spin around z, then camera yaw/pitch */
        vec3 p1 = v3(p.x * cs - p.y * ss, p.x * ss + p.y * cs, p.z);
        vec3 n1 = v3(n.x * cs - n.y * ss, n.x * ss + n.y * cs, n.z);
        vec3 p2 = v3(p1.x * cy + p1.z * sy, p1.y, -p1.x * sy + p1.z * cy);
        vec3 n2 = v3(n1.x * cy + n1.z * sy, n1.y, -n1.x * sy + n1.z * cy);
        vec3 p3 = v3(p2.x, p2.y * cp - p2.z * sp, p2.y * sp + p2.z * cp);
        vec3 n3 = v3(n2.x, n2.y * cp - n2.z * sp, n2.y * sp + n2.z * cp);
        float z = p3.z + sc->dist;
        struct sv *o = &sc->sv[i];
        o->visible = z > 0.2f;
        float inv = 1.0f / MAX(z, 0.2f);
        o->x = (float)W / 2 + p3.x * f * inv;
        o->y = (float)H / 2 - p3.y * f * inv;
        o->z = z;
        /* lighting: two coloured lights + Blinn specular, hue along the mesh */
        vec3 view = v3(0, 0, -1);
        float d1 = MAX(0.0f, vdot(n3, light1));
        float d2 = MAX(0.0f, vdot(n3, light2));
        vec3 h = vnorm(vadd(light1, view));
        float spec = MAX(0.0f, vdot(n3, h));
        spec = spec * spec;
        spec = spec * spec;
        spec = spec * spec;
        spec = spec * spec;
        color_t base = color_hsv(255.0f + m->tex[i] * 140.0f + sc->time * 20.0f, 0.62f, 1.0f);
        float br = (float)C_R(base) / 255.0f, bg = (float)C_G(base) / 255.0f, bb = (float)C_B(base) / 255.0f;
        float rim = 1.0f - fabsf_(n3.z);
        rim = rim * rim * rim;
        o->r = br * (0.10f + 0.85f * d1) + 0.25f * d2 * 0.3f + spec * 0.9f + rim * 0.25f;
        o->g = bg * (0.10f + 0.85f * d1) + 0.25f * d2 * 0.6f + spec * 0.9f + rim * 0.35f;
        o->b = bb * (0.12f + 0.85f * d1) + 0.25f * d2 * 1.0f + spec * 0.9f + rim * 0.55f;
    }
}

struct band_ctx {
    struct scene *sc;
    int W, H;
};

static inline uint32_t pack(float r, float g, float b)
{
    int R_ = (int)(MIN(r, 1.0f) * 255.0f), G_ = (int)(MIN(g, 1.0f) * 255.0f), B_ = (int)(MIN(b, 1.0f) * 255.0f);
    return 0xFF000000u | ((uint32_t)MAX(R_, 0) << 16) | ((uint32_t)MAX(G_, 0) << 8) | (uint32_t)MAX(B_, 0);
}

static void render_band(int band, void *arg)
{
    struct band_ctx *c = arg;
    struct scene *sc = c->sc;
    int W = c->W, H = c->H;
    int y0 = H * band / sc->bands, y1 = H * (band + 1) / sc->bands;
    surface_t *s = sc->target;
    /* background gradient + clear depth */
    for (int y = y0; y < y1; y++) {
        color_t bgc = color_lerp(HEX(0x1A1640), HEX(0x07080F), y * 256 / H);
        memset32(s->px + (size_t)y * s->stride, bgc, (size_t)W);
        float *zr = sc->zbuf + (size_t)y * W;
        for (int x = 0; x < W; x++) zr[x] = 1e30f;
    }
    /* floor grid glow */
    for (int y = MAX(y0, H * 3 / 4); y < y1; y++) {
        int a = (y - H * 3 / 4) * 60 / MAX(1, H / 4);
        uint32_t *line = s->px + (size_t)y * s->stride;
        for (int x = 0; x < W; x++) line[x] = blend(line[x], 0xFF3A2A8Au, (uint32_t)a);
    }
    struct mesh *m = &sc->meshes[sc->cur];
    for (int t = 0; t < m->nt; t++) {
        struct sv *a = &sc->sv[m->idx[t * 3]], *b = &sc->sv[m->idx[t * 3 + 1]], *d = &sc->sv[m->idx[t * 3 + 2]];
        if (!a->visible || !b->visible || !d->visible) continue;
        float area = (b->x - a->x) * (d->y - a->y) - (d->x - a->x) * (b->y - a->y);
        if (area >= 0) continue;                      /* back face */
        float miny = MIN(a->y, MIN(b->y, d->y)), maxy = MAX(a->y, MAX(b->y, d->y));
        if (maxy < (float)y0 || miny >= (float)y1) continue;
        float minx = MIN(a->x, MIN(b->x, d->x)), maxx = MAX(a->x, MAX(b->x, d->x));
        int ix0 = MAX(0, (int)minx), ix1 = MIN(W - 1, (int)maxx + 1);
        int iy0 = MAX(y0, (int)miny), iy1 = MIN(y1 - 1, (int)maxy + 1);
        float inv = 1.0f / area;
        for (int y = iy0; y <= iy1; y++) {
            float py = (float)y + 0.5f;
            uint32_t *line = s->px + (size_t)y * s->stride;
            float *zr = sc->zbuf + (size_t)y * W;
            for (int x = ix0; x <= ix1; x++) {
                float px = (float)x + 0.5f;
                float w0 = ((b->x - px) * (d->y - py) - (d->x - px) * (b->y - py)) * inv;
                float w1 = ((d->x - px) * (a->y - py) - (a->x - px) * (d->y - py)) * inv;
                float w2 = 1.0f - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                float z = w0 * a->z + w1 * b->z + w2 * d->z;
                if (z >= zr[x]) continue;
                zr[x] = z;
                line[x] = pack(w0 * a->r + w1 * b->r + w2 * d->r, w0 * a->g + w1 * b->g + w2 * d->g,
                               w0 * a->b + w1 * b->b + w2 * d->b);
            }
        }
    }
}

static void d3_paint(app_t *a, surface_t *s)
{
    struct scene *sc = a->data;
    int W = s->w, H = s->h;
    if (sc->zw != W || sc->zh != H) {
        kfree(sc->zbuf);
        sc->zbuf = kmalloc(sizeof(float) * (size_t)W * H);
        sc->zw = W;
        sc->zh = H;
    }
    uint64_t t0 = uptime_ms();
    transform(sc, W, H);
    sc->target = s;
    sc->bands = MAX(4, ncpus * 3);
    struct band_ctx c = { sc, W, H };
    parallel_for(sc->bands, render_band, &c);
    if (sc->wire) {
        struct mesh *m = &sc->meshes[sc->cur];
        for (int t = 0; t < m->nt; t += 2) {
            struct sv *p = &sc->sv[m->idx[t * 3]], *q = &sc->sv[m->idx[t * 3 + 1]];
            gfx_line(s, p->x, p->y, q->x, q->y, ALPHA(0xFFFFFF, 60));
        }
    }
    sc->render_ms = uptime_ms() - t0;

    /* HUD */
    struct mesh *m = &sc->meshes[sc->cur];
    static const char *names[] = { "Torus knot", "Torus", "Sphere", "Cube" };
    gfx_round_rect(s, 14, 14, 250, 92, 12, ALPHA(0x000000, 110));
    char l[96];
    gfx_text(s, font_ui_bold, 28, 22, names[sc->cur], 0xFFFFFFFFu);
    snprintf(l, sizeof(l), "%d fps  ·  %lu ms/frame", sc->fps, sc->render_ms);
    gfx_text(s, font_ui, 28, 44, l, HEX(0xC8CCF0));
    snprintf(l, sizeof(l), "%d triangles on %d cores", m->nt, parallel_workers());
    gfx_text(s, font_ui, 28, 62, l, HEX(0xC8CCF0));
    snprintf(l, sizeof(l), "%dx%d  ·  z-buffer  ·  Gouraud", W, H);
    gfx_text(s, font_ui, 28, 80, l, HEX(0x8A90B8));
    gfx_text_right(s, font_ui, W - 16, H - 26, "drag: rotate  ·  wheel: zoom  ·  1-4: model  ·  W: wireframe  ·  space: pause",
                   ALPHA(0xFFFFFF, 150));

    sc->frames++;
    uint64_t now = uptime_ms();
    if (now - sc->fps_t0 >= 1000) {
        sc->fps = sc->frames * 1000 / (int)(now - sc->fps_t0);
        sc->frames = 0;
        sc->fps_t0 = now;
    }
}

static int d3_event(app_t *a, struct gui_event *ev)
{
    struct scene *sc = a->data;
    switch (ev->type) {
    case EV_TIMER: {
        uint64_t now = uptime_ms();
        float dt = (float)(now - sc->last_frame) / 1000.0f;
        sc->last_frame = now;
        if (dt > 0.1f) dt = 0.1f;
        if (!sc->paused) {
            sc->spin += dt * 0.9f;
            if (!sc->dragging) sc->yaw += dt * 0.35f;
            sc->time += dt;
        }
        return 1;
    }
    case EV_MOUSE_DOWN:
        sc->dragging = true;
        sc->drag_x = ev->x;
        sc->drag_y = ev->y;
        return 0;
    case EV_MOUSE_UP:
        sc->dragging = false;
        return 0;
    case EV_MOUSE_MOVE:
        if (sc->dragging) {
            sc->yaw += (float)(ev->x - sc->drag_x) * 0.01f;
            sc->pitch = CLAMP(sc->pitch + (float)(ev->y - sc->drag_y) * 0.01f, -1.4f, 1.4f);
            sc->drag_x = ev->x;
            sc->drag_y = ev->y;
        }
        return 0;
    case EV_MOUSE_WHEEL:
        sc->dist = CLAMP(sc->dist - (float)ev->wheel * 0.3f, 2.5f, 12.0f);
        return 0;
    case EV_KEY_DOWN:
        if (ev->key >= KEY_1 && ev->key <= KEY_4) sc->cur = ev->key - KEY_1;
        if (ev->key == KEY_W) sc->wire = !sc->wire;
        if (ev->key == KEY_SPACE) sc->paused = !sc->paused;
        return 0;
    }
    return 0;
}

int demo3d_main(void *arg)
{
    UNUSED(arg);
    struct scene *sc = kzalloc(sizeof(*sc));
    make_tube(&sc->meshes[0], 160, 18, 0.30f, 1);
    make_tube(&sc->meshes[1], 72, 32, 0.45f, 0);
    make_sphere(&sc->meshes[2], 40, 60);
    make_cube(&sc->meshes[3]);
    sc->dist = 5.2f;
    sc->pitch = 0.35f;
    sc->last_frame = uptime_ms();
    sc->fps_t0 = uptime_ms();
    app_t a = { 0 };
    a.data = sc;
    a.win = wm_create("Zenith 3D", 760, 500, WF_RESIZABLE);
    if (!a.win) return 1;
    wm_set_icon(a.win, ICON_CUBE);
    wm_set_timer(a.win, 16);
    a.on_paint = d3_paint;
    a.on_event = d3_event;
    int r = app_run(&a);
    for (int i = 0; i < 4; i++) {
        kfree(sc->meshes[i].pos); kfree(sc->meshes[i].nrm); kfree(sc->meshes[i].tex); kfree(sc->meshes[i].idx);
    }
    kfree(sc->sv);
    kfree(sc->zbuf);
    kfree(sc);
    return r;
}
