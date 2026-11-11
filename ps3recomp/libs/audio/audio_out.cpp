/*
 * ps3recomp - host audio output stage (see audio_out.h).
 */
#include "audio_out.h"

#include <SoundTouch.h>

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>

struct AudioOut {
    AudioOutConfig cfg;
    soundtouch::SoundTouch st;
    std::vector<float> ring;      /* stereo interleaved, cfg.ring_frames */
    unsigned head = 0, count = 0; /* read position, frames held */
    double tempo = 1.0, applied = 1.0;
    bool dry = true;              /* not playing: priming or after an underrun */
    bool primed = false;          /* played at least once */
    unsigned fadein = 0;          /* frames of the resume fade-in done */
    unsigned latency = 0;         /* average audio held inside SoundTouch, frames */

    unsigned s_ticks = 0, s_stretch = 0, s_underruns = 0, s_overflow = 0;
    double s_lmin = 1e30, s_lsum = 0.0, s_tmin = 1.0, s_tsum = 0.0;
    unsigned long long s_silence = 0;
};

static unsigned env_uint(const char* name, unsigned def, unsigned lo, unsigned hi)
{
    const char* e = getenv(name);
    if (!e || !*e) return def;
    const long v = strtol(e, NULL, 10);
    if (v < (long)lo) return lo;
    if (v > (long)hi) return hi;
    return (unsigned)v;
}

extern "C" void audio_out_default_config(AudioOutConfig* c, unsigned rate, unsigned buffer_ms)
{
    memset(c, 0, sizeof(*c));
    c->rate = rate ? rate : 48000;
    const unsigned ms = rate / 1000;
    c->target_frames = buffer_ms * ms;
    c->threshold = 0.75;              /* RPCS3's default time_stretching_threshold */
    c->min_tempo = 0.5;
    c->fade_frames = 5 * ms;
    c->guard_frames = 15 * ms;
    c->device_frames = 30 * ms;
    if (c->device_frames > c->target_frames) c->device_frames = c->target_frames;
    c->resume_frames = c->target_frames / 4;
    if (c->resume_frames < c->guard_frames + c->fade_frames) c->resume_frames = c->guard_frames + c->fade_frames;
    c->ring_frames = c->target_frames * 4;
    if (c->ring_frames < c->rate) c->ring_frames = c->rate;
}

extern "C" void audio_out_env_config(AudioOutConfig* c, unsigned period)
{
    const unsigned ms = c->rate / 1000;
    c->threshold = env_uint("PORT_AUDIO_STRETCH_THRESHOLD", (unsigned)(c->threshold * 100.0 + 0.5), 1, 100) / 100.0;
    { const char* e = getenv("PORT_AUDIO_STRETCH_MIN");
      if (e && *e) { double v = atof(e); if (!(v >= 0.1)) v = 0.1; if (v > 1.0) v = 1.0; c->min_tempo = v; } }
    c->fade_frames = env_uint("PORT_AUDIO_FADE_MS", c->fade_frames / ms, 1, 50) * ms;
    c->device_frames = env_uint("PORT_AUDIO_DEVICE_MS", c->device_frames / ms, 5, 500) * ms;
    /* the device consumes whole engine periods: keep >= 3 queued, and conceal
     * while there is still more than one period left to play */
    if (period) {
        if (c->device_frames < 3 * period) c->device_frames = 3 * period;
        if (c->guard_frames < period + period / 2) c->guard_frames = period + period / 2;
    }
    if (c->resume_frames < c->guard_frames + c->fade_frames) c->resume_frames = c->guard_frames + c->fade_frames;
}

extern "C" AudioOut* audio_out_create(const AudioOutConfig* cfg)
{
    AudioOut* ao = new AudioOut();
    ao->cfg = *cfg;
    if (ao->cfg.ring_frames < 4096) ao->cfg.ring_frames = 4096;
    if (ao->cfg.target_frames < 256) ao->cfg.target_frames = 256;
    if (!(ao->cfg.threshold > 0.0)) ao->cfg.threshold = 0.75;
    if (!(ao->cfg.min_tempo >= 0.1)) ao->cfg.min_tempo = 0.1;
    if (ao->cfg.min_tempo > 1.0) ao->cfg.min_tempo = 1.0;
    if (ao->cfg.fade_frames < 16) ao->cfg.fade_frames = 16;
    ao->ring.assign((size_t)ao->cfg.ring_frames * 2, 0.0f);
    /* RPCS3's audio_resampler settings, except: a 25 ms sequence instead of
     * 40 (less audio held inside SoundTouch and finer output steps, which
     * the offline test shows matters during 20-40 ms producer stalls), and
     * no anti-alias filter (it only matters for rate changes; the rate
     * stays 1.0 here) */
    ao->st.setChannels(2);
    ao->st.setSampleRate(ao->cfg.rate);
    ao->st.setSetting(SETTING_SEQUENCE_MS, 25);
    ao->st.setSetting(SETTING_SEEKWINDOW_MS, 15);
    ao->st.setSetting(SETTING_OVERLAP_MS, 8);
    ao->st.setSetting(SETTING_USE_QUICKSEEK, 0);
    ao->st.setSetting(SETTING_USE_AA_FILTER, 0);
    ao->st.setTempo(1.0);
    return ao;
}

extern "C" void audio_out_destroy(AudioOut* ao) { delete ao; }
extern "C" const AudioOutConfig* audio_out_config(const AudioOut* ao) { return &ao->cfg; }
extern "C" double audio_out_tempo(const AudioOut* ao) { return ao->tempo; }
extern "C" int audio_out_is_dry(const AudioOut* ao) { return ao->dry ? 1 : 0; }

extern "C" unsigned audio_out_push(AudioOut* ao, const float* in, unsigned frames)
{
    const unsigned cap = ao->cfg.ring_frames;
    if (frames > cap) { ao->s_overflow += frames - cap; in += (size_t)(frames - cap) * 2; frames = cap; }
    if (ao->count + frames > cap) {             /* full: drop the oldest */
        const unsigned drop = ao->count + frames - cap;
        ao->head = (ao->head + drop) % cap; ao->count -= drop; ao->s_overflow += drop;
    }
    unsigned tail = (ao->head + ao->count) % cap;
    unsigned left = frames;
    while (left) {
        const unsigned n = left < cap - tail ? left : cap - tail;
        memcpy(&ao->ring[(size_t)tail * 2], in, (size_t)n * 2 * sizeof(float));
        in += (size_t)n * 2; left -= n; tail = (tail + n) % cap;
    }
    ao->count += frames;
    return frames;
}

extern "C" unsigned audio_out_level(const AudioOut* ao, unsigned device_queued)
{
    return device_queued + ao->count + ao->st.numSamples();
}

/* Move ring audio into the stretcher until it has `need` output frames ready
 * (or the ring is empty). Feeding lazily keeps the audio in the ring, where a
 * tempo change still applies to it. */
static void feed(AudioOut* ao, unsigned need)
{
    const unsigned cap = ao->cfg.ring_frames;
    while (ao->st.numSamples() < need && ao->count) {
        unsigned n = ao->count < 512 ? ao->count : 512;
        if (n > cap - ao->head) n = cap - ao->head;
        ao->st.putSamples(&ao->ring[(size_t)ao->head * 2], n);
        ao->head = (ao->head + n) % cap; ao->count -= n;
    }
}

/* Tempo control, driven by the device clock: called for every chunk handed to
 * the device, so it keeps reacting while the producer is stalled. Like
 * RPCS3, nothing happens above the threshold; below it the tempo follows
 * (fill / threshold)^2 (RPCS3 uses the plain ratio; squaring settles at a
 * fuller buffer for the same slowdown, leaving room for stalls). It drops
 * quickly (25 % of the way per 256 frames played) and recovers gently
 * (+0.01 per 256 frames), so it settles where the slowed playback matches
 * what the game delivers instead of oscillating. */
static void update_tempo(AudioOut* ao, unsigned dq, unsigned played)
{
    if (!ao->primed) return;    /* start-up priming plays at normal speed */
    const unsigned level = audio_out_level(ao, dq);
    const double fill = (double)level / (double)ao->cfg.target_frames;
    double want = 1.0;
    if (fill < ao->cfg.threshold) {
        want = fill / ao->cfg.threshold;
        want *= want;
        if (want < ao->cfg.min_tempo) want = ao->cfg.min_tempo;
    }
    const double steps = played / 256.0;
    if (want < ao->tempo) ao->tempo += (want - ao->tempo) * (1.0 - pow(0.75, steps));
    else { const double up = ao->tempo + 0.01 * steps; ao->tempo = up < want ? up : want; }
    if (ao->tempo > 0.999) ao->tempo = 1.0;
    if (fabs(ao->tempo - ao->applied) > 0.004 || (ao->tempo == 1.0 && ao->applied != 1.0)) {
        ao->st.setTempo(ao->tempo);
        ao->applied = ao->tempo;
    }
    const double lms = level * 1000.0 / ao->cfg.rate;
    if (lms < ao->s_lmin) ao->s_lmin = lms;
}

extern "C" void audio_out_tick(AudioOut* ao, unsigned device_queued)
{
    const double lms = audio_out_level(ao, device_queued) * 1000.0 / ao->cfg.rate;
    if (ao->primed) {
        if (lms < ao->s_lmin) ao->s_lmin = lms;
        ao->s_lsum += lms;
        if (ao->tempo < ao->s_tmin) ao->s_tmin = ao->tempo;
        ao->s_tsum += ao->tempo;
        if (ao->tempo < 1.0) ao->s_stretch++;
        ao->s_ticks++;
    }
}

static unsigned render_inner(AudioOut* ao, float* out, unsigned max, unsigned dq);

extern "C" unsigned audio_out_render(AudioOut* ao, float* out, unsigned max, unsigned dq)
{
    const unsigned n = render_inner(ao, out, max, dq);
    if (n) update_tempo(ao, dq + n, n);
    return n;
}
static unsigned render_inner(AudioOut* ao, float* out, unsigned max, unsigned dq)
{
    if (!max) return 0;
    const unsigned fade = ao->cfg.fade_frames, guard = ao->cfg.guard_frames;
    if (ao->dry) {
        feed(ao, fade + 1024);
        const unsigned need = (ao->primed ? ao->cfg.resume_frames : ao->cfg.target_frames) + fade;
        if (ao->count + ao->st.numSamples() >= need && ao->st.numSamples() > fade) {
            ao->dry = false; ao->primed = true; ao->fadein = 0;
        } else {
            /* keep the device fed with silence (only after an underrun: at
             * start-up the device has not started playing anything yet) */
            unsigned n = ao->primed && dq < guard ? guard - dq : 0;
            if (n > max) n = max;
            memset(out, 0, (size_t)n * 2 * sizeof(float));
            ao->s_silence += n;
            return n;
        }
    }
    feed(ao, max + fade);
    const unsigned ready = ao->st.numSamples();
    if (ready > fade) {
        /* play, holding the last `fade` frames back for a fade-out */
        unsigned n = ready - fade < max ? ready - fade : max;
        n = ao->st.receiveSamples(out, n);
        for (unsigned i = 0; i < n && ao->fadein < fade; i++, ao->fadein++) {
            const float g = (float)(ao->fadein + 1) / (float)(fade + 1);
            out[i * 2] *= g; out[i * 2 + 1] *= g;
        }
        return n;
    }
    if (dq > guard) return 0;   /* the device still has enough: give the producer time */
    /* underrun: fade the held-back audio out, then silence */
    unsigned k = ready < max ? ready : max;
    k = ao->st.receiveSamples(out, k);
    for (unsigned i = 0; i < k; i++) {
        float g = (float)(k - i) / (float)(k + 1);
        if (ao->fadein < fade) { g *= (float)(ao->fadein + 1) / (float)(fade + 1); ao->fadein++; }
        out[i * 2] *= g; out[i * 2 + 1] *= g;
    }
    ao->dry = true; ao->s_underruns++;
    unsigned s = guard > dq + k ? guard - dq - k : 0;
    if (s > max - k) s = max - k;
    memset(out + (size_t)k * 2, 0, (size_t)s * 2 * sizeof(float));
    ao->s_silence += s;
    return k + s;
}

extern "C" void audio_out_take_stats(AudioOut* ao, AudioOutStats* st)
{
    memset(st, 0, sizeof(*st));
    st->ticks = ao->s_ticks;
    st->level_min_ms = ao->s_ticks ? ao->s_lmin : 0.0;
    st->level_avg_ms = ao->s_ticks ? ao->s_lsum / ao->s_ticks : 0.0;
    st->tempo_min = ao->s_ticks ? ao->s_tmin : ao->tempo;
    st->tempo_avg = ao->s_ticks ? ao->s_tsum / ao->s_ticks : ao->tempo;
    st->stretch_ticks = ao->s_stretch;
    st->underruns = ao->s_underruns;
    st->silence_ms = (double)ao->s_silence * 1000.0 / ao->cfg.rate;
    st->overflow_frames = ao->s_overflow;
    ao->s_ticks = ao->s_stretch = ao->s_underruns = ao->s_overflow = 0;
    ao->s_lmin = 1e30; ao->s_lsum = 0.0; ao->s_tmin = 1.0; ao->s_tsum = 0.0; ao->s_silence = 0;
}
