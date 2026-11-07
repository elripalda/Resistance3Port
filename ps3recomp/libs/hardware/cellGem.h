/*
 * ps3recomp - cellGem HLE
 *
 * PlayStation Move motion controller. Stub — reports no Move attached.
 * Games handle gracefully with controller selection UI.
 */

#ifndef PS3RECOMP_CELL_GEM_H
#define PS3RECOMP_CELL_GEM_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes (SDK libgem / RPCS3 cellGem.h) */
#define CELL_GEM_ERROR_NOT_INITIALIZED     0x80121803   /* UNINITIALIZED */
#define CELL_GEM_ERROR_ALREADY_INITIALIZED 0x80121802
#define CELL_GEM_ERROR_INVALID_ARGUMENT    0x80121804   /* INVALID_PARAMETER */

/* Runtime statuses: positive return values, not errors */
#define CELL_GEM_NOT_CONNECTED             1
#define CELL_GEM_NO_EXTERNAL_PORT_DEVICE   9

/* Constants */
#define CELL_GEM_MAX_NUM   4

/* CellGemInfo status values */
#define CELL_GEM_STATUS_DISCONNECTED   0
#define CELL_GEM_STATUS_READY          1

/* Types. Layouts verified against the DWARF in the Gears of War 3 / og-LBP debug
 * builds (dwarf_abi.py) -- the previous definitions were wrong on every one:
 * CellGemState was 58 bytes vs the real 192, CellGemInfo.port was u16 not u32,
 * and CellGemAttribute was missing memoryPtr (and used a host void* where the
 * guest ABI has a 4-byte EA). Pointer fields are guest EAs (u32), per the
 * u32-guest-address convention used across the other structs. */
typedef struct CellGemAttribute {
    u32 version;
    u32 maxConnect;
    u32 memoryPtr;      /* guest EA */
    u32 spursAddr;      /* guest EA (CellSpurs*) */
    u8  spu[8];         /* spu_priorities */
} CellGemAttribute;     /* 24 bytes */

typedef struct CellGemInfo {
    u32 maxConnect;
    u32 nowConnect;
    u32 status[CELL_GEM_MAX_NUM];
    u32 port[CELL_GEM_MAX_NUM];
} CellGemInfo;          /* 40 bytes */

typedef struct CellGemPadData {
    u16 digitalButtons;
    u16 analogT;
} CellGemPadData;       /* 4 bytes */

typedef struct CellGemExtPortData {
    u16 status;
    u16 digital1;
    u16 digital2;
    u16 analogRightX;
    u16 analogRightY;
    u16 analogLeftX;
    u16 analogLeftY;
    u8  custom[6];
} CellGemExtPortData;   /* 20 bytes */

typedef struct CellGemState {
    float pos[4];          /* +0x00 */
    float vel[4];          /* +0x10 */
    float accel[4];        /* +0x20 */
    float quat[4];         /* +0x30 (quaternion orientation) */
    float angvel[4];       /* +0x40 */
    float angaccel[4];     /* +0x50 */
    float handlePos[4];    /* +0x60 */
    float handleVel[4];    /* +0x70 */
    float handleAccel[4];  /* +0x80 */
    CellGemPadData     pad;         /* +0x90 */
    CellGemExtPortData ext;         /* +0x94 */
    u64   timestamp;       /* +0xA8 (system_time_t) */
    float temperature;     /* +0xB0 */
    float cameraPitchAngle;/* +0xB4 */
    u32   trackingFlags;   /* +0xB8 */
} CellGemState;            /* 192 bytes */

/* Functions */
s32 cellGemInit(const CellGemAttribute* attr);
s32 cellGemEnd(void);

s32 cellGemGetInfo(CellGemInfo* info);
s32 cellGemGetState(u32 gemNum, u32 flag, u64 timestamp, CellGemState* state);

s32 cellGemIsTrackableHue(u32 hue);
s32 cellGemGetHue(u32 gemNum, u32* hue);
s32 cellGemTrackHues(const u32* reqHues, u32* resHues);

s32 cellGemCalibrate(u32 gemNum);
s32 cellGemGetStatusFlags(u32 gemNum, u64* flags);

s32 cellGemUpdateStart(const void* cameraFrame, u64 timestamp);
s32 cellGemUpdateFinish(void);

s32 cellGemReset(u32 gemNum);
s32 cellGemEnableMagnetometer(u32 gemNum, s32 enable);
s32 cellGemEnableCameraPitchAngleCorrection(s32 enable);

/* Per-controller queries the game imports; with no Move connected they all
 * answer CELL_GEM_NOT_CONNECTED (an unregistered import would return 0 = OK). */
s32 cellGemGetImageState(u32 gemNum, void* state);
s32 cellGemGetInertialState(u32 gemNum, u32 flag, u64 timestamp, void* state);
s32 cellGemInvalidateCalibration(s32 gemNum);
s32 cellGemReadExternalPortDeviceInfo(u32 gemNum, u32* extId, u8* data);
s32 cellGemSetRumble(u32 gemNum, u8 rumble);
s32 cellGemSetYaw(u32 gemNum, void* zDirection);
s32 cellGemClearStatusFlags(u32 gemNum, u64 mask);
s32 cellGemPrepareCamera(s32 maxExposure, u32 imageQuality);
s32 cellGemPrepareVideoConvert(const void* attr);
s32 cellGemConvertVideoStart(const void* videoFrame);
s32 cellGemConvertVideoFinish(void);
#ifdef __cplusplus
}
#endif

s32 cellGemGetMemorySize(s32 max_connect);

#endif /* PS3RECOMP_CELL_GEM_H */
