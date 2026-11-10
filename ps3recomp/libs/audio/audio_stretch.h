/*
 * ps3recomp - WSOLA tempo stretcher for the host audio output stage.
 *
 * When the guest cannot produce audio in real time (a CPU-bound host: the
 * game's synth jobs fall behind the 5.33 ms block clock) the host buffer
 * drains and the device plays holes. Like RPCS3's "time stretching" (it uses
 * SoundTouch) this plays the mixed stream slower, pitch preserved, while the
 * host buffer is low, instead of letting the device run dry.
 *
 * Pure code: no OS calls, no allocation. Stereo interleaved float.
 * At tempo 1.0 every grain is the natural continuation of the previous one,
 * and the periodic Hann windows at 50% overlap sum to exactly 1, so the
 * output is the input delayed by AS_HS frames, bit-for-bit up to float
 * rounding: an idle stretcher changes nothing.
 */
#ifndef PS3RECOMP_AUDIO_STRETCH_H
#define PS3RECOMP_AUDIO_STRETCH_H

#include <math.h>
#include <string.h>

#define AS_N      1024              /* grain length, frames (21.3 ms @ 48 kHz) */
#define AS_HS     (AS_N / 2)        /* output hop, frames */
#define AS_SEEK   256               /* +- similarity search, frames */
#define AS_IN_CAP 16384             /* input window, frames */

typedef struct {
    float    in[AS_IN_CAP * 2];
    int      in_len;                /* frames valid in in[] */
    double   pos;                   /* nominal analysis position (frames) */
    int      prev;                  /* start of the previous grain, -1 = none */
    float    tail[AS_HS * 2];       /* second half of the previous windowed grain */
    float    win[AS_N];
} AudioStretch;

static void as_init(AudioStretch* s)
{
    memset(s, 0, sizeof(*s));
    s->prev = -1;
    for (int i = 0; i < AS_N; i++)
        s->win[i] = (float)(0.5 - 0.5 * cos(2.0 * 3.14159265358979323846 * i / AS_N));
}

/* Input frames held that have not been played yet (for buffer accounting). */
static int as_pending(const AudioStretch* s)
{
    const int from = s->prev >= 0 ? s->prev + AS_HS : (int)s->pos;
    const int n = s->in_len - from;
    return n > 0 ? n : 0;
}

/* Room for more input, frames. */
static int as_room(const AudioStretch* s) { return AS_IN_CAP - s->in_len; }

static void as_compact(AudioStretch* s)
{
    int keep = (int)s->pos - AS_SEEK;
    if (s->prev >= 0 && s->prev < keep) keep = s->prev;
    if (keep < AS_IN_CAP / 4) return;
    memmove(s->in, s->in + keep * 2, (size_t)(s->in_len - keep) * 2 * sizeof(float));
    s->in_len -= keep;
    s->pos -= keep;
    if (s->prev >= 0) s->prev -= keep;
}

/* Append frames; returns how many were taken. */
static int as_push(AudioStretch* s, const float* stereo, int frames)
{
    if (as_room(s) < frames) as_compact(s);
    if (frames > as_room(s)) frames = as_room(s);
    if (frames <= 0) return 0;
    memcpy(s->in + s->in_len * 2, stereo, (size_t)frames * 2 * sizeof(float));
    s->in_len += frames;
    return frames;
}

/* Best grain start in [lo, hi] whose head matches in[target..target+AS_HS),
 * by normalised cross-correlation of the mono sum (coarse, then refined). */
static int as_seek(const AudioStretch* s, int target, int lo, int hi)
{
    const float* in = s->in;
    float ref[AS_HS / 2];
    double rn = 0.0;
    for (int i = 0; i < AS_HS / 2; i++) {
        const int k = (target + i * 2) * 2;
        ref[i] = in[k] + in[k + 1];
        rn += (double)ref[i] * ref[i];
    }
    if (rn < 1e-9) return target < lo ? lo : (target > hi ? hi : target);   /* silence */
    int best = lo; double bestc = -2.0;
    for (int pass = 0; pass < 2; pass++) {
        int a = lo, b = hi, step = 4;
        if (pass) { a = best - 3; b = best + 3; step = 1; if (a < lo) a = lo; if (b > hi) b = hi; }
        for (int c = a; c <= b; c += step) {
            double xy = 0.0, yy = 0.0;
            for (int i = 0; i < AS_HS / 2; i++) {
                const int k = (c + i * 2) * 2;
                const float y = in[k] + in[k + 1];
                xy += (double)ref[i] * y; yy += (double)y * y;
            }
            const double cc = yy > 1e-12 ? xy / sqrt(yy * rn) : 0.0;
            if (cc > bestc) { bestc = cc; best = c; }
        }
    }
    return best;
}

/* Produce AS_HS output frames at `tempo` (input frames consumed per output
 * frame, 0.5..1.0). Returns 1 on success, 0 if more input is needed. */
static int as_process(AudioStretch* s, double tempo, float* out)
{
    if (tempo > 1.0) tempo = 1.0;
    if (tempo < 0.5) tempo = 0.5;
    int start;
    if (s->prev < 0) {
        start = (int)s->pos;
        if (start + AS_N > s->in_len) return 0;
    } else {
        const int target = s->prev + AS_HS;          /* natural continuation */
        if (tempo >= 0.9999) {
            start = target;
            if (start + AS_N > s->in_len) return 0;
            s->pos = target;                         /* re-anchor: exact passthrough */
        } else {
            const int nom = (int)(s->pos + 0.5);
            int lo = nom - AS_SEEK, hi = nom + AS_SEEK;
            if (lo < 0) lo = 0;
            if (hi + AS_N > s->in_len || target + AS_N > s->in_len) return 0;
            if (hi < lo) hi = lo;
            start = as_seek(s, target, lo, hi);
        }
    }
    const float* g = s->in + start * 2;
    for (int i = 0; i < AS_HS; i++) {
        const float w0 = s->win[i], w1 = s->win[i + AS_HS];
        out[i * 2]     = s->tail[i * 2]     + g[i * 2]     * w0;
        out[i * 2 + 1] = s->tail[i * 2 + 1] + g[i * 2 + 1] * w0;
        s->tail[i * 2]     = g[(i + AS_HS) * 2]     * w1;
        s->tail[i * 2 + 1] = g[(i + AS_HS) * 2 + 1] * w1;
    }
    s->prev = start;
    s->pos += tempo * AS_HS;
    return 1;
}

#endif /* PS3RECOMP_AUDIO_STRETCH_H */
