/*
 * ps3recomp - SDL3 gamepad backend (Windows PC ports). See pad_sdl3.h.
 *
 * Built only when CMake finds third_party/SDL3 (PS3RECOMP_PAD_SDL3). SDL3 is
 * linked statically, so there is no SDL3.dll to ship.
 *
 * Why a thread: SDL's joystick code on Windows wants its events pumped on the
 * thread that initialised it, and the guest polls the pad from several
 * threads. Everything SDL happens on "pad-sdl3"; the game side only touches
 * the snapshot / request arrays under s_lock.
 */
#if defined(_WIN32) && defined(PS3RECOMP_PAD_SDL3)

#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <SDL3/SDL.h>

#include "pad_sdl3.h"
#include "pad_logic.h"

#define PADX_MAX_DEV 16

typedef struct {
    SDL_JoystickID id;
    SDL_Gamepad*   gp;
    int            slot;          /* -1 = not playing (pending / ghost) */
    int            state;         /* 0 pending, 1 active, 2 ghost */
    int            cand;          /* could be a wrapper's virtual copy of another pad */
    int            recheck;       /* active candidate still being compared */
    int            real_ref;      /* a PlayStation / Nintendo pad (the kind wrappers copy) */
    PadPair        pair[PADX_MAX_DEV];
    PadSigHist     hist;
    unsigned       sig;
    int            type;
    unsigned short vid, pid;
    char           name[48];
    /* gyro */
    SDL_SensorType gyro_s, accel_s;
    int            has_gyro, has_accel, sensors_on;
    PadGyro        g;
    double         acc_yaw, acc_pitch;     /* degrees, under s_lock */
    Uint64         last_gyro_ns;
    /* rumble / leds applied */
    unsigned short rum_lo, rum_hi;
    Uint64         rum_ns;
    int            led_player, led_bar, led_set;
    PadxState      st;
} PadDev;

static PadDev  s_dev[PADX_MAX_DEV];
static int     s_ndev = 0;

static SRWLOCK s_lock = SRWLOCK_INIT;
static PadxState s_snap[PADX_SLOTS];
static double  s_gy_yaw[PADX_SLOTS], s_gy_pitch[PADX_SLOTS];      /* taken by the game */
static double  s_gy_ryaw[PADX_SLOTS], s_gy_rpitch[PADX_SLOTS];    /* rate, deg/s */
static Uint64  s_gy_t[PADX_SLOTS];
static int     s_gy_cal[PADX_SLOTS];
static unsigned short s_req_lo[PADX_SLOTS], s_req_hi[PADX_SLOTS];
static int     s_req_player[PADX_SLOTS], s_req_bar[PADX_SLOTS];
static volatile LONG64 s_poll_ms = 0;

static volatile int s_cfg_gyro = 0, s_cfg_inv = 0, s_cfg_space = 0, s_cfg_cal = 1, s_cfg_recal = 0;
static volatile double s_cfg_sens = 2.0;
static volatile int s_nswap = 0, s_dedup = 1;

static volatile int s_state = 0;   /* 0 not started, 1 running, -1 failed */
static volatile int s_quit = 0;
static HANDLE s_thread = NULL, s_ready = NULL;

static int padx_class_of(SDL_Gamepad* gp, unsigned short vid)
{
    SDL_GamepadType t = SDL_GetGamepadType(gp);   /* Steam's virtual pads report the real pad's type here */
    if (!SDL_GetGamepadSteamHandle(gp)) {
        const SDL_GamepadType r = SDL_GetRealGamepadType(gp);
        if (r != SDL_GAMEPAD_TYPE_UNKNOWN) t = r;
    }
    switch (t) {
    case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5:
        return PADX_TYPE_PS;
    case SDL_GAMEPAD_TYPE_XBOX360: case SDL_GAMEPAD_TYPE_XBOXONE:
        return PADX_TYPE_XBOX;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
    case SDL_GAMEPAD_TYPE_GAMECUBE:
        return PADX_TYPE_NINTENDO;
    default: break;
    }
    if (vid == 0x054C) return PADX_TYPE_PS;
    if (vid == 0x057E) return PADX_TYPE_NINTENDO;
    if (vid == 0x045E) return PADX_TYPE_XBOX;
    return PADX_TYPE_OTHER;
}
static const char* padx_class_name(int t)
{
    return t == PADX_TYPE_PS ? "PlayStation" : t == PADX_TYPE_XBOX ? "Xbox" :
           t == PADX_TYPE_NINTENDO ? "Nintendo" : "generic";
}

static int padx_used_slot(int s)
{
    for (int i = 0; i < s_ndev; i++) if (s_dev[i].slot == s) return 1;
    return 0;
}
static void padx_give_slot(PadDev* d)
{
    int used[PADX_SLOTS];
    for (int s = 0; s < PADX_SLOTS; s++) used[s] = padx_used_slot(s);
    d->slot = padl_lowest_free(used, PADX_SLOTS);
    d->state = 1;
    d->led_set = 0;
    if (d->slot >= 0)
        printf("[pad-sdl3] controller %d: %s (%s, %04x:%04x%s)\n", d->slot + 1, d->name,
               padx_class_name(d->type), d->vid, d->pid, d->has_gyro ? ", gyro" : "");
    else
        printf("[pad-sdl3] %s: no free controller slot (max %d)\n", d->name, PADX_SLOTS);
    fflush(stdout);
}
static int padx_any_real(const PadDev* except)
{
    for (int i = 0; i < s_ndev; i++) if (&s_dev[i] != except && s_dev[i].real_ref) return 1;
    return 0;
}

static void padx_sensors(PadDev* d, int on)
{
    if (!d->has_gyro || d->sensors_on == on) return;
    SDL_SetGamepadSensorEnabled(d->gp, d->gyro_s, on);
    if (d->has_accel) SDL_SetGamepadSensorEnabled(d->gp, d->accel_s, on);
    d->sensors_on = on;
    padg_reset(&d->g, s_cfg_cal);
    d->last_gyro_ns = 0;
}

static void padx_open(SDL_JoystickID id)
{
    for (int i = 0; i < s_ndev; i++) if (s_dev[i].id == id) return;
    if (s_ndev >= PADX_MAX_DEV) return;
    SDL_Gamepad* gp = SDL_OpenGamepad(id);
    if (!gp) { printf("[pad-sdl3] could not open pad %u: %s\n", (unsigned)id, SDL_GetError()); return; }
    PadDev* d = &s_dev[s_ndev++];
    memset(d, 0, sizeof *d);
    d->id = id; d->gp = gp; d->slot = -1; d->led_player = -2;
    d->vid = SDL_GetGamepadVendor(gp); d->pid = SDL_GetGamepadProduct(gp);
    d->type = padx_class_of(gp, d->vid);
    const char* nm = SDL_GetGamepadName(gp);
    snprintf(d->name, sizeof d->name, "%s", nm ? nm : "Controller");
    d->cand = padd_virtual_candidate(d->vid, d->pid) && !SDL_GetGamepadSteamHandle(gp);
    d->real_ref = !d->cand && (d->type == PADX_TYPE_PS || d->type == PADX_TYPE_NINTENDO);
    static const SDL_SensorType gs[3] = { SDL_SENSOR_GYRO, SDL_SENSOR_GYRO_R, SDL_SENSOR_GYRO_L };
    static const SDL_SensorType as[3] = { SDL_SENSOR_ACCEL, SDL_SENSOR_ACCEL_R, SDL_SENSOR_ACCEL_L };
    for (int k = 0; k < 3; k++) if (SDL_GamepadHasSensor(gp, gs[k])) {
        d->has_gyro = 1; d->gyro_s = gs[k]; d->accel_s = as[k];
        d->has_accel = SDL_GamepadHasSensor(gp, as[k]);
        break;
    }
    if (s_dedup && d->cand && padx_any_real(d)) {
        printf("[pad-sdl3] %s (%04x:%04x) appeared next to a PlayStation/Nintendo pad: "
               "it joins once it is used on its own (PAD_DEDUP)\n", d->name, d->vid, d->pid);
        fflush(stdout);
    } else {
        padx_give_slot(d);
    }
    if (s_dedup && d->real_ref)   /* a wrapper's copy may already have a slot: watch it */
        for (int i = 0; i < s_ndev; i++) if (s_dev[i].cand && s_dev[i].state == 1) {
            s_dev[i].recheck = 1; memset(s_dev[i].pair, 0, sizeof s_dev[i].pair);
        }
    if (s_cfg_gyro) padx_sensors(d, 1);
}

static void padx_close_at(int i)
{
    PadDev* d = &s_dev[i];
    if (d->slot >= 0) printf("[pad-sdl3] controller %d disconnected (%s)\n", d->slot + 1, d->name);
    else if (d->state == 2) printf("[pad-sdl3] duplicate %s removed\n", d->name);
    fflush(stdout);
    SDL_CloseGamepad(d->gp);
    for (int j = i; j < s_ndev - 1; j++) s_dev[j] = s_dev[j + 1];
    s_ndev--;
    for (int j = 0; j < s_ndev; j++) {   /* pair tables are indexed by device */
        memmove(&s_dev[j].pair[i], &s_dev[j].pair[i + 1], sizeof(PadPair) * (PADX_MAX_DEV - 1 - i));
        memset(&s_dev[j].pair[PADX_MAX_DEV - 1], 0, sizeof(PadPair));
    }
}
static PadDev* padx_find(SDL_JoystickID id)
{
    for (int i = 0; i < s_ndev; i++) if (s_dev[i].id == id) return &s_dev[i];
    return NULL;
}

static void padx_read(PadDev* d)
{
    SDL_Gamepad* gp = d->gp;
    unsigned short b = 0;
    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_SOUTH)) b |= PADX_A;
    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_EAST)) b |= PADX_B;
    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_WEST)) b |= PADX_X;
    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_NORTH)) b |= PADX_Y;
    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_BACK)) b |= PADX_BACK;
    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_TOUCHPAD)) b |= PADX_BACK;   /* DS4 / DualSense: Select */
    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_START)) b |= PADX_START;
    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_LEFT_STICK)) b |= PADX_LTHUMB;
    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_RIGHT_STICK)) b |= PADX_RTHUMB;
    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) b |= PADX_LB;
    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) b |= PADX_RB;
    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_UP)) b |= PADX_UP;
    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) b |= PADX_DOWN;
    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) b |= PADX_LEFT;
    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) b |= PADX_RIGHT;
    if (d->type == PADX_TYPE_NINTENDO && s_nswap) b = padl_nintendo_swap(b);
    PadxState* s = &d->st;
    s->connected = 1;
    s->buttons = b;
    const int lt = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    const int rt = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    s->lt = (unsigned char)(lt <= 0 ? 0 : lt >= 32767 ? 255 : lt * 255 / 32767);
    s->rt = (unsigned char)(rt <= 0 ? 0 : rt >= 32767 ? 255 : rt * 255 / 32767);
    const int ly = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY);
    const int ry = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTY);
    s->lx = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX);
    s->rx = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTX);
    s->ly = (short)(ly <= -32767 ? 32767 : -ly);    /* SDL: + = down; XInput: + = up */
    s->ry = (short)(ry <= -32767 ? 32767 : -ry);
    s->type = d->type;
    s->has_gyro = d->has_gyro;
    s->vid = d->vid; s->pid = d->pid;
    memcpy(s->name, d->name, sizeof s->name);
    d->sig = padd_sig(b, s->lx, s->ly, s->rx, s->ry, s->lt, s->rt);
}

/* Duplicate detection (see pad_logic.h). */
static void padx_dedup(void)
{
    for (int i = 0; i < s_ndev; i++) if (s_dev[i].real_ref) padd_hist_push(&s_dev[i].hist, s_dev[i].sig);
    for (int i = 0; i < s_ndev; i++) {
        PadDev* c = &s_dev[i];
        if (!c->cand || !(c->state == 0 || (c->state == 1 && c->recheck))) continue;
        if (!s_dedup || !padx_any_real(c)) {
            if (c->state == 0) padx_give_slot(c);
            c->recheck = 0;
            continue;
        }
        int ghost = 0, distinct = 1;
        for (int j = 0; j < s_ndev; j++) {
            if (j == i || !s_dev[j].real_ref) continue;
            const int v = padd_update(&c->pair[j], c->sig, &s_dev[j].hist);
            if (v > 0) ghost = 1;
            if (v >= 0) distinct = 0;
        }
        if (ghost) {
            printf("[pad-sdl3] %s (%04x:%04x) mirrors a PlayStation/Nintendo pad: ignored as a duplicate "
                   "(DS4Windows / a virtual pad; PAD_DEDUP=0 keeps it)\n", c->name, c->vid, c->pid);
            fflush(stdout);
            c->state = 2; c->slot = -1; c->recheck = 0;
            if (c->has_gyro) padx_sensors(c, 0);
        } else if (distinct) {
            c->recheck = 0;
            if (c->state == 0) padx_give_slot(c);
        }
    }
}

static void padx_on_sensor(const SDL_GamepadSensorEvent* e)
{
    PadDev* d = padx_find(e->which);
    if (!d || !d->sensors_on) return;
    const Uint64 ns = e->sensor_timestamp ? e->sensor_timestamp : e->timestamp;
    const double t = (double)ns * 1e-9;
    if (e->sensor == d->accel_s) { padg_accel(&d->g, t, e->data); return; }
    if (e->sensor != d->gyro_s) return;
    PadGyroCfg c = { s_cfg_sens, s_cfg_inv, s_cfg_space, s_cfg_cal };
    double y = 0.0, p = 0.0;
    const int out = padg_gyro(&d->g, &c, t, e->data, &y, &p);
    d->last_gyro_ns = SDL_GetTicksNS();
    if (d->slot < 0) return;
    AcquireSRWLockExclusive(&s_lock);
    if (out) { s_gy_yaw[d->slot] += y; s_gy_pitch[d->slot] += p; }
    s_gy_ryaw[d->slot] = d->g.rate_yaw; s_gy_rpitch[d->slot] = d->g.rate_pitch;
    s_gy_t[d->slot] = d->last_gyro_ns;
    s_gy_cal[d->slot] = d->g.calibrating;
    ReleaseSRWLockExclusive(&s_lock);
    { static int said[PADX_SLOTS];
      const int st = d->g.calibrating ? 0 : d->g.calibrated;
      if (st != said[d->slot]) {
          said[d->slot] = st;
          if (st == 1 && s_cfg_cal) printf("[pad-sdl3] controller %d: gyro calibrated (bias %.2f %.2f %.2f deg/s)\n", d->slot + 1,
                              d->g.bias[0] * PADG_DEG, d->g.bias[1] * PADG_DEG, d->g.bias[2] * PADG_DEG);
          else if (st == -1) printf("[pad-sdl3] controller %d: gyro calibration timed out (pad kept moving); "
                                    "using no bias\n", d->slot + 1);
          fflush(stdout);
      } }
}

static void padx_apply_outputs(void)
{
    static const Uint8 col[4][3] = { { 0, 48, 255 }, { 255, 0, 0 }, { 0, 220, 0 }, { 255, 0, 160 } };
    const Uint64 now = SDL_GetTicksNS();
    const int polling = (LONG64)GetTickCount64() - s_poll_ms < 1000;
    for (int i = 0; i < s_ndev; i++) {
        PadDev* d = &s_dev[i];
        if (d->slot < 0) continue;
        const int sl = d->slot;
        AcquireSRWLockShared(&s_lock);
        unsigned short lo = s_req_lo[sl], hi = s_req_hi[sl];
        const int pl = s_req_player[sl], bar = s_req_bar[sl];
        ReleaseSRWLockShared(&s_lock);
        if (!polling) lo = hi = 0;
        if (lo != d->rum_lo || hi != d->rum_hi || ((lo | hi) && now - d->rum_ns > 400000000ull)) {
            SDL_RumbleGamepad(d->gp, lo, hi, (lo | hi) ? 1000 : 0);
            d->rum_lo = lo; d->rum_hi = hi; d->rum_ns = now;
        }
        if (!d->led_set || pl != d->led_player || bar != d->led_bar) {
            d->led_set = 1; d->led_player = pl; d->led_bar = bar;
            SDL_SetGamepadPlayerIndex(d->gp, pl);
            if (!bar) SDL_SetGamepadLED(d->gp, 0, 0, 0);
            else if (pl >= 0) SDL_SetGamepadLED(d->gp, col[pl & 3][0], col[pl & 3][1], col[pl & 3][2]);
            else SDL_SetGamepadLED(d->gp, 24, 24, 24);
        }
    }
}

static DWORD WINAPI padx_thread(LPVOID arg)
{
    (void)arg;
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_GAMEINPUT, "0");          /* XInput + HIDAPI, as tested */
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_PLAYER_LED, "1");
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    SDL_SetHint(SDL_HINT_APP_NAME, "ps3recomp");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        printf("[pad-sdl3] SDL_Init failed: %s\n", SDL_GetError());
        fflush(stdout);
        s_state = -1;
        SetEvent(s_ready);
        return 0;
    }
    static const Uint32 quiet[] = {
        SDL_EVENT_JOYSTICK_AXIS_MOTION, SDL_EVENT_JOYSTICK_BALL_MOTION, SDL_EVENT_JOYSTICK_HAT_MOTION,
        SDL_EVENT_JOYSTICK_BUTTON_DOWN, SDL_EVENT_JOYSTICK_BUTTON_UP, SDL_EVENT_JOYSTICK_BATTERY_UPDATED,
        SDL_EVENT_GAMEPAD_AXIS_MOTION, SDL_EVENT_GAMEPAD_BUTTON_DOWN, SDL_EVENT_GAMEPAD_BUTTON_UP,
        SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN, SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION, SDL_EVENT_GAMEPAD_TOUCHPAD_UP };
    for (unsigned k = 0; k < sizeof quiet / sizeof quiet[0]; k++) SDL_SetEventEnabled(quiet[k], false);
    { int n = 0; SDL_JoystickID* ids = SDL_GetGamepads(&n);
      for (int k = 0; ids && k < n; k++) {   /* real pads first, so a wrapper's copy finds its pad */
          const SDL_GamepadType t = SDL_GetRealGamepadTypeForID(ids[k]);
          const Uint16 v = SDL_GetGamepadVendorForID(ids[k]);
          if (v == 0x054C || v == 0x057E || t == SDL_GAMEPAD_TYPE_PS4 || t == SDL_GAMEPAD_TYPE_PS5 ||
              t == SDL_GAMEPAD_TYPE_PS3 || t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO ||
              t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR) padx_open(ids[k]);
      }
      for (int k = 0; ids && k < n; k++) padx_open(ids[k]);
      SDL_free(ids); }
    { const char* v = SDL_GetRevision();
      printf("[pad-sdl3] SDL %d.%d.%d (%s) ready, %d controller(s)\n", SDL_VERSIONNUM_MAJOR(SDL_GetVersion()),
             SDL_VERSIONNUM_MINOR(SDL_GetVersion()), SDL_VERSIONNUM_MICRO(SDL_GetVersion()), v ? v : "", s_ndev);
      fflush(stdout); }
    s_state = 1;
    SetEvent(s_ready);

    int gyro_was = 0;
    while (!s_quit) {
        SDL_Event ev;
        SDL_PumpEvents();
        while (SDL_PeepEvents(&ev, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) > 0) {
            if (ev.type == SDL_EVENT_GAMEPAD_ADDED) padx_open(ev.gdevice.which);
            else if (ev.type == SDL_EVENT_GAMEPAD_REMOVED) {
                for (int i = 0; i < s_ndev; i++) if (s_dev[i].id == ev.gdevice.which) { padx_close_at(i); break; }
            } else if (ev.type == SDL_EVENT_GAMEPAD_SENSOR_UPDATE) padx_on_sensor(&ev.gsensor);
        }
        const int gy = s_cfg_gyro;
        if (gy != gyro_was || s_cfg_recal) {
            s_cfg_recal = 0;
            for (int i = 0; i < s_ndev; i++) {
                if (s_dev[i].state == 2) continue;
                if (gy && gyro_was) { padg_reset(&s_dev[i].g, s_cfg_cal); continue; }   /* recalibrate */
                padx_sensors(&s_dev[i], gy);
            }
            gyro_was = gy;
        }
        for (int i = 0; i < s_ndev; i++) padx_read(&s_dev[i]);
        padx_dedup();
        AcquireSRWLockExclusive(&s_lock);
        memset(s_snap, 0, sizeof s_snap);
        for (int i = 0; i < s_ndev; i++) if (s_dev[i].slot >= 0 && s_dev[i].slot < PADX_SLOTS) s_snap[s_dev[i].slot] = s_dev[i].st;
        for (int s = 0; s < PADX_SLOTS; s++) if (!s_snap[s].connected) {
            s_gy_yaw[s] = s_gy_pitch[s] = s_gy_ryaw[s] = s_gy_rpitch[s] = 0.0; s_gy_t[s] = 0;
        }
        ReleaseSRWLockExclusive(&s_lock);
        padx_apply_outputs();
        SDL_Delay(1);
    }
    while (s_ndev > 0) padx_close_at(s_ndev - 1);
    SDL_Quit();
    return 0;
}

int padx_init(void)
{
    if (s_state) return s_state > 0;
    { const char* e = getenv("PAD_DEDUP"); if (e && *e == '0') s_dedup = 0; }
    { const char* e = getenv("PAD_NINTENDO_SWAP"); if (e && *e && *e != '0') s_nswap = 1; }
    for (int s = 0; s < PADX_SLOTS; s++) { s_req_player[s] = -1; s_req_bar[s] = 1; }
    s_ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    s_thread = CreateThread(NULL, 256 * 1024, padx_thread, NULL, 0, NULL);
    if (!s_thread) { s_state = -1; return 0; }
    if (WaitForSingleObject(s_ready, 8000) != WAIT_OBJECT_0) {
        printf("[pad-sdl3] SDL3 did not start in time\n");
        s_quit = 1; s_state = -1;
        return 0;
    }
    return s_state > 0;
}
int padx_active(void) { return s_state > 0; }
void padx_shutdown(void)
{
    /* SDL stays up for the life of the process (cellPadEnd / Init may repeat). */
}
void padx_snapshot(PadxState* out)
{
    s_poll_ms = (LONG64)GetTickCount64();
    padx_peek(out);
}
void padx_peek(PadxState* out)
{
    AcquireSRWLockShared(&s_lock);
    memcpy(out, s_snap, sizeof s_snap);
    ReleaseSRWLockShared(&s_lock);
}
void padx_set_rumble(int slot, unsigned short lo, unsigned short hi)
{
    if (slot < 0 || slot >= PADX_SLOTS) return;
    AcquireSRWLockExclusive(&s_lock);
    s_req_lo[slot] = lo; s_req_hi[slot] = hi;
    ReleaseSRWLockExclusive(&s_lock);
}
void padx_set_player(int slot, int player, int lightbar)
{
    if (slot < 0 || slot >= PADX_SLOTS) return;
    if (s_req_player[slot] == player && s_req_bar[slot] == (lightbar ? 1 : 0)) return;
    AcquireSRWLockExclusive(&s_lock);
    s_req_player[slot] = player; s_req_bar[slot] = lightbar ? 1 : 0;
    ReleaseSRWLockExclusive(&s_lock);
}
void padx_gyro_config(int enabled, double sens, int invert_y, int player_space, int calibrate)
{
    s_cfg_sens = sens > 0.0 ? sens : 2.0;
    s_cfg_inv = invert_y; s_cfg_space = player_space; s_cfg_cal = calibrate;
    s_cfg_gyro = enabled ? 1 : 0;
}
void padx_set_nintendo_swap(int on) { s_nswap = on ? 1 : 0; }
int padx_gyro_take(int slot, double* yaw_deg, double* pitch_deg)
{
    *yaw_deg = *pitch_deg = 0.0;
    if (slot < 0 || slot >= PADX_SLOTS) return 0;
    AcquireSRWLockExclusive(&s_lock);
    *yaw_deg = s_gy_yaw[slot]; *pitch_deg = s_gy_pitch[slot];
    s_gy_yaw[slot] = s_gy_pitch[slot] = 0.0;
    const int ok = s_gy_t[slot] != 0;
    ReleaseSRWLockExclusive(&s_lock);
    return ok;
}
int padx_gyro_rate(int slot, double* yaw_dps, double* pitch_dps)
{
    *yaw_dps = *pitch_dps = 0.0;
    if (slot < 0 || slot >= PADX_SLOTS) return 0;
    AcquireSRWLockShared(&s_lock);
    const Uint64 t = s_gy_t[slot];
    if (t && SDL_GetTicksNS() - t < 60000000ull) { *yaw_dps = s_gy_ryaw[slot]; *pitch_dps = s_gy_rpitch[slot]; }
    ReleaseSRWLockShared(&s_lock);
    return t != 0;
}
int padx_gyro_calibrating(int slot)
{
    return slot >= 0 && slot < PADX_SLOTS && s_gy_cal[slot];
}
void padx_gyro_recalibrate(void) { s_cfg_recal = 1; }

#endif /* _WIN32 && PS3RECOMP_PAD_SDL3 */
