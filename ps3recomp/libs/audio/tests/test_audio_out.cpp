/*
 * Offline test for libs/audio/audio_out (ring buffer + SoundTouch stretch).
 *
 * Simulates the real setup: a 48 kHz shared-mode device that plays 480 frames
 * every 10 ms, a mixer loop waking every 1 ms, and a guest producer that
 * mixes 256-frame blocks of a continuous 440 Hz tone. The producer is made
 * irregular (fps drops: slow stretches plus 20-40 ms stalls). Checks the
 * device output for gaps and discontinuities and that the tempo recovers.
 */
#include "../audio_out.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <deque>

static const double PI = 3.14159265358979323846;
static const unsigned RATE = 48000, PERIOD = 480, BLOCK = 256;
static const float AMP = 0.5f;
static const double FREQ = 440.0;
static int g_fail = 0;

#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL: " __VA_ARGS__); printf("\n"); g_fail++; } } while (0)

struct Rng { unsigned s; double next() { s = s * 1664525u + 1013904223u; return (s >> 8) / 16777216.0; } };

/* producer speed (fraction of real time) at simulated time t ms; <0 = stalled */
typedef double (*SpeedFn)(double t, Rng& r);

struct Result {
    std::vector<float> out;        /* left channel as played */
    unsigned device_dry = 0;       /* periods the device found short */
    unsigned underruns = 0;
    double tempo_min = 1.0;
    double recover_ms = -1;        /* time after the drop ends until tempo 1.0 */
    double level_min_ms = 1e9;
    std::vector<double> tempo_trace;
};

static Result simulate(SpeedFn speed, double total_ms, double drop_end_ms, bool stretch, unsigned buffer_ms, unsigned seed = 12345)
{
    Result R;
    AudioOutConfig cfg;
    audio_out_default_config(&cfg, RATE, buffer_ms);
    audio_out_env_config(&cfg, PERIOD);
    AudioOut* ao = stretch ? audio_out_create(&cfg) : nullptr;
    std::deque<float> naive;       /* legacy-style FIFO for the comparison */
    bool naive_primed = false;
    std::deque<float> dev;         /* device queue, stereo */
    double phase = 0.0, guest_ms = 0.0, next_period = 10.0;
    std::vector<float> blk(BLOCK * 2), tmp(8192 * 2);
    Rng rng{seed};
    bool recovered_seen = false;

    for (double t = 1.0; t <= total_ms; t += 1.0) {
        /* guest progress this ms */
        const double sp = speed(t, rng);
        if (sp > 0) guest_ms += sp;
        while (guest_ms >= BLOCK * 1000.0 / RATE) {
            guest_ms -= BLOCK * 1000.0 / RATE;
            for (unsigned i = 0; i < BLOCK; i++) {
                const float v = AMP * (float)sin(phase);
                phase += 2 * PI * FREQ / RATE;
                if (phase > 2 * PI) phase -= 2 * PI;
                blk[i * 2] = v; blk[i * 2 + 1] = v;
            }
            if (ao) { audio_out_push(ao, blk.data(), BLOCK); audio_out_tick(ao, (unsigned)dev.size() / 2); }
            else naive.insert(naive.end(), blk.begin(), blk.end());
        }
        /* mixer: top the device up */
        if (ao) {
            const unsigned cap = audio_out_config(ao)->device_frames;
            for (;;) {
                const unsigned dq = (unsigned)dev.size() / 2;
                if (dq >= cap) break;
                unsigned want = cap - dq; if (want > 8192) want = 8192;
                const unsigned n = audio_out_render(ao, tmp.data(), want, dq);
                if (!n) break;
                dev.insert(dev.end(), tmp.begin(), tmp.begin() + n * 2);
            }
            const double tp = audio_out_tempo(ao);
            if (tp < R.tempo_min) R.tempo_min = tp;
            if (t >= drop_end_ms && !recovered_seen && tp == 1.0) { recovered_seen = true; R.recover_ms = t - drop_end_ms; }
            if (t < drop_end_ms) recovered_seen = false;
            if ((int)t % 50 == 0) R.tempo_trace.push_back(tp);
        } else {
            /* naive: prime 100 ms once, then pass blocks straight through */
            if (!naive_primed && naive.size() / 2 >= buffer_ms * 48u) naive_primed = true;
            if (naive_primed) { dev.insert(dev.end(), naive.begin(), naive.end()); naive.clear(); }
        }
        /* device: plays one period every 10 ms */
        if (t >= next_period) {
            next_period += 10.0;
            const unsigned have = (unsigned)dev.size() / 2;
            if (have < PERIOD && (ao ? !audio_out_is_dry(ao) || have : naive_primed)) R.device_dry++;
            for (unsigned i = 0; i < PERIOD; i++) {
                if (i < have) { R.out.push_back(dev[0]); dev.pop_front(); dev.pop_front(); }
                else if (ao ? true : naive_primed) R.out.push_back(0.0f);
            }
        }
    }
    if (ao) {
        AudioOutStats st; audio_out_take_stats(ao, &st);
        R.underruns = st.underruns;
        R.level_min_ms = st.level_min_ms;
        audio_out_destroy(ao);
    }
    return R;
}

/* skip the start-up silence */
static size_t first_sound(const std::vector<float>& v)
{
    for (size_t i = 0; i < v.size(); i++) if (fabsf(v[i]) > 1e-3f) return i;
    return v.size();
}

static double max_jump(const std::vector<float>& v, size_t from)
{
    double m = 0;
    for (size_t i = from + 1; i < v.size(); i++) { const double d = fabs(v[i] - v[i - 1]); if (d > m) m = d; }
    return m;
}

/* longest run of near-silence after playback started, in ms */
static double longest_gap_ms(const std::vector<float>& v, size_t from)
{
    size_t run = 0, best = 0;
    for (size_t i = from; i < v.size(); i++) {
        if (fabsf(v[i]) < 0.02f * AMP) { if (++run > best) best = run; } else run = 0;
    }
    return best * 1000.0 / RATE;
}

/* average frequency from zero crossings over [a, b) ms of output */
static double zc_freq(const std::vector<float>& v, double a_ms, double b_ms)
{
    const size_t a = (size_t)(a_ms * RATE / 1000), b = (size_t)(b_ms * RATE / 1000);
    unsigned zc = 0;
    for (size_t i = a + 1; i < b && i < v.size(); i++) if ((v[i - 1] < 0) != (v[i] < 0)) zc++;
    return zc / 2.0 / ((b - a) / (double)RATE);
}

static const double CLEAN_STEP = AMP * 2 * PI * FREQ / RATE;   /* 0.0288 */

static double sp_steady(double, Rng&) { return 1.0; }
/* 1 s normal, 3 s "fps drop": 80 % speed plus a 20-40 ms stall every
 * 150-400 ms (about 70 % overall), then normal again */
static double sp_drop(double t, Rng& r)
{
    static double stall_until = 0, next_stall = 0;
    if (t <= 1.0) { stall_until = 0; next_stall = 1000; }
    if (t < 1000 || t >= 4000) return 1.0;
    if (t < stall_until) return -1;
    if (t >= next_stall) { stall_until = t + 20 + 20 * r.next(); next_stall = t + 150 + 250 * r.next(); return -1; }
    return 0.80;
}
/* one 300 ms hitch (a loading stall): the buffer must run dry */
static double sp_hitch(double t, Rng&) { return (t >= 1500 && t < 1800) ? -1 : 1.0; }

int main()
{
    printf("test_audio_out: SoundTouch output stage, %u Hz, device period %u\n", RATE, PERIOD);

    {   /* steady producer: plays at tempo 1, never dry */
        Result r = simulate(sp_steady, 3000, 0, true, 100);
        const size_t s = first_sound(r.out);
        const double j = max_jump(r.out, s), g = longest_gap_ms(r.out, s);
        printf("steady : start %.0f ms, max jump %.4f (clean %.4f), longest gap %.2f ms, tempo min %.3f, underruns %u, device dry %u\n",
               s * 1000.0 / RATE, j, CLEAN_STEP, g, r.tempo_min, r.underruns, r.device_dry);
        CHECK(r.tempo_min == 1.0, "steady: tempo changed (%.3f)", r.tempo_min);
        CHECK(r.underruns == 0 && r.device_dry == 0, "steady: underrun");
        CHECK(j < 3 * CLEAN_STEP, "steady: discontinuity %.4f", j);
        CHECK(g < 1.0, "steady: gap %.2f ms", g);
    }
    {   /* fps drop: stretches instead of gapping, pitch kept, recovers afterwards */
        const unsigned seeds[] = {12345, 1, 777, 4242, 99991};
        for (unsigned seed : seeds) {
            Result r = simulate(sp_drop, 9000, 4000, true, 100, seed);
            const size_t s = first_sound(r.out);
            const double j = max_jump(r.out, s), g = longest_gap_ms(r.out, s);
            /* output time of the stretched part: the drop starts ~1 s into playback */
            const double f = zc_freq(r.out, s * 1000.0 / RATE + 1500, s * 1000.0 / RATE + 4000);
            printf("fpsdrop seed %5u: max jump %.4f, longest gap %.2f ms, pitch %.1f Hz, tempo min %.3f, back to 1.0 %.0f ms after the drop, underruns %u, device dry %u\n",
                   seed, j, g, f, r.tempo_min, r.recover_ms, r.underruns, r.device_dry);
            if (seed == seeds[0]) {
                printf("         tempo every 250 ms:");
                for (size_t i = 0; i < r.tempo_trace.size(); i += 5) printf(" %.2f", r.tempo_trace[i]);
                printf("\n");
            }
            CHECK(r.tempo_min < 0.95, "fpsdrop: never stretched");
            CHECK(r.underruns == 0 && r.device_dry == 0, "fpsdrop: ran dry (%u underruns, %u device dry)", r.underruns, r.device_dry);
            CHECK(j < 3 * CLEAN_STEP, "fpsdrop: discontinuity %.4f", j);
            CHECK(g < 1.0, "fpsdrop: gap %.2f ms", g);
            CHECK(fabs(f - FREQ) < 5.0, "fpsdrop: pitch moved to %.1f Hz", f);
            CHECK(r.recover_ms >= 0 && r.recover_ms < 3000, "fpsdrop: tempo did not recover (%.0f)", r.recover_ms);
        }
        Result n = simulate(sp_drop, 9000, 4000, false, 100);
        const size_t ns = first_sound(n.out);
        printf("naive FIFO, same producer: device dry %u periods, longest gap %.2f ms, max jump %.4f\n",
               n.device_dry, longest_gap_ms(n.out, ns), max_jump(n.out, ns));
    }
    {   /* 300 ms stall: must fade out to silence and back in, no clicks */
        Result r = simulate(sp_hitch, 4000, 1800, true, 100);
        const size_t s = first_sound(r.out);
        const double j = max_jump(r.out, s), g = longest_gap_ms(r.out, s);
        printf("hitch  : max jump %.4f, longest gap %.1f ms, underruns %u, device dry %u, recovered %.0f ms after\n",
               j, g, r.underruns, r.device_dry, r.recover_ms);
        CHECK(r.underruns == 1, "hitch: expected one underrun, got %u", r.underruns);
        CHECK(r.device_dry == 0, "hitch: device found short %u times (silence must be submitted)", r.device_dry);
        CHECK(j < 3 * CLEAN_STEP, "hitch: click %.4f", j);
        CHECK(g > 50 && g < 300, "hitch: gap %.1f ms", g);
        CHECK(r.recover_ms >= 0 && r.recover_ms < 3000, "hitch: tempo did not recover");
    }

    printf(g_fail ? "FAILED (%d)\n" : "ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
