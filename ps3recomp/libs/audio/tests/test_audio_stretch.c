/* Unit test for the WSOLA output stretcher (audio_stretch.h). Pure code.
 * Windows: cl /nologo /O2 libs\audio\tests\test_audio_stretch.c
 * POSIX:   cc -O2 libs/audio/tests/test_audio_stretch.c -lm
 */
#include "../audio_stretch.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static AudioStretch st;

static void gen(float* buf, int frames, long long* phase, double hz)
{
    for (int i = 0; i < frames; i++, (*phase)++) {
        const double t = (double)*phase / 48000.0;
        buf[i * 2]     = (float)(0.5 * sin(2.0 * 3.14159265358979 * hz * t));
        buf[i * 2 + 1] = (float)(0.5 * sin(2.0 * 3.14159265358979 * hz * t + 0.3));
    }
}

/* Drive the stretcher like the mix thread: push 256-frame blocks only when it
 * asks for input; returns output hops produced. */
static long long g_pushed;
static int run(double tempo, int hops, float* out, double hz)
{
    static float blk[256 * 2];
    long long ph = 0; int made = 0, guard = 0;
    as_init(&st); g_pushed = 0;
    while (made < hops && guard++ < 100000) {
        if (as_process(&st, tempo, out + (size_t)made * AS_HS * 2)) { made++; continue; }
        gen(blk, 256, &ph, hz);
        assert(as_push(&st, blk, 256) == 256); g_pushed += 256;
    }
    return made;
}

int main(void)
{
    enum { HOPS = 400 };
    float* out = malloc((size_t)HOPS * AS_HS * 2 * sizeof(float));
    float* ref = malloc((size_t)HOPS * AS_HS * 2 * sizeof(float));
    assert(out && ref);

    /* tempo 1: exact passthrough after the first (faded-in) hop */
    assert(run(1.0, HOPS, out, 440.0) == HOPS);
    { long long ph = 0; gen(ref, HOPS * AS_HS, &ph, 440.0); }
    double maxerr = 0.0;
    for (int i = AS_HS * 2; i < HOPS * AS_HS * 2; i++) {
        const double e = fabs((double)out[i] - ref[i]); if (e > maxerr) maxerr = e;
    }
    printf("tempo 1.00: max error %.2e\n", maxerr);
    assert(maxerr < 1e-5);

    /* tempo < 1: continuous (no clicks), level kept, input consumed at tempo */
    const double tempos[] = { 0.9, 0.8, 0.7 };
    for (int t = 0; t < 3; t++) {
        assert(run(tempos[t], HOPS, out, 440.0) == HOPS);
        double maxstep = 0.0, e = 0.0;
        for (int i = AS_HS * 2 * 2; i < HOPS * AS_HS * 2; i += 2) {
            const double d = fabs((double)out[i] - out[i - 2]); if (d > maxstep) maxstep = d;
            e += (double)out[i] * out[i];
        }
        const double rms = sqrt(e / (HOPS * AS_HS - AS_HS * 2));
        /* a 0.5-amplitude 440 Hz sine steps at most 0.029 per sample */
        const double consumed = (g_pushed - (st.in_len - st.pos)) / ((double)HOPS * AS_HS);
        printf("tempo %.2f: max step %.4f, rms %.3f (sine 0.354), consumed/out %.3f\n",
               tempos[t], maxstep, rms, consumed);
        assert(maxstep < 0.06);
        assert(rms > 0.32 && rms < 0.39);
        assert(fabs(consumed - tempos[t]) < 0.02);
    }

    /* tempo change mid-stream back to 1 stays continuous */
    {
        static float blk[256 * 2]; long long ph = 0; int made = 0;
        as_init(&st);
        while (made < HOPS) {
            const double tempo = (made / 50) & 1 ? 0.75 : 1.0;
            if (as_process(&st, tempo, out + (size_t)made * AS_HS * 2)) { made++; continue; }
            gen(blk, 256, &ph, 300.0); assert(as_push(&st, blk, 256) == 256);
        }
        double maxstep = 0.0;
        for (int i = AS_HS * 2 * 2; i < HOPS * AS_HS * 2; i += 2) {
            const double d = fabs((double)out[i] - out[i - 2]); if (d > maxstep) maxstep = d;
        }
        printf("switching tempo: max step %.4f\n", maxstep);
        assert(maxstep < 0.05);
    }
    free(out); free(ref);
    printf("test_audio_stretch: OK\n");
    return 0;
}
