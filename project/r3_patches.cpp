// Resistance 3: community RPCS3 patches (patch.yml), ported as guest-memory
// data patches applied once at boot, after the EBOOT is loaded and before the
// game's entry runs -- the moment RPCS3 applies them.
//
// Only disc EBOOT 1.00 is supported: BCUS98176 / BCES01118 01.00, PPU hashes
// 502feb2351d2b637a2704d5a50cb44bc1b781532 (US) and
// 2701cf372814d59f26e771dc9394323d4f799682 (EU), which share every address.
// Every write checks the original value first and is skipped (logged) on a
// mismatch, so another EBOOT (e.g. 1.05) never gets patched at the wrong place.
//
// Code patches (instruction edits) cannot be byte-patched: the PPU code is
// recompiled. Those live as hooks in tools/hooks/r3.json (see README,
// "RPCS3 patches").
//
//   Unlock FPS (Whatcookie, boma), tied to R2_FPS:
//     be32 0x00F02AD4 0x38D1B717   0.001 s frame-time floor -> 0.0001 (here,
//                                  and again by the limiter hook)
//     be32 0x007EEAC8 lfs ...      the game clock's 1/75 lower clamp: the
//                                  r3_dt_floor / r3_dt_ceil hooks in
//                                  func_007EEA44 (tools/hooks/r3.json)
//     be32 0x00736C44 nop          the limiter's wait: the r2_frame_period
//                                  hook in func_00736BAC
//   R2_PATCH_FPS_FLOORS=1  applies the floor at 30 fps too.
//   R2_PATCH_FOV=1         "Extended FOV" (illusion), off by default.
//                          R2_PATCH_FOV_VALUE=<float 0.3..1.0> overrides 0.7
//                          (and turns it on by itself unless R2_PATCH_FOV=0).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "ps3emu/ps3types.h"
#include "ppu_recomp.h"

extern "C" double r2_fps_target(void);   /* stubs.cpp: 30 = the game's own limiter */

namespace {

/* -1 unset, 0 off, 1 on */
int env_flag(const char* name)
{
    const char* e = getenv(name);
    if (!e || !*e) return -1;
    if (*e == '0' || !_stricmp(e, "off") || !_stricmp(e, "false") || !_stricmp(e, "no")) return 0;
    return 1;
}

uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* Write `val` at `ea` if it still holds `orig`. */
bool poke32(const char* patch, uint32_t ea, uint32_t orig, uint32_t val)
{
    const uint32_t cur = vm_read32(ea);
    if (cur == val) return true;
    if (cur != orig) {
        fprintf(stderr, "[patch] %s: 0x%08X holds %08X, expected %08X -- skipped (not EBOOT 1.00?)\n",
                patch, ea, cur, orig);
        return false;
    }
    vm_write32(ea, val);
    return true;
}

} // namespace

extern "C" void r3_patches_apply(void)
{
    /* Unlock FPS: the 0.001 s floor. Written here so nothing reads it before
     * the first limiter call; the limiter hook writes it again if needed. */
    if (r2_fps_target() != 30.0 || env_flag("R2_PATCH_FPS_FLOORS") == 1) {
        const bool ok = poke32("fps_floor", 0x00F02AD4u, 0x3A83126Fu, 0x38D1B717u);
        fprintf(stderr, "[patch] Unlock FPS frame-time floor: %s\n", ok ? "on" : "skipped");
    } else {
        fprintf(stderr, "[patch] Unlock FPS: off (R2_FPS=30, the game's own limiter)\n");
    }

    /* Extended FOV (illusion): bef32 0x00F0608C 1.0 -> 0.7. Same kind of edit
     * as the R2 patch (the constant term of the projection's tangent
     * approximation); on R2 it misaligns shadows, so it stays opt-in. */
    /* on with R2_PATCH_FOV=1, or a R2_PATCH_FOV_VALUE alone (the launcher's
     * "Field of view" setting); R2_PATCH_FOV=0 keeps it off either way */
    const int fov_flag = env_flag("R2_PATCH_FOV");
    const char* fov_val = getenv("R2_PATCH_FOV_VALUE");
    if (fov_flag == 1 || (fov_flag != 0 && fov_val && *fov_val)) {
        float v = 0.7f;
        if (const char* e = getenv("R2_PATCH_FOV_VALUE")) {
            const float x = (float)atof(e);
            if (x >= 0.3f && x <= 1.0f) v = x;
            else fprintf(stderr, "[patch] R2_PATCH_FOV_VALUE=%s out of range 0.3..1.0, using 0.7\n", e);
        }
        if (poke32("fov", 0x00F0608Cu, 0x3F800000u, f2u(v)))
            fprintf(stderr, "[patch] Extended FOV: on (%.3f)\n", u2f(vm_read32(0x00F0608Cu)));
    } else {
        fprintf(stderr, "[patch] Extended FOV (R2_PATCH_FOV): off\n");
    }
}
