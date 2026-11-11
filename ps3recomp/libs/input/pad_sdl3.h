/*
 * ps3recomp - SDL3 gamepad backend (Windows PC ports)
 *
 * SDL3 runs on its own input thread (init, events, rumble, light bar); the
 * pad emulation (cellPad.c) reads a snapshot. Pads are given "slots" 0..7:
 * the lowest free one when they connect, kept while they stay connected.
 */
#ifndef PS3RECOMP_PAD_SDL3_H
#define PS3RECOMP_PAD_SDL3_H

#ifdef __cplusplus
extern "C" {
#endif

#define PADX_SLOTS 8

enum { PADX_TYPE_NONE = 0, PADX_TYPE_XBOX, PADX_TYPE_PS, PADX_TYPE_NINTENDO, PADX_TYPE_OTHER };

typedef struct {
    int            connected;
    unsigned short buttons;        /* XINPUT_GAMEPAD_* bits, Nintendo swap applied */
    unsigned char  lt, rt;
    short          lx, ly, rx, ry; /* XInput convention: +y = up */
    int            type;           /* PADX_TYPE_* (prompt family) */
    int            has_gyro;
    unsigned short vid, pid;
    char           name[48];
} PadxState;

/* Starts the input thread and SDL3. 1 = ready, 0 = SDL3 unusable (caller falls back). */
int  padx_init(void);
int  padx_active(void);
void padx_shutdown(void);
/* Copies PADX_SLOTS states. Also tells the thread the game is still polling (rumble keep-alive). */
void padx_snapshot(PadxState* out);
/* The same without the keep-alive (PC menus, which run even when the game hangs). */
void padx_peek(PadxState* out);
/* Rumble per slot, 0..65535 each (low = large motor). */
void padx_set_rumble(int slot, unsigned short lo, unsigned short hi);
/* Player number for a slot (0-based, -1 = not playing): player LEDs, and the
 * light bar in the player's colour unless lightbar is 0. */
void padx_set_player(int slot, int player, int lightbar);
/* Gyro. mode 0 = sensors off. Settings can change at any time. */
void padx_gyro_config(int enabled, double sens, int invert_y, int player_space, int calibrate);
/* Camera turn since the last call, degrees (mouse convention: + = right / down). */
int  padx_gyro_take(int slot, double* yaw_deg, double* pitch_deg);
/* Current smoothed turn rate, deg/s (0 if no recent samples). */
int  padx_gyro_rate(int slot, double* yaw_dps, double* pitch_dps);
/* 1 while the slot's gyro is still learning its resting bias. */
int  padx_gyro_calibrating(int slot);
/* Start calibrating every pad's gyro again (keep them still for a second). */
void padx_gyro_recalibrate(void);
/* PAD_NINTENDO_SWAP: Nintendo pads confirm with A (east) instead of the bottom button. */
void padx_set_nintendo_swap(int on);

#ifdef __cplusplus
}
#endif
#endif
