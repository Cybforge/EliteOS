#ifndef ZENITH_AUDIO_H
#define ZENITH_AUDIO_H

#include <kernel.h>

/*
 * Sound: an Intel HD Audio driver (dev/hda.c) feeding a software mixer
 * (dev/audio.c). Everything runs at 48 kHz, 16-bit stereo.
 */
#define AUDIO_RATE 48000

/* hda.c - the hardware side */
bool hda_init(void);
bool hda_present(void);
const char *hda_name(void);
uint32_t hda_play_pos(void);            /* frame the DMA engine is playing */
int16_t *hda_buffer(uint32_t *frames);  /* the ring buffer the mixer fills */

/* audio.c - mixer, synthesizer, sound effects */
enum { WAVE_SINE, WAVE_SQUARE, WAVE_SAW, WAVE_TRIANGLE, WAVE_PIANO, WAVE_BELL, WAVE_ORGAN, WAVE_PLUCK, WAVE_COUNT };
extern const char *wave_names[WAVE_COUNT];

enum { SND_STARTUP, SND_NOTIFY, SND_ERROR, SND_CLICK, SND_SUCCESS, SND_SHUTDOWN, SND_POP };

void audio_init(void);
bool audio_available(void);
void audio_set_volume(int pct);          /* 0..100 */
int  audio_volume(void);
void audio_set_muted(bool m);
bool audio_muted(void);
int  audio_note_on(float freq, int wave, float vol, int ms);   /* ms < 0: until audio_note_off */
void audio_note_off(int voice);
void audio_sound(int snd);
int  audio_play_pcm(int16_t *stereo, size_t frames, float vol, bool free_after);
int  audio_play_wav(const void *data, size_t size);            /* 0 or error */
void audio_stop_all(void);
float audio_level(void);                 /* recent peak, 0..1, for meters */
void audio_scope(int16_t *out, int n);   /* the last n mixed samples (left channel) */
float note_freq(int midi);

#endif
