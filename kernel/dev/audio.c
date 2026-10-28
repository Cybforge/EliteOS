/*
 * Audio mixer and synthesizer.
 *
 * A mixer thread keeps the HDA ring buffer ~40 ms ahead of the DMA engine.
 * It mixes up to 32 voices: PCM clips (WAV files, 48 kHz stereo after
 * conversion) and synthesized notes with per-instrument envelopes.
 */
#include <audio.h>
#include <sched.h>
#include <mm.h>
#include <spinlock.h>
#include <dev.h>
#include <vfs.h>

#define NVOICES 32
#define SINE_N 4096
#define TWO_PI 6.28318530718f

enum { V_FREE, V_PCM, V_SYNTH };

struct voice {
    volatile int kind;
    int gen;
    /* pcm */
    int16_t *pcm;
    size_t frames, pos;
    bool owned;
    /* synth */
    float freq, phase, lp;
    int wave;
    uint32_t age, delay;
    int32_t dur;                 /* samples until release, -1 = held */
    volatile bool release_req;   /* set by note_off */
    bool released;
    uint32_t rel_count;
    float env, rel_level;
    float e, ek;                 /* decay state and per-sample factor */
    float h[4], hk[4];           /* harmonic / modulation decays */
    float vol;
};

const char *wave_names[WAVE_COUNT] = { "Sine", "Square", "Saw", "Triangle", "Piano", "Bell", "Organ", "Pluck" };

struct instr { float attack_ms, decay_ms, sustain, release_ms; };
static const struct instr instr[WAVE_COUNT] = {
    [WAVE_SINE]     = { 6, 300, 0.85f, 90 },
    [WAVE_SQUARE]   = { 4, 250, 0.60f, 70 },
    [WAVE_SAW]      = { 4, 250, 0.65f, 70 },
    [WAVE_TRIANGLE] = { 5, 300, 0.85f, 90 },
    [WAVE_PIANO]    = { 2, 900, 0.00f, 160 },
    [WAVE_BELL]     = { 1, 1600, 0.00f, 400 },
    [WAVE_ORGAN]    = { 12, 100, 1.00f, 60 },
    [WAVE_PLUCK]    = { 1, 380, 0.00f, 80 },
};

static struct voice voices[NVOICES];
static spinlock_t vlock = SPINLOCK_INIT("audio");
static float sine_tab[SINE_N];
static int volume = 70;
static bool muted;
static bool ready;
static float peak;
static int16_t scope[1024];
static unsigned scope_pos;

float note_freq(int midi) { return 440.0f * (float)k_pow(2.0, (midi - 69) / 12.0); }

static inline float fsin(float ph)
{
    /* ph in cycles */
    ph -= (float)(int)ph;
    if (ph < 0) ph += 1.0f;
    return sine_tab[(int)(ph * SINE_N) & (SINE_N - 1)];
}

/* ------------------------------------------------------------------------
 * voices
 * ---------------------------------------------------------------------- */

static int alloc_voice(void)
{
    spin_lock(&vlock);
    int best = -1;
    uint32_t oldest = 0;
    for (int i = 0; i < NVOICES; i++) {
        if (voices[i].kind == V_FREE) { best = i; break; }
        /* steal the oldest released synth note */
        if (voices[i].kind == V_SYNTH && voices[i].released && voices[i].age >= oldest) {
            oldest = voices[i].age;
            best = i;
        }
    }
    if (best >= 0) {
        struct voice *v = &voices[best];
        v->kind = V_FREE;
        v->gen++;
    }
    spin_unlock(&vlock);
    return best;
}

static int schedule_note(float freq, int wave, float vol, int ms, int delay_ms)
{
    if (!ready) return -1;
    int i = alloc_voice();
    if (i < 0) return -1;
    struct voice *v = &voices[i];
    v->freq = freq;
    v->phase = v->lp = 0;
    v->wave = CLAMP(wave, 0, WAVE_COUNT - 1);
    v->age = 0;
    v->delay = (uint32_t)delay_ms * (AUDIO_RATE / 1000);
    v->dur = ms < 0 ? -1 : ms * (AUDIO_RATE / 1000);
    v->release_req = false;
    v->released = false;
    v->rel_count = 0;
    v->env = 0;
    v->vol = vol;
    /* per-sample decay factors, so the mixer never calls exp() */
    const float dt = 1.0f / AUDIO_RATE;
    v->e = 1.0f;
    v->ek = (float)k_exp(-2.3 * 1000.0 / (instr[v->wave].decay_ms * AUDIO_RATE));
    static const float piano_rates[4] = { 3, 5, 8, 12 };
    for (int k = 0; k < 4; k++) {
        v->h[k] = 1.0f;
        v->hk[k] = (float)k_exp(-piano_rates[k] * dt);
    }
    if (v->wave == WAVE_BELL) v->hk[0] = (float)k_exp(-2.5 * dt);
    if (v->wave == WAVE_PLUCK) v->hk[0] = (float)k_exp(-9.0 * dt);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    v->kind = V_SYNTH;
    return i | (v->gen << 8);
}

int audio_note_on(float freq, int wave, float vol, int ms) { return schedule_note(freq, wave, vol, ms, 0); }

void audio_note_off(int h)
{
    if (h < 0) return;
    struct voice *v = &voices[h & 0xFF];
    if (v->gen != (h >> 8) || v->kind != V_SYNTH) return;
    v->release_req = true;
}

int audio_play_pcm(int16_t *stereo, size_t frames, float vol, bool free_after)
{
    if (!ready) { if (free_after) kfree(stereo); return -1; }
    int i = alloc_voice();
    if (i < 0) { if (free_after) kfree(stereo); return -1; }
    struct voice *v = &voices[i];
    v->pcm = stereo;
    v->frames = frames;
    v->pos = 0;
    v->owned = free_after;
    v->vol = vol;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    v->kind = V_PCM;
    return i;
}

void audio_stop_all(void)
{
    for (int i = 0; i < NVOICES; i++)
        if (voices[i].kind == V_SYNTH) voices[i].release_req = true;
        else if (voices[i].kind == V_PCM) voices[i].pos = voices[i].frames;
}

/* ------------------------------------------------------------------------
 * instruments
 * ---------------------------------------------------------------------- */



static float envelope(struct voice *v)
{
    const struct instr *in = &instr[v->wave];
    float attack = in->attack_ms * (AUDIO_RATE / 1000.0f);
    float level;
    if ((float)v->age < attack) level = (float)v->age / attack;
    else {
        /* exponential decay towards the sustain level */
        level = in->sustain + (1.0f - in->sustain) * v->e;
        v->e *= v->ek;
    }
    if (v->release_req && !v->released) {
        v->released = true;
        v->rel_count = 0;
        v->rel_level = v->env;
    }
    if (v->released) {
        float k = 1.0f - (float)v->rel_count++ / (in->release_ms * (AUDIO_RATE / 1000.0f));
        if (k <= 0) return -1;                 /* done */
        return v->rel_level * k * k;
    }
    return level;
}

static float synth(struct voice *v)
{
    float ph = v->phase;
    float s;
    switch (v->wave) {
    case WAVE_SQUARE: s = (ph - (float)(int)ph) < 0.5f ? 0.55f : -0.55f; break;
    case WAVE_SAW: s = ((ph - (float)(int)ph) * 2.0f - 1.0f) * 0.6f; break;
    case WAVE_TRIANGLE: {
        float x = ph - (float)(int)ph;
        s = (x < 0.5f ? 4.0f * x - 1.0f : 3.0f - 4.0f * x) * 0.9f;
        break;
    }
    case WAVE_PIANO:
        s = fsin(ph) * 0.62f + fsin(ph * 2) * 0.26f * v->h[0] + fsin(ph * 3) * 0.14f * v->h[1] +
            fsin(ph * 4) * 0.07f * v->h[2] + fsin(ph * 5.02f) * 0.04f * v->h[3];
        for (int k = 0; k < 4; k++) v->h[k] *= v->hk[k];
        break;
    case WAVE_BELL: {
        float index = 2.2f * v->h[0];
        v->h[0] *= v->hk[0];
        s = fsin(ph + index * fsin(ph * 3.5f) * 0.16f) * 0.8f;
        break;
    }
    case WAVE_ORGAN:
        s = fsin(ph) * 0.45f + fsin(ph * 2) * 0.28f + fsin(ph * 4) * 0.16f + fsin(ph * 8) * 0.07f;
        break;
    case WAVE_PLUCK: {
        float x = ph - (float)(int)ph;
        float raw = x * 2.0f - 1.0f;
        float cut = 0.05f + 0.6f * v->h[0];                   /* the filter closes as the note fades */
        v->h[0] *= v->hk[0];
        v->lp += (raw - v->lp) * cut;
        s = v->lp * 0.9f;
        break;
    }
    default: s = fsin(ph) * 0.8f; break;
    }
    v->phase = ph + v->freq * (1.0f / AUDIO_RATE);
    if (v->phase > 1024.0f) v->phase -= 1024.0f;
    return s;
}

/* ------------------------------------------------------------------------
 * mixing
 * ---------------------------------------------------------------------- */

static void mix(int16_t *out, uint32_t frames)
{
    float master = muted ? 0 : (float)volume / 100.0f;
    master *= master;                                   /* perceptual curve */
    float pk = 0;
    for (uint32_t f = 0; f < frames; f++) {
        float l = 0, r = 0;
        for (int i = 0; i < NVOICES; i++) {
            struct voice *v = &voices[i];
            int kind = v->kind;
            if (kind == V_SYNTH) {
                if (v->delay) { v->delay--; continue; }
                if (v->dur >= 0 && v->age >= (uint32_t)v->dur) v->release_req = true;
                float e = envelope(v);
                if (e < 0) { v->kind = V_FREE; continue; }
                v->env = e;
                float s = synth(v) * e * v->vol;
                l += s;
                r += s;
                v->age++;
            } else if (kind == V_PCM) {
                if (v->pos >= v->frames) {
                    v->kind = V_FREE;
                    if (v->owned) kfree(v->pcm);
                    continue;
                }
                l += (float)v->pcm[v->pos * 2] * (1.0f / 32768.0f) * v->vol;
                r += (float)v->pcm[v->pos * 2 + 1] * (1.0f / 32768.0f) * v->vol;
                v->pos++;
            }
        }
        l *= master * 0.5f;
        r *= master * 0.5f;
        /* soft clipper */
        if (l > 1.5f) l = 1.5f; else if (l < -1.5f) l = -1.5f;
        if (r > 1.5f) r = 1.5f; else if (r < -1.5f) r = -1.5f;
        l = l - l * l * l * (4.0f / 27.0f);
        r = r - r * r * r * (4.0f / 27.0f);
        float a = l < 0 ? -l : l;
        if (a > pk) pk = a;
        int16_t sl = (int16_t)(l * 32000.0f), sr = (int16_t)(r * 32000.0f);
        out[f * 2] = sl;
        out[f * 2 + 1] = sr;
        scope[scope_pos++ & 1023] = sl;
    }
    peak = MAX(pk, peak * 0.9f);
}

static int mixer_thread(void *arg)
{
    UNUSED(arg);
    uint32_t n;
    int16_t *ring = hda_buffer(&n);
    uint32_t wpos = (hda_play_pos() + 1024) % n;
    const uint32_t target = 2048;                        /* ~43 ms ahead of the DMA engine */
    for (;;) {
        uint32_t play = hda_play_pos();
        uint32_t ahead = (wpos - play + n) % n;
        if (ahead > n / 2) {                             /* fell behind: jump ahead of the DMA */
            wpos = (play + 1024) % n;
            ahead = 1024;
        }
        while (ahead < target) {
            uint32_t chunk = MIN(MIN(256u, target - ahead), n - wpos);
            mix(ring + wpos * 2, chunk);
            wpos = (wpos + chunk) % n;
            ahead += chunk;
        }
        sched_sleep(4);
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * sound effects
 * ---------------------------------------------------------------------- */

void audio_sound(int snd)
{
    if (!ready) return;
    switch (snd) {
    case SND_STARTUP: {
        static const int notes[] = { 60, 64, 67, 72, 76 };
        for (int i = 0; i < 5; i++) schedule_note(note_freq(notes[i]), WAVE_BELL, 0.34f, 900, i * 110);
        schedule_note(note_freq(48), WAVE_ORGAN, 0.18f, 700, 0);
        schedule_note(note_freq(55), WAVE_ORGAN, 0.14f, 700, 0);
        break;
    }
    case SND_SHUTDOWN: {
        static const int notes[] = { 76, 72, 67, 60 };
        for (int i = 0; i < 4; i++) schedule_note(note_freq(notes[i]), WAVE_BELL, 0.3f, 600, i * 120);
        break;
    }
    case SND_NOTIFY:
        schedule_note(note_freq(81), WAVE_BELL, 0.28f, 300, 0);
        schedule_note(note_freq(88), WAVE_BELL, 0.24f, 500, 90);
        break;
    case SND_SUCCESS:
        schedule_note(note_freq(72), WAVE_PLUCK, 0.4f, 200, 0);
        schedule_note(note_freq(79), WAVE_PLUCK, 0.4f, 300, 80);
        break;
    case SND_ERROR:
        schedule_note(note_freq(45), WAVE_SQUARE, 0.22f, 120, 0);
        schedule_note(note_freq(41), WAVE_SQUARE, 0.22f, 220, 140);
        break;
    case SND_CLICK:
        schedule_note(note_freq(96), WAVE_TRIANGLE, 0.15f, 12, 0);
        break;
    case SND_POP:
        schedule_note(note_freq(84), WAVE_PLUCK, 0.3f, 60, 0);
        break;
    }
}

/* ------------------------------------------------------------------------
 * WAV files
 * ---------------------------------------------------------------------- */

int audio_play_wav(const void *data, size_t size)
{
    const uint8_t *p = data;
    if (size < 44 || memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4)) return E_INVAL;
    int fmt = 0, ch = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t *pcm = NULL;
    size_t pcm_len = 0;
    for (size_t off = 12; off + 8 <= size;) {
        uint32_t len = *(const uint32_t *)(p + off + 4);
        if (!memcmp(p + off, "fmt ", 4) && len >= 16) {
            fmt = *(const uint16_t *)(p + off + 8);
            ch = *(const uint16_t *)(p + off + 10);
            rate = *(const uint32_t *)(p + off + 12);
            bits = *(const uint16_t *)(p + off + 22);
        } else if (!memcmp(p + off, "data", 4)) {
            pcm = p + off + 8;
            pcm_len = MIN((size_t)len, size - off - 8);
        }
        off += 8 + len + (len & 1);
    }
    if (fmt != 1 || !pcm || (ch != 1 && ch != 2) || (bits != 8 && bits != 16) || rate < 4000 || rate > 192000)
        return E_INVAL;
    size_t in_frames = pcm_len / (size_t)(ch * bits / 8);
    size_t out_frames = (size_t)((uint64_t)in_frames * AUDIO_RATE / rate);
    int16_t *out = kmalloc(out_frames * 4 + 4);
    if (!out) return E_NOMEM;
    for (size_t i = 0; i < out_frames; i++) {
        /* linear interpolation between input frames */
        uint64_t pos = (uint64_t)i * rate * 256 / AUDIO_RATE;
        size_t a = (size_t)(pos >> 8), b = MIN(a + 1, in_frames - 1);
        int frac = (int)(pos & 255);
        for (int c = 0; c < 2; c++) {
            int cc = ch == 2 ? c : 0;
            int sa, sb;
            if (bits == 16) {
                sa = ((const int16_t *)pcm)[a * ch + cc];
                sb = ((const int16_t *)pcm)[b * ch + cc];
            } else {
                sa = (pcm[a * ch + cc] - 128) << 8;
                sb = (pcm[b * ch + cc] - 128) << 8;
            }
            out[i * 2 + c] = (int16_t)(sa + ((sb - sa) * frac >> 8));
        }
    }
    return audio_play_pcm(out, out_frames, 1.0f, true) < 0 ? E_NOMEM : 0;
}

/* ------------------------------------------------------------------------ */

void audio_set_volume(int pct) { volume = CLAMP(pct, 0, 100); }
int audio_volume(void) { return volume; }
void audio_set_muted(bool m) { muted = m; }
bool audio_muted(void) { return muted; }
bool audio_available(void) { return ready; }
float audio_level(void) { return peak; }

void audio_scope(int16_t *out, int n)
{
    unsigned p = scope_pos;
    for (int i = 0; i < n; i++) out[i] = scope[(p - (unsigned)n + (unsigned)i) & 1023];
}

void audio_init(void)
{
    for (int i = 0; i < SINE_N; i++) sine_tab[i] = (float)k_sin(TWO_PI * (float)i / SINE_N);
    if (!hda_init()) return;
    ready = true;
    thread_create_ex("mixer", mixer_thread, NULL, 1, -1, NULL, 32 * 1024);
}
