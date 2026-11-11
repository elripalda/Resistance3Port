/*
 * ps3recomp - pure helpers for the SDL3 pad backend (pad_sdl3.c)
 *
 * No SDL, no Windows: gyro calibration / player-space / tightening, the
 * gyro -> stick fallback, and the duplicate-pad (virtual wrapper) detector.
 * Kept header-only so scratch\padtest can test it offline.
 */
#ifndef PS3RECOMP_PAD_LOGIC_H
#define PS3RECOMP_PAD_LOGIC_H

#include <math.h>
#include <string.h>

/* XInput XINPUT_GAMEPAD_* button bits (the pad state handed to cellPad) */
#define PADX_UP     0x0001u
#define PADX_DOWN   0x0002u
#define PADX_LEFT   0x0004u
#define PADX_RIGHT  0x0008u
#define PADX_START  0x0010u
#define PADX_BACK   0x0020u
#define PADX_LTHUMB 0x0040u
#define PADX_RTHUMB 0x0080u
#define PADX_LB     0x0100u
#define PADX_RB     0x0200u
#define PADX_A      0x1000u
#define PADX_B      0x2000u
#define PADX_X      0x4000u
#define PADX_Y      0x8000u

/* Nintendo layout: A (east) confirms and B (south) cancels, as on a Switch.
 * The PS3 confirms with Cross (south), so with the swap A/B and X/Y trade places. */
static inline unsigned short padl_nintendo_swap(unsigned short b)
{
    unsigned short o = (unsigned short)(b & ~(PADX_A | PADX_B | PADX_X | PADX_Y));
    if (b & PADX_A) o |= PADX_B;
    if (b & PADX_B) o |= PADX_A;
    if (b & PADX_X) o |= PADX_Y;
    if (b & PADX_Y) o |= PADX_X;
    return o;
}

/* ---------------------------------------------------------------------------
 * Gyro
 * -------------------------------------------------------------------------*/
#define PADG_DEG              57.29577951308232
#define PADG_CAL_WINDOW_S     1.0     /* average the bias over 1 s of stillness */
#define PADG_CAL_TIMEOUT_S    6.0     /* give up waiting for stillness after this */
#define PADG_STILL_RAD_S      0.12    /* ~7 deg/s: faster than this is "moving" */
#define PADG_TIGHT_DEG_S      1.5     /* below this, output fades to 0 (sensor noise) */

typedef struct {
    double sens;          /* camera degrees per degree turned */
    int    invert_y;
    int    player_space;  /* yaw about gravity instead of the pad's own axis */
    int    calibrate;     /* learn the resting bias at connect */
} PadGyroCfg;

typedef struct {
    int    calibrating, calibrated;
    double cal_t0, cal_start;
    double sum[3]; int n;
    double bias[3];
    double grav[3]; int have_grav;     /* low-passed accelerometer (points up at rest) */
    double last_t, last_at; int have_t, have_at;
    double rate_yaw, rate_pitch;       /* smoothed output rate, deg/s (mouse convention) */
} PadGyro;

static inline void padg_reset(PadGyro* g, int calibrate)
{
    memset(g, 0, sizeof *g);
    g->calibrating = calibrate ? 1 : 0;
    g->calibrated = calibrate ? 0 : 1;
}

/* Accelerometer sample (m/s^2, t in seconds). */
static inline void padg_accel(PadGyro* g, double t, const float a[3])
{
    if (!g->have_grav) {
        for (int k = 0; k < 3; k++) g->grav[k] = a[k];
        g->have_grav = 1; g->last_at = t; g->have_at = 1;
        return;
    }
    double dt = g->have_at ? t - g->last_at : 0.004;
    if (dt <= 0.0 || dt > 0.1) dt = 0.004;
    g->last_at = t; g->have_at = 1;
    const double al = 1.0 - exp(-dt / 0.15);
    for (int k = 0; k < 3; k++) g->grav[k] += (a[k] - g->grav[k]) * al;
}

/* Gyro sample (rad/s, SDL axes: x right, y up, z towards the player,
 * counter-clockwise positive). Adds this sample's turn to *yaw_deg / *pitch_deg
 * in mouse convention: yaw + = turn right, pitch + = look down.
 * Returns 0 while calibrating (no output). */
static inline int padg_gyro(PadGyro* g, const PadGyroCfg* c, double t, const float w_in[3],
                            double* yaw_deg, double* pitch_deg)
{
    double dt = g->have_t ? t - g->last_t : 0.0;
    if (dt <= 0.0 || dt > 0.05) dt = g->have_t ? 0.004 : 0.0;
    g->last_t = t; g->have_t = 1;
    double w[3] = { w_in[0], w_in[1], w_in[2] };

    if (g->calibrating) {
        if (g->n == 0 && g->cal_start == 0.0) { g->cal_start = t; g->cal_t0 = t; }
        const double m = sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
        int moving = m > PADG_STILL_RAD_S;
        if (!moving && g->n >= 8) {
            for (int k = 0; k < 3; k++)
                if (fabs(w[k] - g->sum[k] / g->n) > PADG_STILL_RAD_S * 0.5) moving = 1;
        }
        if (moving) { g->n = 0; g->sum[0] = g->sum[1] = g->sum[2] = 0.0; g->cal_t0 = t; }
        else { for (int k = 0; k < 3; k++) g->sum[k] += w[k]; g->n++; }
        const int done = (g->n >= 20 && t - g->cal_t0 >= PADG_CAL_WINDOW_S);
        if (done || t - g->cal_start >= PADG_CAL_TIMEOUT_S) {
            for (int k = 0; k < 3; k++) g->bias[k] = g->n >= 20 ? g->sum[k] / g->n : 0.0;
            g->calibrating = 0; g->calibrated = done ? 1 : -1;   /* -1: timed out, no bias */
        }
        return 0;
    }
    for (int k = 0; k < 3; k++) w[k] -= g->bias[k];

    double yaw = w[1], pitch = w[0];
    if (c->player_space && g->have_grav) {
        const double gl = sqrt(g->grav[0] * g->grav[0] + g->grav[1] * g->grav[1] + g->grav[2] * g->grav[2]);
        if (gl > 4.0) {   /* a usable gravity reading (m/s^2) */
            const double wy = (g->grav[1] * w[1] + g->grav[2] * w[2]) / gl;
            const double lim = sqrt(w[1] * w[1] + w[2] * w[2]);
            double a = fabs(wy) * 1.41;
            if (a > lim) a = lim;
            yaw = wy < 0.0 ? -a : a;
        }
    }
    /* soft "tightening": tiny rates (sensor noise) fade out instead of drifting */
    const double mag = sqrt(yaw * yaw + pitch * pitch) * PADG_DEG;
    double scale = mag < PADG_TIGHT_DEG_S ? mag / PADG_TIGHT_DEG_S : 1.0;
    const double ry = -yaw * PADG_DEG * scale * c->sens;           /* deg/s, + = right */
    double rp = -pitch * PADG_DEG * scale * c->sens;               /* deg/s, + = down */
    if (c->invert_y) rp = -rp;
    *yaw_deg += ry * dt;
    *pitch_deg += rp * dt;
    if (dt > 0.0) {
        const double al = 1.0 - exp(-dt / 0.03);
        g->rate_yaw += (ry - g->rate_yaw) * al;
        g->rate_pitch += (rp - g->rate_pitch) * al;
    }
    return 1;
}

/* Gyro rate (deg/s) as extra right-stick deflection (-1..1 per axis, + = right
 * / down), for when the game's look code can't take it directly (vehicles,
 * player 2). `full` deg/s = full deflection; `adz` lifts small rates past the
 * game's own stick deadzone so slow turns still register. */
static inline void padg_rate_to_stick(double yaw_dps, double pitch_dps, double full, double adz,
                                      double* sx, double* sy)
{
    *sx = *sy = 0.0;
    const double m = sqrt(yaw_dps * yaw_dps + pitch_dps * pitch_dps);
    if (m < 0.5 || full <= 0.0) return;
    double d = m / full;
    if (d > 1.0) d = 1.0;
    d = adz + (1.0 - adz) * d;
    *sx = yaw_dps / m * d;
    *sy = pitch_dps / m * d;
}

/* ---------------------------------------------------------------------------
 * Duplicate pads
 *
 * DS4Windows (and similar wrappers) read a PlayStation pad and present a
 * virtual Xbox 360 pad through XInput, while SDL3 also opens the real pad
 * over HID: the same hands then drive two "controllers", and the second one
 * takes player 2. A virtual pad cannot be told from a real 360 pad by its
 * ids, so a candidate (Xbox 360 / generic XInput ids, appearing while a
 * PlayStation or Nintendo pad is connected) is held back and compared with
 * the real pad: if it keeps mirroring the real pad's input it is a ghost and
 * ignored; once it does something the real pad did not, it is a controller.
 * -------------------------------------------------------------------------*/
#define PADD_HIST 48   /* the other pad's last ~50-90 ms of states (one per input-thread pass) */

typedef struct { unsigned sig[PADD_HIST]; int pos, n; } PadSigHist;
typedef struct { int match, mismatch; } PadPair;

/* Quantised state: buttons (Back left out: wrappers map the touchpad / Share
 * differently), each stick axis as -/0/+, triggers half-pressed. */
static inline unsigned padd_sig(unsigned short buttons, short lx, short ly, short rx, short ry,
                                unsigned char lt, unsigned char rt)
{
    unsigned s = (unsigned)(buttons & ~PADX_BACK) & 0xFFFFu;
    const short ax[4] = { lx, ly, rx, ry };
    for (int i = 0; i < 4; i++) {
        if (ax[i] > 16384) s |= 1u << (16 + 2 * i);
        else if (ax[i] < -16384) s |= 2u << (16 + 2 * i);
    }
    if (lt > 128) s |= 1u << 24;
    if (rt > 128) s |= 1u << 25;
    return s;
}
static inline void padd_hist_push(PadSigHist* h, unsigned sig)
{
    h->sig[h->pos] = sig;
    h->pos = (h->pos + 1) % PADD_HIST;
    if (h->n < PADD_HIST) h->n++;
}
static inline int padd_popc(unsigned v) { int c = 0; while (v) { v &= v - 1; c++; } return c; }
static inline int padd_close(unsigned a, unsigned b)
{
    const unsigned d = a ^ b;
    return (d & 0xFFFFu) == 0 && padd_popc(d & 0x03FF0000u) <= 1;
}
/* One sample of the candidate against the real pad's recent history.
 * Returns +1 = ghost (mirrors it), -1 = a different controller, 0 = not sure yet. */
static inline int padd_update(PadPair* pp, unsigned sig_c, const PadSigHist* real)
{
    if (sig_c) {
        int hit = 0;
        for (int i = 0; i < real->n && !hit; i++) if (real->sig[i] && padd_close(real->sig[i], sig_c)) hit = 1;
        if (hit) pp->match++; else pp->mismatch++;
        if (pp->match + pp->mismatch > 400) { pp->match /= 2; pp->mismatch /= 2; }
    }
    if (pp->match >= 12 && pp->mismatch * 5 <= pp->match) return 1;
    if (pp->mismatch >= 8 && pp->mismatch > pp->match) return -1;
    return 0;
}
/* Ids that a virtual wrapper pad reports: Xbox 360 wired (ViGEm / DS4Windows,
 * also a real 360 pad) and SDL's generic XInput id. */
static inline int padd_virtual_candidate(unsigned short vid, unsigned short pid)
{
    return vid == 0x045E && (pid == 0x028E || pid == 0x02FF);
}

/* Lowest free slot (stable assignment: a pad keeps its slot while connected). */
static inline int padl_lowest_free(const int* used, int n)
{
    for (int i = 0; i < n; i++) if (!used[i]) return i;
    return -1;
}

/* PAD_GYRO = 0|off | 1|on | aim */
static inline int padl_gyro_mode(const char* e)
{
    if (!e || !*e) return 0;
    if (e[0] == 'a' || e[0] == 'A') return 2;
    if (e[0] == '1' || ((e[0] == 'o' || e[0] == 'O') && (e[1] == 'n' || e[1] == 'N'))) return 1;
    return 0;
}

#endif /* PS3RECOMP_PAD_LOGIC_H */
