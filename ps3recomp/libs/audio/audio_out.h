/*
 * ps3recomp - host audio output stage: ring buffer + SoundTouch time stretch.
 *
 * After RPCS3's cellAudio "audio buffering" + "time stretching": the mixed
 * stereo stream (one 256-frame block every 5.33 ms) is pushed into a ring
 * buffer; the device is fed from it through SoundTouch. While the buffered
 * audio drops below PORT_AUDIO_STRETCH_THRESHOLD % of the target
 * (PORT_AUDIO_BUFFER_MS), playback is slowed down, pitch preserved, so the
 * buffer stretches instead of running dry; the tempo returns to 1.0 as it
 * refills. If it still runs dry, the last few ms that were held back are
 * faded out to silence (no repeated audio, no click), and playback fades back
 * in once the buffer has refilled.
 *
 * Single-threaded: every call comes from the mixer thread. No OS calls.
 */
#ifndef PS3RECOMP_AUDIO_OUT_H
#define PS3RECOMP_AUDIO_OUT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AudioOutConfig {
    unsigned rate;           /* sample rate, Hz (48000) */
    unsigned target_frames;  /* desired buffered audio (PORT_AUDIO_BUFFER_MS) */
    double   threshold;      /* stretch below this fraction of the target (PORT_AUDIO_STRETCH_THRESHOLD/100) */
    double   min_tempo;      /* slowest playback (PORT_AUDIO_STRETCH_MIN) */
    unsigned fade_frames;    /* underrun fade-out / resume fade-in length (PORT_AUDIO_FADE_MS) */
    unsigned guard_frames;   /* conceal when the device holds less than this and nothing is ready */
    unsigned resume_frames;  /* after an underrun, resume once this much is ready */
    unsigned device_frames;  /* most audio to hand to the device at once (PORT_AUDIO_DEVICE_MS) */
    unsigned ring_frames;    /* ring capacity; the oldest audio is dropped when full */
} AudioOutConfig;

typedef struct AudioOutStats {
    unsigned ticks;            /* audio_out_tick calls in the window */
    double   level_min_ms, level_avg_ms;
    double   tempo_min, tempo_avg;
    unsigned stretch_ticks;    /* ticks with tempo < 1 */
    unsigned underruns;        /* times the buffer ran dry (faded to silence) */
    double   silence_ms;       /* silence played while dry */
    unsigned overflow_frames;  /* ring full: oldest frames dropped */
} AudioOutStats;

typedef struct AudioOut AudioOut;

/* Defaults for `buffer_ms` of target buffer at `rate`. */
void audio_out_default_config(AudioOutConfig* cfg, unsigned rate, unsigned buffer_ms);
/* Apply PORT_AUDIO_STRETCH_THRESHOLD, PORT_AUDIO_STRETCH_MIN, PORT_AUDIO_FADE_MS,
 * PORT_AUDIO_DEVICE_MS on top of cfg. `device_period_frames` (the device engine
 * period, 0 = unknown) raises the device cap to at least three periods. */
void audio_out_env_config(AudioOutConfig* cfg, unsigned device_period_frames);

AudioOut* audio_out_create(const AudioOutConfig* cfg);
void      audio_out_destroy(AudioOut* ao);
const AudioOutConfig* audio_out_config(const AudioOut* ao);

/* Producer side: append mixed stereo frames. Returns frames stored. */
unsigned audio_out_push(AudioOut* ao, const float* stereo, unsigned frames);

/* Once per guest block: update the tempo from the buffer level (device_queued
 * = frames the device holds, not yet played) and the statistics. */
void audio_out_tick(AudioOut* ao, unsigned device_queued);

/* Device side: write up to max_frames stereo frames to out; returns how many.
 * May return silence (while dry) and 0 (nothing to do yet). */
unsigned audio_out_render(AudioOut* ao, float* out, unsigned max_frames, unsigned device_queued);

/* Playable audio, frames: device + ring + stretcher output. */
unsigned audio_out_level(const AudioOut* ao, unsigned device_queued);
double   audio_out_tempo(const AudioOut* ao);
int      audio_out_is_dry(const AudioOut* ao);

/* Statistics since the last call; resets the window. */
void audio_out_take_stats(AudioOut* ao, AudioOutStats* st);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_AUDIO_OUT_H */
