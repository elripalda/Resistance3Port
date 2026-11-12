// Resistance 3 -- PC port launcher.
//
// One window: a sidebar (Display / Graphics / Upscaling & Frame Gen / Performance /
// Controls / Audio / Interface & Notifications / Advanced / Trophies / Saves & tools /
// About + Play) and a page of settings drawn by hand with GDI+ (no stock controls, so it looks the
// same everywhere and scales with the Windows DPI setting).
//
// Everything is saved the moment it changes:
//   settings.ini  -- the launcher's own choices
//   settings.bat  -- the same choices as `set` lines; run.bat calls it
//   controls.txt  -- keyboard & mouse bindings, read by the game at start
// so run.bat keeps working with or without the launcher.
//
// Build (build.bat does this):
//   clang-cl /nologo /O2 /EHsc project\launcher.cpp /Fe:Launcher.exe /link /SUBSYSTEM:WINDOWS
//            user32.lib gdi32.lib shell32.lib gdiplus.lib dwmapi.lib

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <string>
#include <vector>
#include <functional>

#include "r2_autoconfig.h"
#include "r3_trophy_data.h"
#include <shlobj.h>
#include <tlhelp32.h>
#include <intrin.h>
#include <immintrin.h>

#ifdef _MSC_VER
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#endif

using namespace Gdiplus;

// ===========================================================================
// Settings (settings.ini / settings.bat)
// ===========================================================================
struct Choice { const char* label; const char* value; };
// Where a data-driven option (add_option) is listed. The built-in settings are placed by layout() itself.
enum Section { SEC_NONE, SEC_GAME_QUALITY, SEC_RENDER_QUALITY, SEC_PC, SEC_INPUT, SEC_X_DISPLAY, SEC_X_FX, SEC_X_PAD, SEC_SHADERS };
struct Setting {
    const char* key;      // settings.ini key
    const char* title;
    const char* help;     // shown in the help strip while hovered
    std::vector<Choice> choices;
    int def;
    int sel = 0;
    // data-driven options only (add_option): settings.bat writes `set "env=value"` (nothing for ""),
    const char* env = nullptr;
    int section = SEC_NONE;
    const char* only_renderer = nullptr;      // greyed out unless this renderer is selected ("rpcs3" / "vulkan")
    const char* tier_val[4] = {};            // recommended value for the Low/Medium/High/Ultra tier (nullptr = def)
};

enum { S_DISPLAY, S_WINDOW, S_FPS, S_VSYNC, S_RES, S_FSR, S_UPSCALE, S_ANISO, S_FILTER, S_SHARPEN, S_RENDERER, S_CPU,
       S_SHADER, S_COUNT };
static std::vector<Setting> g_set = {
    { "display", "Display mode",
      "Borderless fills the screen and lets you Alt+Tab instantly. Exclusive fullscreen gives the game the "
      "display (lowest latency; Alt+Tab is slower). Windowed uses the window size below. "
      "Alt+Enter switches between windowed and fullscreen in game.",
      { {"Borderless fullscreen", "borderless"}, {"Exclusive fullscreen", "exclusive"}, {"Windowed", "windowed"} }, 0 },
    { "window", "Window size",
      "The size of the game window in Windowed mode. Fullscreen modes always use your screen's resolution.",
      { {"1280 x 720", "1280x720"}, {"1600 x 900", "1600x900"}, {"1920 x 1080", "1920x1080"},
        {"2560 x 1440", "2560x1440"}, {"3840 x 2160", "3840x2160"} }, 2 },
    { "fps", "Frame rate",
      "The PS3 game runs at 30 fps. 60 fps doubles how often the game updates and draws; the game measures "
      "real time each frame, so it does not play faster. Needs about twice the CPU and GPU time. "
      "120 / 144 / Match display / Uncapped go further (uncapped: as fast as the PC allows; "
      "best with VSync Auto or Off and a G-Sync / FreeSync monitor).",
      { {"30 fps  (PS3 original)", "30"}, {"60 fps", "60"}, {"120 fps", "120"}, {"144 fps", "144"},
        {"Match display refresh", "refresh"}, {"Just under refresh  (G-Sync / FreeSync)", "vrr"}, {"Uncapped", "0"} }, 1 },
    { "vsync", "VSync",
      "Off is smoothest with a G-Sync / FreeSync monitor and has the lowest input lag. On removes tearing on "
      "other screens. Adaptive syncs when the game keeps up and tears instead of stuttering when it doesn't. "
      "Auto uses On whenever the frame rate is capped at or below your display's refresh (it never waits then, and "
      "it is what G-Sync / FreeSync follow) and Adaptive when uncapped.",
      { {"Off", "0"}, {"On", "1"}, {"Adaptive", "adaptive"}, {"Auto", "auto"} }, 0 },
    { "res", "Render resolution",
      "The resolution the game draws at. Higher is sharper (shadows too) but costs GPU time. "
      "Above your screen's resolution it acts as supersampling: 3840 x 2160 with DLAA on a 1080p screen "
      "looks like NVIDIA DLDSR. With DLSS/FSR upscaling (Quality/Performance) keep it at your screen: "
      "upscaling to more than the screen and scaling back down only costs GPU time. "
      "Match screen picks the largest step that fits the screen (Windowed: the window).",
      { {"1280 x 720  (PS3 original)", "1"}, {"1920 x 1080", "1.5"}, {"2560 x 1440", "2"},
        {"3200 x 1800", "2.5"}, {"3840 x 2160", "3"}, {"Match screen  (auto)", "auto"} }, 4 },
    { "fsr", "Anti-aliasing",
      "Smooths jagged edges and shimmer. NVIDIA DLSS uses NVIDIA's AI model on GeForce RTX cards - the "
      "sharpest and steadiest; it falls back to FSR on other GPUs. AMD FSR 3.1 works on any GPU. "
      "Intel XeSS is Intel's AI upscaler and runs on any GPU (fastest on Intel Arc; experimental, "
      "Compatible renderer only). If DLSS or XeSS can't start, FSR 3.1 is used. Fast motion can ghost slightly.",
      { {"Off  (PS3 original)", "0"}, {"AMD FSR 3.1", "1"}, {"NVIDIA DLSS", "2"},
        {"Intel XeSS  (experimental)", "4"} }, 2 },
    { "upscale", "Upscaling",
      "With DLSS, FSR or XeSS on, draws the 3D scene at a lower resolution and upscales it to the render "
      "resolution - much faster. Native (DLAA) draws it at full resolution. Performance is the "
      "recommended way to reach 3840 x 2160; the HUD always stays at full resolution.",
      { {"Native  (DLAA)", "1"}, {"Quality  (67%)", "0.667"}, {"Balanced  (58%)", "0.58"},
        {"Performance  (50%)", "0.5"} }, 3 },
    { "aniso", "Texture filtering",
      "Keeps floors and walls sharp when seen at an angle. 16x costs almost nothing on a modern GPU.",
      { {"Trilinear  (PS3 original)", "0"}, {"Anisotropic 2x", "2"}, {"Anisotropic 4x", "4"},
        {"Anisotropic 8x", "8"}, {"Anisotropic 16x", "16"} }, 0 },
    { "filter", "Scaling",
      "How the image is resized to fit your screen when the sizes differ. Smooth suits most people; "
      "Sharp pixels gives a crisp, blocky look. AMD FSR 1 upscales spatially and sharpens (Sharpening sets "
      "how much).",
      { {"Smooth", "linear"}, {"Sharp pixels", "point"}, {"AMD FSR 1", "fsr"} }, 0 },
    { "sharpen", "Sharpening",
      "Counteracts the softness of scaling and anti-aliasing. Off when the render resolution matches your "
      "screen and anti-aliasing is off.",
      { {"Off", "0"}, {"Low", "0.25"}, {"Medium", "0.5"}, {"High", "0.8"} }, 1 },
    { "renderer", "Renderer",
      "Compatible draws through RPCS3's PS3 graphics emulation - the most tested. Native Vulkan "
      "(experimental) draws the game's graphics directly on your GPU, which is faster on weaker PCs "
      "but may still show glitches.",
      { {"Compatible  (RPCS3)", "rpcs3"}, {"Native Vulkan  (experimental)", "vulkan"} }, 0 },
    { "cpu", "CPU profile",
      "How the game uses your processor. Auto picks from your core count: Low trims busy-waiting and "
      "helper threads for 4-core CPUs, Mid for 6-core CPUs, High changes nothing.",
      { {"Auto", "auto"}, {"Low  (4 cores)", "low"}, {"Mid  (6 cores)", "mid"}, {"High  (8+ cores)", "high"} }, 0 },
    { "shaders", "Shader compilation",
      "How the Compatible renderer builds the GPU programs it needs. Asynchronous compiles them in the background: "
      "no stutter, but an object can be missing for a moment the first time it appears. Synchronous waits for "
      "each one (brief hitches the first time). Compiled shaders are kept, so either gets smoother as you play.",
      { {"Asynchronous", "async"}, {"Asynchronous + interpreter  (experimental)", "interp"}, {"Synchronous", "sync"} }, 0 },
};

// ---------------------------------------------------------------------------
// Data-driven options: one call per environment switch, shown on the page launcher_cats.inc puts them on
// and written to settings.bat as `set "ENV=value"` (a "" value writes nothing: the game's default).
// tiers: the recommended value for Low / Medium / High / Ultra PCs (nullptr = the default choice).
//
//   add_option(SEC_GAME_QUALITY, "lod", "Level of detail", "Help text.", "R2_LOD",
//              { {"Original", ""}, {"High", "1"}, {"Ultra", "2"} }, 0, { "", "", "1", "2" });
//   add_option(SEC_RENDER_QUALITY, "shadowres", "Shadow resolution", "...", "RSX_SHADOW_SCALE",
//              { {"Original", ""}, {"2x", "2"} }, 0, {}, "rpcs3");
// ---------------------------------------------------------------------------
[[maybe_unused]] static void add_option(int section, const char* key, const char* title, const char* help, const char* env,
                       std::vector<Choice> choices, int def, std::vector<const char*> tiers = {},
                       const char* only_renderer = nullptr)
{
    Setting s{ key, title, help, std::move(choices), def };
    s.env = env; s.section = section; s.only_renderer = only_renderer;
    for (size_t i = 0; i < tiers.size() && i < 4; i++) s.tier_val[i] = tiers[i];
    g_set.push_back(std::move(s));
}
static void register_options()
{
    // GRAPHICS QUALITY (game-side quality patches: LOD, shadows, FOV, post effects) and
    // RENDERER QUALITY (renderer switches) are added here as they land.
    add_option(SEC_GAME_QUALITY, "fov", "Field of view  (experimental)",
               "Widens the view: the community \"Extended FOV\" patch for Resistance 3. Lower numbers show more "
               "of the scene. Experimental: effects and shadows near the screen edges can be slightly off.",
               "R2_PATCH_FOV_VALUE", { {"Original", ""}, {"Wider  (0.85)", "0.85"}, {"Widest  (0.7)", "0.7"} }, 0);
    add_option(SEC_RENDER_QUALITY, "shadowscale", "Shadow resolution",
               "Renders the game's shadow maps at a higher resolution: sharper, steadier shadow edges. Costs GPU time "
               "and video memory.",
               "RSX_SHADOW_SCALE", { {"Original", ""}, {"2x", "2"}, {"4x", "4"} }, 0, {}, "rpcs3");
    add_option(SEC_RENDER_QUALITY, "fxfullres", "Effects resolution",
               "The PS3 draws smoke, particles, glow and blur at half or quarter resolution. Full draws the half-"
               "resolution effects at full resolution; Full (all) also the quarter-resolution ones. Costs GPU time.",
               "RSX_FX_FULLRES", { {"Original", ""}, {"Full", "1"}, {"Full (all)", "2"} }, 0, {}, "rpcs3");
    add_option(SEC_RENDER_QUALITY, "ssao", "Ambient occlusion",
               "Adds soft contact shadows in corners and creases (SSAO). Needs Anti-aliasing off or FSR. "
               "Ambient occlusion is disabled with DLSS in this beta. "
               "Experimental.",
               "RSX_SSAO", { {"Off", ""}, {"On  (experimental)", "1"} }, 0, {}, "rpcs3");
    add_option(SEC_RENDER_QUALITY, "texpack", "Texture pack",
               "Loads replacement textures from mods\\textures (DDS / PNG named after the texture's hash, see Dump "
               "textures). Only the textures in the pack change.",
               "RSX_TEXPACK", { {"Off", ""}, {"On  (mods\\textures)", "1"} }, 0, {}, "rpcs3");
    add_option(SEC_RENDER_QUALITY, "texdump", "Dump textures",
               "Writes every texture the game uploads to texdump\\ (DDS / PNG) for making a texture pack. Slow; "
               "leave off for play.",
               "RSX_TEXDUMP", { {"Off", ""}, {"On", "1"} }, 0, {}, "rpcs3");
    add_option(SEC_RENDER_QUALITY, "objmv", "Object motion vectors",
               "DLSS / FSR: On gives moving characters and objects their own motion. Pros: sharper moving objects, less "
               "ghosting and smearing. Cons: a small GPU cost; cloth and other dynamic meshes only get partial motion. "
               "Off uses camera motion only.",
               "RSX_DLSS_OBJMV", { {"Off", "0"}, {"On", "1"} }, 0, {}, "rpcs3");
    // SHADER CACHE (Performance page): VKGSRender.cpp preload_shaders. Not part of the recommendation.
    add_option(SEC_SHADERS, "precompile", "Compile shaders before playing",
               "On: at start-up the game compiles every shader it has already used on this PC before play begins "
               "(a \"Compiling shaders N / M\" panel shows the progress), so shaders seen before never stutter. Usually a "
               "second or two; longer after a driver update or Rebuild shader cache. Off: only the first few hundred "
               "before play, the rest in the background while you play. Pros of On: no stutter from shaders already "
               "seen or found by the shader scan. Cons: longer start-up (seconds normally, about a minute after a driver "
               "update or Rebuild shader cache).",
               "R2_PRELOAD_BOOT", { {"On", "-1"}, {"Off", ""} }, 0, {}, "rpcs3");
    add_option(SEC_SHADERS, "shaderthreads", "Shader compile threads",
               "How many processor threads compile shaders while the game starts (Compile shaders before playing). "
               "Auto uses all of them but one.",
               "R2_PRELOAD_BOOT_THREADS", { {"Auto  (all cores but one)", ""}, {"2", "2"}, {"4", "4"}, {"6", "6"}, {"8", "8"},
                                            {"12", "12"}, {"16", "16"} }, 0, {}, "rpcs3");
    // r2rsx_bridge.cpp apply_shader_defaults: RPCS3 "Async with Shader Interpreter"
    add_option(SEC_SHADERS, "shaderinterp", "Shader interpreter while compiling",
               "On: a shader that isn't compiled yet is drawn with a slower interpreter until it is ready, so nothing "
               "goes invisible for a moment, but frames are slower while it is active. Off: the object is skipped "
               "for the frames until its shader is compiled. Pros of On: nothing pops in. Cons: slower frames while it is "
               "active and possible DLSS ghosting on those objects. Rarely needed with Compile shaders before playing on.",
               "R2_SHADER_INTERP", { {"Off", ""}, {"On", "1"} }, 0, {}, "rpcs3");
}

// PC-port behaviour (Display / Audio / Interface / Advanced pages, see launcher_cats.inc). Kept apart from register_options() (graphics).
static BOOL CALLBACK pc_mon_enum(HMONITOR m, HDC, LPRECT, LPARAM lp)
{
    auto* out = (std::vector<Choice>*)lp;
    MONITORINFOEXA mi; mi.cbSize = sizeof mi;
    char label[96];
    const int n = (int)out->size();               // 1-based: choice 0 is "Primary"
    if (GetMonitorInfoA(m, &mi))
        snprintf(label, sizeof label, "Monitor %d  (%ldx%ld%s)", n, mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top, (mi.dwFlags & MONITORINFOF_PRIMARY) ? ", primary" : "");
    else snprintf(label, sizeof label, "Monitor %d", n);
    char num[8]; snprintf(num, sizeof num, "%d", n);
    out->push_back({ _strdup(label), _strdup(num) });
    return TRUE;
}
static void register_pc_options()
{
    std::vector<Choice> mons = { {"Primary monitor", ""} };
    EnumDisplayMonitors(NULL, NULL, pc_mon_enum, (LPARAM)&mons);
    add_option(SEC_PC, "monitor", "Monitor",
               "Which screen the game opens on (borderless fullscreen and windowed). Numbered in Windows' order.",
               "RSX_MONITOR", std::move(mons), 0);
    add_option(SEC_PC, "background", "When the game is in the background",
               "What happens when you Alt+Tab away or click another window. Pause freezes the game and mutes it; "
               "Mute keeps it running silently; Keep running changes nothing.",
               "R2_BACKGROUND", { {"Pause and mute", ""}, {"Mute only", "mute"}, {"Keep running", "run"} }, 0);
    // stubs.cpp port_speakers / cellAudio.c audio_buffer_ms (switches that had no launcher option before)
    add_option(SEC_PC, "speakers", "Speaker layout",
               "The channel layout the game mixes for. Windows down-mixes to your real speakers or headphones. "
               "Pros of 5.1 / 7.1: proper surround on a surround system. Cons: on plain stereo Stereo can sound a little "
               "fuller, since nothing is down-mixed.",
               "PORT_SPEAKERS", { {"5.1 surround", ""}, {"7.1 surround", "8"}, {"Stereo", "2"} }, 0);
    add_option(SEC_PC, "audio_buffer", "Audio buffer",
               "How much sound is buffered ahead. Auto is 100 ms (150 ms on CPUs with fewer than 12 threads). "
               "Pros of a larger buffer: no crackles or stretching when the CPU is busy. Cons: sound lags the picture a "
               "little more. Smaller: tighter sound, but may crackle on a busy CPU.",
               "PORT_AUDIO_BUFFER_MS", { {"Auto", ""}, {"80 ms", "80"}, {"100 ms", "100"}, {"150 ms", "150"},
                                         {"200 ms", "200"} }, 0);
    add_option(SEC_PC, "skipintro", "Skip intro movies",
               "Skips the studio logo video and the opening movie at start-up "
               "(the game goes straight to the title screen).",
               "SKIP_INTRO", { {"On", ""}, {"Off", "0"} }, 0);
    add_option(SEC_PC, "menufps", "Frame rate in menus",
               "Caps the frame rate whenever you are not in control of your character (menus, loading screens, "
               "cutscenes) to save power and heat. Gameplay keeps the Frame rate setting.",
               "PORT_MENU_FPS", { {"Same as in game", ""}, {"60 fps", "60"}, {"30 fps", "30"} }, 0);
    add_option(SEC_PC, "padglyphs", "Controller button prompts",
               "Which button names the game shows while you play with a controller. Auto follows player 1's controller (SDL3: PlayStation "
               "symbols for a DualShock / DualSense, Xbox buttons (A B X Y, LB RB...) for Xbox and others, Switch by its layout). "
               "With keyboard and mouse the prompts always show your keys. Keyboard keys shows your keys even "
               "with a controller.",
               "R2_PAD_GLYPHS", { {"Auto", ""}, {"Xbox", "xbox"}, {"PlayStation", "ps"}, {"Keyboard keys", "keys"} }, 0);
    add_option(SEC_PC, "present", "Presentation mode",
               "How finished frames reach the screen. Default follows VSync. Immediate (VRR) never waits - best with "
               "G-Sync / FreeSync and an uncapped or Just under refresh frame rate. Mailbox shows the newest frame "
               "without tearing but at the fixed refresh, so G-Sync / FreeSync do not follow it. FIFO is classic VSync. "
               "Compatible renderer only.",
               "R2_PRESENT", { {"Default  (follow VSync)", ""}, {"Immediate  (VRR / tearing)", "immediate"},
                               {"Mailbox  (no tearing, no VRR)", "mailbox"}, {"FIFO  (VSync)", "fifo"} }, 0, {}, "rpcs3");
    add_option(SEC_PC, "refreshswitch", "Refresh rate switch",
               "Exclusive fullscreen only: when the frame rate does not divide your display's refresh rate evenly (60 fps "
               "on 144 Hz shows frames for 2, then 3 refreshes - judder), switch the display to the nearest refresh rate "
               "that does (144 -> 120 Hz) while the game has focus. Not needed with G-Sync / FreeSync. Experimental: the screen goes black on every switch, which also happens when you alt-tab.",
               "PORT_REFRESH_SWITCH", { {"Off", ""}, {"On", "1"} }, 0, {}, "rpcs3");
    add_option(SEC_PC, "overlay", "Performance overlay",
               "Frame rate, frame time and GPU draw statistics in a corner of the game window. F3 shows or hides it "
               "in game. Not visible in exclusive fullscreen.",
               "R2_OVERLAY", { {"Off  (F3 shows it)", ""}, {"On at start", "1"}, {"Disabled  (F3 does nothing)", "0"} }, 0);
    add_option(SEC_PC, "trophytoast", "Trophy notifications",
               "A small notification in the corner of the game window when you earn a trophy. Trophies are always "
               "recorded (see the Trophies page).",
               "R2_TROPHY_TOAST", { {"On", ""}, {"Off", "0"} }, 0);
    // r3_pcextra.cpp: the "Hold Space / A to skip" hint when a movie starts (skipping itself is "cutscene_skip")
    add_option(SEC_PC, "skiphint", "Show cutscene skip hint",
               "A short \"Hold Space / A to skip\" notification in the corner when a movie starts. Off hides it; "
               "holding the button still skips (Hold to skip movies).",
               "PORT_SKIP_HINT", { {"On", ""}, {"Off", "0"} }, 0);
    add_option(SEC_PC, "mods", "Mods folder",
               "Loads replacement game files from the mods folder next to the game: mods\\<mod name>\\USRDIR\\... "
               "replaces the same file under the game's PS3_GAME folder. A folder named like a .psarc archive replaces "
               "single files inside it, or single level assets by tuid (see docs\\MODDING.md). Pros: no repacking. "
               "Cons: the first start of a modded level takes a moment to build the patched archive (kept in "
               "mods\\.cache). The replaced files are listed in logs\\run.log.",
               "R2_MODS", { {"Off", ""}, {"On", "1"} }, 0);
}
// Controls page
static void register_input_options()
{
    add_option(SEC_INPUT, "msensy", "Vertical mouse sensitivity",
               "Up / down mouse speed relative to left / right (the Sensitivity slider above).",
               "PAD_MOUSE_SENS_Y", { {"Same as horizontal", ""}, {"50%", "0.5"}, {"75%", "0.75"}, {"125%", "1.25"},
                                     {"150%", "1.5"}, {"200%", "2"} }, 0);
    add_option(SEC_INPUT, "adssens", "Aiming sensitivity",
               "Mouse speed while aiming down the sights or through a scope, relative to the hip-fire speed "
               "(on top of the game's own zoom slowdown).",
               "R2_ADS_SENS", { {"Game default", ""}, {"50%", "0.5"}, {"75%", "0.75"}, {"125%", "1.25"}, {"150%", "1.5"} }, 0);
    add_option(SEC_INPUT, "aimtoggle", "Aim mode",
               "Hold: aim while the Aim key / left bumper is held (PS3 original). Toggle: press once to aim, again to "
               "stop. Keyboard, mouse and controller.",
               "PAD_AIM_TOGGLE", { {"Hold", ""}, {"Toggle", "1"} }, 0);
    add_option(SEC_INPUT, "deadzone", "Stick deadzone",
               "How far a controller stick must move before it counts. Smaller is more responsive; larger hides "
               "stick drift. Default is the XInput standard, applied per axis.",
               "PAD_DEADZONE", { {"Default", ""}, {"Small  (8%)", "0.08"}, {"Medium  (15%)", "0.15"},
                                 {"Large  (25%)", "0.25"}, {"Very large  (35%)", "0.35"} }, 0);
    add_option(SEC_INPUT, "curve", "Stick response curve",
               "How stick travel maps to speed. Linear is direct; Precise is slow near the centre for fine aim; "
               "Fast reaches full speed sooner.",
               "PAD_CURVE", { {"Default", ""}, {"Linear", "1"}, {"Precise", "1.6"}, {"Very precise", "2.2"}, {"Fast", "0.7"} }, 0);
    // SDL3 controllers, gyro aiming, light bar (ps3recomp/libs/input/cellPad.c, pad_sdl3.c)
    add_option(SEC_INPUT, "padbackend", "Controller support",
               "SDL3 reads DualShock 4, DualSense, Switch Pro / Joy-Con and Xbox controllers directly: correct "
               "button prompts, rumble, light bar, gyro aiming, up to 8 controllers -- no DS4Windows or Steam "
               "Input needed. XInput is the old path (Xbox controllers and wrappers only); it is also used "
               "automatically if SDL3 cannot start.",
               "PAD_BACKEND", { {"SDL3", ""}, {"XInput", "xinput"} }, 0);
    add_option(SEC_INPUT, "gyro", "Gyro aiming",
               "Turn the view by turning the controller (DualShock 4, DualSense, Switch Pro, Joy-Con). While "
               "aiming: only with the Aim button held / aiming down the sights. Keep the controller still for "
               "a second after it connects (calibration).",
               "PAD_GYRO", { {"Off", ""}, {"On", "1"}, {"While aiming", "aim"} }, 0);
    add_option(SEC_INPUT, "gyrosens", "Gyro sensitivity",
               "How far the view turns for each degree the controller turns.",
               "PAD_GYRO_SENS", { {"Default  (2x)", ""}, {"0.5x", "0.5"}, {"1x", "1"}, {"1.5x", "1.5"}, {"3x", "3"},
                                  {"4x", "4"}, {"6x", "6"} }, 0);
    add_option(SEC_INPUT, "gyroinvy", "Gyro vertical",
               "Invert the up / down gyro.",
               "PAD_GYRO_INVERT_Y", { {"Normal", ""}, {"Inverted", "1"} }, 0);
    add_option(SEC_INPUT, "gyrospace", "Gyro turning axis",
               "Controller: turning left / right follows the controller's own axis (yaw). Player: follows "
               "gravity, so it works the same with the controller flat or upright.",
               "PAD_GYRO_SPACE", { {"Controller", ""}, {"Player", "player"} }, 0);
    add_option(SEC_INPUT, "gyrocal", "Gyro calibration",
               "Learns the gyro's resting drift when a controller connects (keep it still for a second).",
               "PAD_GYRO_CALIBRATE", { {"On connect", ""}, {"Off", "0"} }, 0);
    add_option(SEC_INPUT, "lightbar", "Light bar",
               "DualShock 4 / DualSense light bar in the player's colour (player 1 blue, player 2 red).",
               "PAD_LIGHTBAR", { {"Player colour", ""}, {"Off", "0"} }, 0);
    add_option(SEC_INPUT, "nswap", "Nintendo button layout",
               "Switch Pro / Joy-Con: By position confirms with the bottom button (B) as on PlayStation; "
               "Nintendo style confirms with A and shows Xbox-style letters.",
               "PAD_NINTENDO_SWAP", { {"By position", ""}, {"Nintendo style (A confirms)", "1"} }, 0);
    // Local co-op (split-screen): which controller is which player (ps3recomp/libs/input/cellPad.c)
    add_option(SEC_INPUT, "coop", "Local co-op  (split-screen)",
               "A second controller is player 2: press START on it to join (see the README, \"Local co-op\"). "
               "Off keeps every extra controller away from the game.",
               "PAD_COOP", { {"On", ""}, {"Off", "0"} }, 0);
    add_option(SEC_INPUT, "p1pad", "Player 1 controller",
               "Which controller is player 1 (with the keyboard and mouse). Automatic takes the first one found; "
               "controllers are numbered in the order they were connected (SDL3), or in Windows' XInput order.",
               "PAD_P1_CONTROLLER", { {"Automatic", ""}, {"Controller 1", "1"}, {"Controller 2", "2"},
                                      {"Controller 3", "3"}, {"Controller 4", "4"}, {"Controller 5", "5"},
                                      {"Controller 6", "6"}, {"Controller 7", "7"}, {"Controller 8", "8"} }, 0);
    add_option(SEC_INPUT, "p2pad", "Player 2 controller",
               "Which controller is player 2 in local co-op. Automatic takes the next one found.",
               "PAD_P2_CONTROLLER", { {"Automatic", ""}, {"Controller 1", "1"}, {"Controller 2", "2"},
                                      {"Controller 3", "3"}, {"Controller 4", "4"}, {"Controller 5", "5"},
                                      {"Controller 6", "6"}, {"Controller 7", "7"}, {"Controller 8", "8"} }, 0);
    add_option(SEC_INPUT, "p2kb", "Player 2 uses keyboard and mouse",
               "Local co-op with one controller: player 1 plays on the controller, player 2 on the keyboard and "
               "mouse (the mouse moves player 2's view like a right stick).",
               "PAD_P2_KEYBOARD", { {"Off", ""}, {"On", "1"} }, 0);
}

static char g_dir[MAX_PATH];
static std::string g_hw_desc = "default settings";   // the detected hardware
static std::string g_rec_desc;                       // what was recommended for it
static r2ac::Hardware g_hwi;
static r2ac::Settings g_rec;
static bool g_detected = false;

// Test hooks (--detect / --table): override what detect() found.
static void apply_sim(r2ac::Hardware& h, const char* cmd)
{
    if (!cmd) return;
    auto arg = [&](const char* k) -> const char* { const char* p = strstr(cmd, k); return p ? p + strlen(k) : nullptr; };
    if (const char* p = arg("--sim-gpu ")) {
        char b[128] = {}; const char* e;
        if (*p == '"') { p++; e = strchr(p, '"'); } else e = strchr(p, ' ');
        size_t n = e ? (size_t)(e - p) : strlen(p); if (n >= sizeof b) n = sizeof b - 1;
        memcpy(b, p, n); strcpy_s(h.gpu, b);
        h.vendor = r2ac::has(b, "NVIDIA") || r2ac::has(b, "GeForce") ? 0x10DE : r2ac::has(b, "AMD") || r2ac::has(b, "Radeon") ? 0x1002
                 : r2ac::has(b, "Intel") ? 0x8086 : 0;
    }
    if (const char* p = arg("--sim-vram ")) h.vram_gb = atof(p);
    if (const char* p = arg("--sim-cores ")) { h.cores = atoi(p); h.pcores = h.ecores = 0; if (h.threads < h.cores) h.threads = h.cores; }
    if (const char* p = arg("--sim-threads ")) h.threads = atoi(p);
    if (const char* p = arg("--sim-screen ")) { h.screen_w = atoi(p); if (const char* x = strchr(p, 'x')) h.screen_h = atoi(x + 1); }
    if (const char* p = arg("--sim-hz ")) h.refresh = atoi(p);
}

static void pick(int s, const char* v)
{
    for (size_t i = 0; i < g_set[s].choices.size(); i++) if (!strcmp(g_set[s].choices[i].value, v)) g_set[s].def = (int)i;
}

// Recommended defaults from the hardware (project/r2_autoconfig.h, shared with the game's R2_PRESET):
// the GPU the renderer will use (discrete before integrated), the CPU's cores and threads, and the
// primary screen's resolution and refresh rate. Sets each Setting's def.
static void detect_hardware(const char* sim = nullptr)
{
    g_hwi = r2ac::Hardware();
    r2ac::detect(g_hwi);
    if (sim && strstr(sim, "--sim-")) { apply_sim(g_hwi, sim); r2ac::classify(g_hwi); }
    g_rec = r2ac::recommend(g_hwi);
    pick(S_DISPLAY, "borderless");
    pick(S_WINDOW, g_rec.window);
    pick(S_FPS, g_rec.fps);
    pick(S_VSYNC, g_rec.vsync);
    pick(S_RES, g_rec.res);
    pick(S_FSR, g_rec.fsr);
    pick(S_UPSCALE, !strcmp(g_rec.fsr, "0") ? "1" : g_rec.upscale);
    pick(S_ANISO, g_rec.aniso);
    pick(S_FILTER, g_rec.filter);
    pick(S_SHARPEN, g_rec.sharpen);
    pick(S_RENDERER, "rpcs3");         // Native Vulkan stays opt-in (experimental)
    pick(S_CPU, g_rec.cpu);
    pick(S_SHADER, g_rec.shader);
    for (size_t i = S_COUNT; i < g_set.size(); i++)
        if (const char* v = g_set[i].tier_val[g_rec.tier]) pick((int)i, v);
    char b[512];
    r2ac::describe_hw(g_hwi, b, sizeof b); g_hw_desc = b;
    r2ac::describe(g_rec, b, sizeof b); g_rec_desc = b;
    g_detected = true;
}

static std::string path_of(const char* name) { return std::string(g_dir) + "\\" + name; }

static void load_settings()
{
    for (auto& s : g_set) s.sel = s.def;
    FILE* f = fopen(path_of("settings.ini").c_str(), "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char* v = eq + 1;
        v[strcspn(v, "\r\n")] = 0;
        for (auto& s : g_set)
            if (!strcmp(s.key, line))
                for (size_t i = 0; i < s.choices.size(); i++)
                    if (!strcmp(s.choices[i].value, v)) s.sel = (int)i;
    }
    fclose(f);
}
static const char* val(int i) { return g_set[i].choices[g_set[i].sel].value; }

static void write_env(FILE* f);
static bool save_settings()
{
    FILE* f = fopen(path_of("settings.ini").c_str(), "w");
    if (!f) return false;
    for (auto& s : g_set) fprintf(f, "%s=%s\n", s.key, s.choices[s.sel].value);
    fclose(f);
    f = fopen(path_of("settings.bat").c_str(), "w");
    if (!f) return false;
    fprintf(f, "@echo off\nrem Written by Launcher.exe -- change these in the launcher.\n");
    write_env(f);
    fclose(f);
    return true;
}

// ---- update check, Wine / Steam Deck extras (launcher_dist.inc) ----
#define DIST_REPO "miroslavdanutalexandru-alt/Resistance3Port"
#define DIST_USER_DIR "Resistance 3"   // settings backup: Saved Games\<this>\config
#define DIST_ADD_OPTION
#include "launcher_dist.inc"
#include "port_report.cpp"                  // "Report a problem" zip + "the game crashed last time" prompt

// RSX_RES_SCALE: "auto" = the largest step not above the output (fullscreen: the screen, Windowed: the window),
// so DLSS/FSR never upscale beyond what is shown and get scaled back down
static const char* res_value()
{
    if (strcmp(val(S_RES), "auto")) return val(S_RES);
    int oh = g_hwi.screen_h > 0 ? g_hwi.screen_h : 1080;
    if (!strcmp(val(S_DISPLAY), "windowed"))
        if (const char* x = strchr(val(S_WINDOW), 'x')) oh = atoi(x + 1);
    return r2ac::res_for_height(oh);
}

// the settings.bat lines for the current selection
static void write_env(FILE* f)
{
    fprintf(f, "set \"R2_RSX=%s\"\n", val(S_RENDERER));
    fprintf(f, "set \"RSX_DISPLAY=%s\"\n", val(S_DISPLAY));
    fprintf(f, "set \"RSX_WINDOW=%s\"\n", val(S_WINDOW));
    fprintf(f, "set \"RSX_RES_SCALE=%s\"\n", res_value());
    fprintf(f, "set \"RSX_VSYNC=%s\"\n", val(S_VSYNC));
    fprintf(f, "set \"R2_FPS=%s\"\n", val(S_FPS));
    fprintf(f, "set \"PORT_CPU_PROFILE=%s\"\n", val(S_CPU));
    fprintf(f, "set \"RSX_ANISO=%s\"\n", val(S_ANISO));
    fprintf(f, "set \"RSX_FILTER=%s\"\n", val(S_FILTER));
    fprintf(f, "set \"RSX_SHARPEN=%s\"\n", val(S_SHARPEN));
    // AMD FSR 3.1 is RSX_FSR=1 on the RPCS3 renderer and RSX_FSR=3 on the native Vulkan one (1 = FSR1 there).
    // XeSS (4) only exists on the RPCS3 renderer; on the native one it falls back to FSR 3.1 as well.
    const bool fsr3_native = !strcmp(val(S_RENDERER), "vulkan") &&
                             (!strcmp(val(S_FSR), "1") || !strcmp(val(S_FSR), "4"));
    fprintf(f, "set \"RSX_FSR=%s\"\n", fsr3_native ? "3" : val(S_FSR));
    fprintf(f, "set \"RSX_UPSCALE=%s\"\n", g_set[S_FSR].sel ? val(S_UPSCALE) : "1");
    fprintf(f, "set \"R2_SHADER_MODE=%s\"\n", val(S_SHADER));
    for (size_t i = S_COUNT; i < g_set.size(); i++)
        if (g_set[i].env && *val((int)i)) fprintf(f, "set \"%s=%s\"\n", g_set[i].env, val((int)i));
}

// ===========================================================================
// Keyboard & mouse controls (controls.txt, read by libs/input/cellPad.c)
// ===========================================================================
// Resistance 3's default PS3 layout: L1 aim, R1 fire, R2 secondary fire, L2 grenade,
// L3 sprint, R3 melee, Cross jump, Circle crouch, Square reload / use (hold),
// Triangle switch weapon (hold for the weapon wheel), d-pad left/right grenade type,
// d-pad up/down scope zoom. Several rows may press the same button; controls.txt
// keeps which row a key belongs to in its comment.
struct Act { const char* name; const char* pad; const char* group; const char* help; bool hidden; };
static const Act g_acts[] = {
    { "Move forward",        "LS_UP",      "Movement", "", false },
    { "Move back",           "LS_DOWN",    "Movement", "", false },
    { "Move left",           "LS_LEFT",    "Movement", "", false },
    { "Move right",          "LS_RIGHT",   "Movement", "", false },
    { "Jump",                "CROSS",      "Movement", "", false },
    { "Crouch",              "CIRCLE",     "Movement", "Tap to crouch or stand up.", false },
    { "Sprint",              "L3",         "Movement", "Press while moving (the left stick click).", false },
    { "Fire",                "R1",         "Combat", "", false },
    { "Aim / zoom",          "L1",         "Combat", "Aims down the sights or through the scope.", false },
    { "Secondary fire",      "R2",         "Combat", "Each weapon's secondary mode (Bullseye tag, Magnum detonate, "
                                                      "Auger shield...).", false },
    { "Reload",              "SQUARE",     "Combat", "Same PS3 button as Use (Square): a tap reloads.", false },
    { "Melee",               "R3",         "Combat", "", false },
    { "Switch weapon",       "TRIANGLE",   "Combat", "Tap to swap to the previous weapon, hold for the weapon wheel.", false },
    { "Next weapon (wheel)", "TRIANGLE",   "Combat", "Each notch of the mouse wheel swaps weapon (a tap of Triangle).", false },
    { "Throw grenade",       "L2",         "Combat", "", false },
    { "Previous grenade type", "DPAD_LEFT", "Combat", "", false },
    { "Next grenade type",   "DPAD_RIGHT", "Combat", "", false },
    { "Use / interact",      "INTERACT",   "Actions", "Doors, switches, ammo boxes, reviving a team-mate (hold). The game's "
                                                       "Square button: a tap also reloads.", false },
    { "D-pad up (scope zoom in)", "DPAD_UP", "Actions", "Changes the Deadeye scope's magnification.", false },
    { "D-pad down (scope zoom out / co-op partner)", "DPAD_DOWN", "Actions", "Scope magnification; calls your "
                                                       "partner in co-op.", false },
    { "Pause menu",          "START",      "Menus", "", false },
    { "Scoreboard / objectives", "SELECT", "Menus", "", false },
    { "Menu confirm",        "CROSS",      "Menus", "The Cross button: selects in menus (Jump does the same).", false },
    { "Menu back",           "CIRCLE",     "Menus", "The Circle button: goes back in menus (Crouch does the same).", false },
    { "Look up",             "RS_UP",      "Look with keys", "Optional: turn the camera with keys as well as the mouse.", false },
    { "Look down",           "RS_DOWN",    "Look with keys", "Optional: turn the camera with keys as well as the mouse.", false },
    { "Look left",           "RS_LEFT",    "Look with keys", "Optional: turn the camera with keys as well as the mouse.", false },
    { "Look right",          "RS_RIGHT",   "Look with keys", "Optional: turn the camera with keys as well as the mouse.", false },
};
static const int NACT = sizeof g_acts / sizeof g_acts[0];
static const int NSLOT = 3;            // the file may hold 3; the page shows 2 (a 3rd is kept)
static const int NSHOW = 2;
static std::string g_bind[NACT][NSLOT];
static double g_msens = 1.0; static int g_minv = 0, g_mdirect = 1;
static std::string g_mrelease = "F8";
static double g_mdeg = 0.05, g_mdz = 0.28, g_msmooth = 0.03;
static const char* g_padint_opts[][2] = {
    {"NONE","Same as Reload  (as on PS3)"},{"SQUARE","Square"},{"CIRCLE","Circle"},{"CROSS","Cross"},
    {"TRIANGLE","Triangle"},{"L1","L1"},{"R1","R1"},{"L2","L2"},{"R2","R2"},{"L3","L3  (left stick click)"},
    {"R3","R3  (right stick click)"},{"DPAD_UP","D-pad up"},{"DPAD_DOWN","D-pad down"},{"DPAD_LEFT","D-pad left"},
    {"DPAD_RIGHT","D-pad right"},
};
static const int NPADINT = sizeof g_padint_opts / sizeof g_padint_opts[0];
static int g_padint = 0;

// the defaults that ship in controls.txt: { key, row name } (the row gives the button)
static const char* g_default_binds[][2] = {
    {"W","Move forward"},{"S","Move back"},{"A","Move left"},{"D","Move right"},
    {"SPACE","Jump"},{"C","Crouch"},{"CTRL","Crouch"},{"SHIFT","Sprint"},
    {"MOUSE1","Fire"},{"MOUSE2","Aim / zoom"},{"MOUSE3","Secondary fire"},{"F","Secondary fire"},
    {"R","Reload"},{"V","Melee"},{"MOUSE4","Melee"},
    {"Q","Switch weapon"},{"WHEELUP","Next weapon (wheel)"},{"WHEELDOWN","Next weapon (wheel)"},
    {"G","Throw grenade"},{"MOUSE5","Throw grenade"},{"1","Previous grenade type"},{"2","Next grenade type"},
    {"E","Use / interact"},{"T","D-pad up (scope zoom in)"},{"B","D-pad down (scope zoom out / co-op partner)"},
    {"ESC","Pause menu"},{"TAB","Scoreboard / objectives"},{"ENTER","Pause menu"},{"BACKSPACE","Menu back"},
    {"UP","Look up"},{"DOWN","Look down"},{"LEFT","Look left"},{"RIGHT","Look right"},
};

static int act_of(const char* pad)
{
    for (int i = 0; i < NACT; i++) if (!_stricmp(g_acts[i].pad, pad)) return i;
    return -1;
}
static int act_named(const char* name)
{
    for (int i = 0; i < NACT; i++) if (!_stricmp(g_acts[i].name, name)) return i;
    return -1;
}
static void bind_add(int a, const char* key)
{
    for (int k = 0; k < NSLOT; k++) if (!_stricmp(g_bind[a][k].c_str(), key)) return;
    for (int k = 0; k < NSLOT; k++) if (g_bind[a][k].empty()) { g_bind[a][k] = key; return; }
}
// the row for a controls.txt line: the one its comment names, else the first row for
// that button with a free visible box, else the first row for that button
static int act_for_line(const char* pad, const char* comment)
{
    const int named = act_named(comment);
    if (named >= 0 && !_stricmp(g_acts[named].pad, pad)) return named;
    for (int i = 0; i < NACT; i++)
        if (!_stricmp(g_acts[i].pad, pad))
            for (int k = 0; k < NSHOW; k++) if (g_bind[i][k].empty()) return i;
    return act_of(pad);
}
// keys fill the boxes from the left (a cleared first key lets the second move up)
static void ctl_compact()
{
    for (int a = 0; a < NACT; a++) {
        std::string keep[NSLOT]; int n = 0;
        for (int k = 0; k < NSLOT; k++) if (!g_bind[a][k].empty()) keep[n++] = g_bind[a][k];
        for (int k = 0; k < NSLOT; k++) g_bind[a][k] = k < n ? keep[k] : std::string();
    }
}
static void ctl_defaults()
{
    for (auto& r : g_bind) for (auto& k : r) k.clear();
    for (auto& d : g_default_binds) { int a = act_named(d[1]); if (a >= 0) bind_add(a, d[0]); }
    g_msens = 1.0; g_minv = 0; g_mdirect = 1; g_mrelease = "F8"; g_mdeg = 0.05; g_mdz = 0.28; g_msmooth = 0.03;
    g_padint = 0;
}
static void ctl_load()
{
    FILE* f = fopen(path_of("controls.txt").c_str(), "r");
    if (!f) { ctl_defaults(); return; }
    for (auto& r : g_bind) for (auto& k : r) k.clear();
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char cmt[128] = "";
        if (char* h = strchr(line, '#')) {
            *h = 0; h++;
            while (*h == ' ' || *h == '\t') h++;
            snprintf(cmt, sizeof cmt, "%s", h);
            for (size_t n = strlen(cmt); n && (cmt[n - 1] == '\n' || cmt[n - 1] == '\r' || cmt[n - 1] == ' '); n--) cmt[n - 1] = 0;
        }
        char* eq = strchr(line, '='); if (!eq) continue;
        *eq = 0;
        char k[64] = "", v[64] = "";
        if (sscanf(line, " %63s", k) != 1 || sscanf(eq + 1, " %63s", v) != 1) continue;
        if (!_stricmp(k, "mouse_sensitivity")) { g_msens = atof(v); continue; }
        if (!_stricmp(k, "mouse_invert_y")) { g_minv = atoi(v); continue; }
        if (!_stricmp(k, "mouse_look")) { g_mdirect = _stricmp(v, "stick") ? 1 : 0; continue; }
        if (!_stricmp(k, "mouse_release_key")) { g_mrelease = v; continue; }
        if (!_stricmp(k, "mouse_degrees_per_count")) { g_mdeg = atof(v); continue; }
        if (!_stricmp(k, "mouse_deadzone")) { g_mdz = atof(v); continue; }
        if (!_stricmp(k, "mouse_smoothing")) { g_msmooth = atof(v); continue; }
        if (!_stricmp(k, "pad_interact")) {
            g_padint = 0;
            for (int i = 0; i < NPADINT; i++) if (!_stricmp(v, g_padint_opts[i][0])) g_padint = i;
            continue; }
        const int a = act_for_line(v, cmt);
        if (a >= 0) { for (char* p = k; *p; p++) *p = (char)toupper((unsigned char)*p); bind_add(a, k); }
    }
    fclose(f);
    if (g_msens < 0.1) g_msens = 0.1; if (g_msens > 4.0) g_msens = 4.0;
}
static bool ctl_save()
{
    ctl_compact();
    FILE* f = fopen(path_of("controls.txt").c_str(), "w");
    if (!f) return false;
    fprintf(f, "# Resistance 3 PC port -- keyboard & mouse controls.\n"
               "# Edit these in Launcher.exe -> Controls, or by hand: KEY = ACTION, one per line.\n"
               "# KEYS:    A-Z 0-9 F1-F12 SPACE SHIFT CTRL ALT TAB ENTER ESC BACKSPACE UP DOWN LEFT RIGHT\n"
               "#          CAPSLOCK INSERT DELETE HOME END PGUP PGDN NUM0-NUM9\n"
               "#          GRAVE MINUS EQUALS LBRACKET RBRACKET BACKSLASH SEMICOLON APOSTROPHE COMMA PERIOD SLASH\n"
               "#          MOUSE1 (left) MOUSE2 (right) MOUSE3 (middle) MOUSE4 MOUSE5 WHEELUP WHEELDOWN\n"
               "# ACTIONS: LS_UP LS_DOWN LS_LEFT LS_RIGHT (move)  RS_UP RS_DOWN RS_LEFT RS_RIGHT (look)\n"
               "#          CROSS CIRCLE SQUARE TRIANGLE L1 R1 L2 R2 L3 R3 START SELECT\n"
               "#          DPAD_UP DPAD_DOWN DPAD_LEFT DPAD_RIGHT\n"
               "#          INTERACT (presses Square; on-screen prompts to use something show this key)\n"
               "# A key may be listed more than once to press several things. The comment after a\n"
               "# binding is the launcher row it belongs to (and picks the key shown in prompts).\n\n");
    for (int a = 0; a < NACT; a++)
        for (int k = 0; k < NSLOT; k++)
            if (!g_bind[a][k].empty())
                fprintf(f, "%-10s = %-10s # %s\n", g_bind[a][k].c_str(), g_acts[a].pad, g_acts[a].name);
    fprintf(f, "\n# Mouse\n");
    fprintf(f, "mouse_sensitivity = %.2f\n", g_msens);
    fprintf(f, "mouse_invert_y = %d\n", g_minv);
    fprintf(f, "mouse_look = %s        # direct: the mouse turns the camera 1:1 on foot; stick: old stick emulation\n", g_mdirect ? "direct" : "stick");
    fprintf(f, "mouse_degrees_per_count = %.3f   # direct look speed (x mouse_sensitivity, x the game's Look Sensitivity)\n", g_mdeg);
    fprintf(f, "mouse_deadzone = %.2f      # stick emulation only\n", g_mdz);
    fprintf(f, "mouse_smoothing = %.2f     # stick emulation only, seconds\n", g_msmooth);
    fprintf(f, "mouse_release_key = %s     # free / recapture the mouse cursor\n", g_mrelease.c_str());
    fclose(f);
    return true;
}

// key name for a captured input (names the game's controls.txt reader knows)
static const char* vk_name(int vk)
{
    static char one[2];
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { one[0] = (char)vk; one[1] = 0; return one; }
    if (vk >= VK_F1 && vk <= VK_F12) { static char fn[4]; snprintf(fn, sizeof fn, "F%d", vk - VK_F1 + 1); return fn; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { static char nn[5]; snprintf(nn, sizeof nn, "NUM%d", vk - VK_NUMPAD0); return nn; }
    switch (vk) {
    case VK_SPACE: return "SPACE"; case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return "SHIFT";
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return "CTRL"; case VK_MENU: case VK_LMENU: case VK_RMENU: return "ALT";
    case VK_TAB: return "TAB"; case VK_RETURN: return "ENTER"; case VK_ESCAPE: return "ESC"; case VK_BACK: return "BACKSPACE";
    case VK_UP: return "UP"; case VK_DOWN: return "DOWN"; case VK_LEFT: return "LEFT"; case VK_RIGHT: return "RIGHT";
    case VK_CAPITAL: return "CAPSLOCK"; case VK_INSERT: return "INSERT"; case VK_DELETE: return "DELETE";
    case VK_HOME: return "HOME"; case VK_END: return "END"; case VK_PRIOR: return "PGUP"; case VK_NEXT: return "PGDN";
    case VK_OEM_3: return "GRAVE"; case VK_OEM_MINUS: return "MINUS"; case VK_OEM_PLUS: return "EQUALS";
    case VK_OEM_4: return "LBRACKET"; case VK_OEM_6: return "RBRACKET"; case VK_OEM_5: return "BACKSLASH";
    case VK_OEM_1: return "SEMICOLON"; case VK_OEM_7: return "APOSTROPHE"; case VK_OEM_COMMA: return "COMMA";
    case VK_OEM_PERIOD: return "PERIOD"; case VK_OEM_2: return "SLASH";
    }
    return NULL;
}
static std::string pretty(const std::string& k)
{
    static const struct { const char* n; const char* p; } t[] = {
        {"MOUSE1","Left mouse"},{"MOUSE2","Right mouse"},{"MOUSE3","Middle mouse"},{"MOUSE4","Mouse 4"},{"MOUSE5","Mouse 5"},
        {"WHEELUP","Wheel up"},{"WHEELDOWN","Wheel down"},{"SPACE","Space"},{"SHIFT","Shift"},{"CTRL","Ctrl"},{"ALT","Alt"},
        {"TAB","Tab"},{"ENTER","Enter"},{"ESC","Esc"},{"BACKSPACE","Backspace"},{"UP","Up arrow"},{"DOWN","Down arrow"},
        {"LEFT","Left arrow"},{"RIGHT","Right arrow"},{"CAPSLOCK","Caps Lock"},{"INSERT","Insert"},{"DELETE","Delete"},
        {"HOME","Home"},{"END","End"},{"PGUP","Page Up"},{"PGDN","Page Down"},
        {"GRAVE","`"},{"MINUS","-"},{"EQUALS","="},
        {"LBRACKET","["},{"RBRACKET","]"},{"BACKSLASH","\\"},{"SEMICOLON",";"},{"APOSTROPHE","'"},{"COMMA",","},{"PERIOD","."},{"SLASH","/"},
    };
    for (auto& e : t) if (!_stricmp(k.c_str(), e.n)) return e.p;
    if (k.size() == 4 && !_strnicmp(k.c_str(), "NUM", 3)) return std::string("Numpad ") + k[3];
    return k;
}

// ===========================================================================
// Drawing: palette, DPI, fonts
// ===========================================================================
static const Color C_SIDEBAR(255, 14, 17, 21), C_BG(255, 20, 24, 28), C_PANEL(255, 27, 32, 37),
    C_CTL(255, 35, 42, 48), C_CTL_HOT(255, 46, 56, 63), C_BORDER(255, 52, 63, 71),
    C_TEXT(255, 236, 236, 236), C_DIM(255, 150, 156, 164), C_FAINT(255, 104, 110, 118),
    C_ACCENT(255, 92, 214, 226), C_ACCENT_HOT(255, 140, 234, 242), C_ACCENT_DIM(255, 30, 78, 86),
    C_ON_ACCENT(255, 8, 22, 26), C_WARN(255, 236, 160, 90);

static double g_s = 1.0;                 // DPI scale
static int S(double v) { return (int)lround(v * g_s); }
static float SF(double v) { return (float)(v * g_s); }

static ULONG_PTR g_gdip;
static Font *f_title, *f_sub, *f_tiny, *f_h1, *f_h2, *f_body, *f_small, *f_btn, *f_play, *f_nav;
static std::wstring g_fam_ui = L"Segoe UI", g_fam_ui_semi = L"Segoe UI Semibold", g_fam_title = L"Segoe UI Semibold";

static bool family_ok(const wchar_t* n) { FontFamily ff(n); return ff.IsAvailable() != FALSE; }
static Font* mkfont(const std::wstring& fam, double px, int style)
{
    FontFamily ff(fam.c_str());
    if (!ff.IsAvailable()) { FontFamily gf(L"Tahoma"); return new Font(&gf, SF(px), style, UnitPixel); }
    return new Font(&ff, SF(px), style, UnitPixel);
}
static void make_fonts()
{
    Font* all[] = { f_title, f_sub, f_tiny, f_h1, f_h2, f_body, f_small, f_btn, f_play, f_nav };
    for (Font* f : all) delete f;
    if (!family_ok(L"Segoe UI")) { g_fam_ui = L"Tahoma"; }
    if (family_ok(L"Segoe UI Semibold")) g_fam_ui_semi = L"Segoe UI Semibold"; else g_fam_ui_semi = g_fam_ui;
    // R3's look: the wide, squared Bahnschrift (R2's launcher uses the condensed cut)
    if (family_ok(L"Bahnschrift SemiBold")) g_fam_title = L"Bahnschrift SemiBold";
    else if (family_ok(L"Bahnschrift")) g_fam_title = L"Bahnschrift";
    else g_fam_title = g_fam_ui_semi;
    const bool semi = g_fam_ui_semi != g_fam_ui;
    f_title = mkfont(g_fam_title, 34, FontStyleBold * (g_fam_title == g_fam_ui));
    f_sub   = mkfont(g_fam_title, 15, FontStyleRegular);
    f_tiny  = mkfont(g_fam_ui, 11, FontStyleRegular);
    f_h1    = mkfont(g_fam_ui_semi, 24, semi ? FontStyleRegular : FontStyleBold);
    f_h2    = mkfont(g_fam_ui_semi, 13, semi ? FontStyleRegular : FontStyleBold);
    f_body  = mkfont(g_fam_ui, 14.5, FontStyleRegular);
    f_small = mkfont(g_fam_ui, 12.5, FontStyleRegular);
    f_btn   = mkfont(g_fam_ui_semi, 14, semi ? FontStyleRegular : FontStyleBold);
    f_play  = mkfont(g_fam_title, 24, FontStyleRegular);
    f_nav   = mkfont(g_fam_ui, 15, FontStyleRegular);
}

static void round_rect(GraphicsPath& p, RectF r, float rad)
{
    const float d = rad * 2;
    if (rad <= 0.5f) { p.AddRectangle(r); return; }
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}
static void fill_rr(Graphics& g, RectF r, float rad, const Color& c)
{
    GraphicsPath p; round_rect(p, r, rad); SolidBrush b(c); g.FillPath(&b, &p);
}
static void stroke_rr(Graphics& g, RectF r, float rad, const Color& c, float w)
{
    GraphicsPath p; round_rect(p, r, rad); Pen pen(c, w); g.DrawPath(&pen, &p);
}
static std::wstring W(const std::string& s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n); return w;
}
enum { AL_L = 0, AL_C = 1, AL_R = 2 };
static void text(Graphics& g, const std::wstring& t, Font* f, const Color& c, RectF r, int h = AL_L, bool vcenter = true,
                 bool wrap = false)
{
    StringFormat sf;
    sf.SetAlignment(h == AL_C ? StringAlignmentCenter : h == AL_R ? StringAlignmentFar : StringAlignmentNear);
    sf.SetLineAlignment(vcenter ? StringAlignmentCenter : StringAlignmentNear);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    if (!wrap) sf.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush b(c);
    g.DrawString(t.c_str(), -1, f, r, &sf, &b);
}
static float text_h(Graphics& g, const std::wstring& t, Font* f, float w)
{
    RectF out; g.MeasureString(t.c_str(), -1, f, RectF(0, 0, w, 10000), &out);
    return out.Height;
}

// ===========================================================================
// Widgets: rebuilt by layout() for the current page, then painted / hit-tested
// ===========================================================================
enum WType { W_NAV, W_PLAY, W_BUTTON, W_DROP, W_SLIDER, W_TOGGLE, W_KEY, W_KEYX, W_ROW };
struct Widget {
    WType t; RectF r; int id = 0, sub = 0;
    bool scroll = true;          // lives in the scrolling content area
    std::string title, help;     // for the help strip
    bool disabled = false;
};
// --page N opens page N (the order below; README "Launcher"). launcher_cats.inc places the options on the pages.
enum Page { P_DISPLAY, P_GRAPHICS, P_UPSCALE, P_PERF, P_CONTROLS, P_AUDIO, P_UI, P_ADVANCED, P_TROPHIES, P_TOOLS,
            P_ABOUT, P_COUNT };
static const char* g_page_name[P_COUNT] = { "Display", "Graphics", "Upscaling & Frame Gen", "Performance", "Controls",
                                            "Audio", "Interface & Notifications", "Advanced", "Trophies",
                                            "Saves & tools", "About" };
static int g_page = P_DISPLAY;
static float g_nav_end = 0;              // bottom of the last sidebar item (the art starts below it)
static std::vector<Widget> g_w;
static float g_scroll[P_COUNT], g_content_h = 0;
static int g_hot = -1, g_press = -1;
static RectF g_view;                     // content viewport (client coordinates)
static HWND g_hw;

// dropdown overlay
enum { DD_SETTING = 1, DD_PADINT, DD_MLOOK };
static struct { bool open = false; int kind = 0, idx = 0, hot = -1; RectF anchor, list; float item_h = 0; } g_dd;
// sliders
enum { SL_FOV = 1, SL_SENS };
static int g_drag = 0;                   // slider being dragged
// toggles
enum { TG_INVERT = 1 };
// buttons
enum { BT_RESET_CTL = 1, BT_RESET_GFX, BT_FOLDER, BT_LOGS, BT_WEB, BT_CONTROLS_TXT, BT_SAVES, BT_SHOTS, BT_RESET_ALL, BT_REBUILD_SHADERS, BT_REPORT };
// key capture
static int g_cap_a = -1, g_cap_k = -1;   // action/slot being captured (-2 = mouse release key)
static std::wstring g_toast; static DWORD g_toast_until = 0;

static void toast(const std::string& s) { g_toast = W(s); g_toast_until = GetTickCount() + 5000; SetTimer(g_hw, 1, 5100, NULL); }

// ---- offline shader scan state (see the shaderscan block above click()) ----
enum { WM_SCAN_PROGRESS = WM_APP + 0x53, WM_SCAN_DONE };
static SRWLOCK g_scan_lock = SRWLOCK_INIT;
static struct {
    bool active = false, checked = false, quit = false;
    HANDLE thread = NULL, proc = NULL;    // proc: guarded by g_scan_lock
    bool cancel = false;                  // guarded by g_scan_lock
    bool ran = false;                     // the scan itself started (not only --check); guarded
    std::string phase, error;             // guarded
    unsigned done = 0, total = 0;         // guarded
    int exit_code = 0;
    DWORD t0 = 0;
    RectF skip_btn;
} g_scan;

static int dd_count(int kind, int idx)
{
    if (kind == DD_SETTING) return (int)g_set[idx].choices.size();
    if (kind == DD_PADINT) return NPADINT;
    return 2;
}
static std::string dd_label(int kind, int idx, int i)
{
    if (kind == DD_SETTING) return g_set[idx].choices[i].label;
    if (kind == DD_PADINT) return g_padint_opts[i][1];
    return i == 0 ? "Direct  (recommended)" : "Classic  (stick emulation)";
}
static int dd_sel(int kind, int idx)
{
    if (kind == DD_SETTING) return g_set[idx].sel;
    if (kind == DD_PADINT) return g_padint;
    return g_mdirect ? 0 : 1;
}
// the choice detect_hardware() recommends for this PC
static bool dd_recommended(int kind, int idx, int i) { return kind == DD_SETTING && g_detected && i == g_set[idx].def; }
static void dd_set(int kind, int idx, int i)
{
    if (kind == DD_SETTING) { g_set[idx].sel = i; save_settings(); }
    else if (kind == DD_PADINT) { g_padint = i; ctl_save(); }
    else { g_mdirect = i == 0; ctl_save(); }
}

// ---- layout ---------------------------------------------------------------
static const float SIDEBAR = 236, PAD = 40, FOOT = 76, ROWH = 52, CTLW = 340;

static Widget& add(WType t, RectF r, int id = 0, int sub = 0, bool scroll = true)
{
    g_w.push_back(Widget()); Widget& w = g_w.back(); w.t = t; w.r = r; w.id = id; w.sub = sub; w.scroll = scroll; return w;
}
struct Lay { float x, y, w; };          // content coordinates (unscrolled), unscaled-logical * s


struct Deco { int kind; RectF r; std::string s; };      // labels and headings (painted, not hit)
static std::vector<Deco> g_deco;
enum { D_H1, D_INTRO, D_H2, D_LABEL, D_LABEL_DIM, D_GROUP, D_NOTE, D_ABOUT_TITLE, D_ABOUT_TEXT, D_KEYCOL, D_PANEL, D_REC, D_TROPHY };

static void deco(int kind, RectF r, const std::string& s) { g_deco.push_back({ kind, r, s }); }

static void layout_setting_row(float x, float& y, float w, int si)
{
    Setting& s = g_set[si];
    const bool dis = (si == S_WINDOW && strcmp(val(S_DISPLAY), "windowed") != 0) ||
                     (si == S_UPSCALE && g_set[S_FSR].sel == 0) ||
                     (si == S_SHADER && strcmp(val(S_RENDERER), "rpcs3") != 0) ||
                     (s.only_renderer && strcmp(val(S_RENDERER), s.only_renderer) != 0);
    Widget& row = add(W_ROW, RectF(x - SF(12), y, w + SF(24), SF(ROWH)), si);
    row.title = s.title; row.help = s.help; row.disabled = dis;
    deco(dis ? D_LABEL_DIM : D_LABEL, RectF(x, y, w - SF(CTLW) - SF(16), SF(ROWH)), s.title);
    Widget& d = add(W_DROP, RectF(x + w - SF(CTLW), y + SF(9), SF(CTLW), SF(ROWH - 18)), DD_SETTING, si);
    d.title = s.title; d.help = s.help; d.disabled = dis;
    y += SF(ROWH);
}

// differs from the recommendation (the window size and display mode are personal taste)
static bool differs_from_recommended()
{
    for (size_t i = 0; i < g_set.size(); i++)
        if (i != S_DISPLAY && i != S_WINDOW && g_set[i].section < SEC_PC && g_set[i].sel != g_set[i].def) return true;
    return false;
}

// "Recommended for your PC": what was detected, what it recommends, and the button that applies it
static void layout_recommend_panel(float x, float& y, float w)
{
    const float top = y;
    deco(D_PANEL, RectF(x - SF(12), top, w + SF(24), SF(150)), "");
    y += SF(12);
    deco(D_H2, RectF(x, y, w, SF(22)), "RECOMMENDED FOR YOUR PC"); y += SF(26);
    deco(D_REC, RectF(x, y, w, SF(24)), g_rec_desc); y += SF(26);
    deco(D_NOTE, RectF(x, y, w, SF(36)), g_hw_desc); y += SF(40);
    Widget& b = add(W_BUTTON, RectF(x, y, SF(250), SF(36)), BT_RESET_GFX);
    b.title = "Auto-detect (recommended)";
    b.help = "Detects your graphics card, processor and screen again and resets the settings on every page to the "
             "ones recommended for them (also the first-run defaults). Key bindings and the mouse settings are not changed.";
    deco(D_LABEL_DIM, RectF(x + SF(266), y, w - SF(266), SF(36)),
         differs_from_recommended() ? "Your settings differ from the recommendation." : "Using the recommended settings.");
    y = top + SF(150) + SF(22);
}

// ---- Trophies page (launcher_trophies.inc): the game's trophy list and icons (its TROPHY.TRP, from gamefiles) and
// the unlocks the game recorded (gamedata\\trophies\\<id>.json and Saved Games\\Resistance 3\\trophies.json). Read-only.
#define LT_GAME "Resistance 3"
#include "launcher_trophies.inc"

static void play();
#define LX_ENV(x) "R3_" x
#define LX_TAG "R3"
#define LX_HAS_FX 0                         // R2's toggles (screen blur, shake, letterbox) do not exist in Resistance 3
#define LX_HAS_PFX 1                        // Resistance 3: film grain / depth of field / motion blur (r3_pcextra.cpp)
#include "launcher_extra.inc"               // PC extras options, Saves & tools page, benchmark
#include "launcher_cats.inc"                // which options each page lists

static void layout()
{
    g_w.clear(); g_deco.clear();
    RECT cr; GetClientRect(g_hw, &cr);
    const float cw = (float)cr.right, ch = (float)cr.bottom;
    // sidebar: the items share the height between the title and the Play button
    const float nav0 = SF(126);
    const float step = std::max(SF(30), std::min(SF(46), (ch - SF(108) - nav0) / P_COUNT));
    float ny = nav0;
    for (int p = 0; p < P_COUNT; p++) {
        Widget& n = add(W_NAV, RectF(SF(14), ny, SF(SIDEBAR - 28), step - SF(4)), p, 0, false);
        n.title = g_page_name[p]; ny += step;
    }
    g_nav_end = ny;
    Widget& play = add(W_PLAY, RectF(SF(20), ch - SF(96), SF(SIDEBAR - 40), SF(58)), 0, 0, false);
    play.title = "Play"; play.help = "Saves everything and starts the game.";

    g_view = RectF(SF(SIDEBAR), 0, cw - SF(SIDEBAR), ch - SF(FOOT));
    const float x = SF(SIDEBAR + PAD), w = std::min(cw - SF(SIDEBAR + 2 * PAD) - SF(10), SF(720));
    float y = SF(34);
    if (g_page == P_DISPLAY || g_page == P_GRAPHICS || g_page == P_UPSCALE || g_page == P_PERF || g_page == P_AUDIO ||
        g_page == P_UI || g_page == P_ADVANCED) {
        deco(D_H1, RectF(x, y, w, SF(34)), g_page_title[g_page]); y += SF(36);
        deco(D_INTRO, RectF(x, y, w, SF(20)), g_page_intro[g_page]);
        y += SF(40);
        if (g_page == P_DISPLAY || g_page == P_GRAPHICS || g_page == P_UPSCALE) layout_recommend_panel(x, y, w);
        layout_cat_groups(x, y, w, g_page);
        if (g_page == P_PERF) {
            y += SF(10);
            Widget& rb = add(W_BUTTON, RectF(x, y, SF(250), SF(36)), BT_REBUILD_SHADERS);
            rb.title = "Rebuild shader cache";
            rb.help = "Deletes the compiled shader caches made for your graphics driver (shadercache\\rpcs3\\shaders_cache\\"
                      "vk_pipeline_cache.bin, the Fossilize .foz database, r2_gpu_stamp.txt and the native renderer's "
                      "shadercache\\vk_pipeline_cache.bin, also from the backup in %LOCALAPPDATA%). The list of shaders the game "
                      "has used is kept, so the next start compiles them all again. Try it if graphics look corrupted; a new "
                      "GPU or driver is detected and rebuilt automatically.";
            y += SF(36) + SF(8);
        }
    } else if (g_page == P_CONTROLS) {
        deco(D_H1, RectF(x, y, w, SF(34)), "Controls"); y += SF(36);
        deco(D_INTRO, RectF(x, y, w, SF(20)), "Click a key box, then press the key or mouse button you want. "
                                             "Right-click a box to clear it.");
        y += SF(40);
        const float kw = SF(150), gap = SF(10);
        const float kx0 = x + w - 2 * kw - gap;
        deco(D_KEYCOL, RectF(kx0, y, kw, SF(20)), "KEY");
        deco(D_KEYCOL, RectF(kx0 + kw + gap, y, kw, SF(20)), "ALTERNATE");
        const char* grp = "";
        for (int a = 0; a < NACT; a++) {
            if (g_acts[a].hidden) continue;
            if (strcmp(grp, g_acts[a].group)) {
                grp = g_acts[a].group;
                if (a) y += SF(16);
                deco(D_H2, RectF(x, y, w - 2 * kw - gap, SF(22)), grp);
                std::string up = grp; for (auto& c : up) c = (char)toupper((unsigned char)c);
                g_deco.back().s = up;
                y += SF(28);
            }
            Widget& row = add(W_ROW, RectF(x - SF(12), y, w + SF(24), SF(40)), -100 - a);
            row.title = g_acts[a].name; row.help = g_acts[a].help;
            deco(D_LABEL, RectF(x, y, w - 2 * kw - gap - SF(10), SF(40)), g_acts[a].name);
            for (int k = 0; k < NSHOW; k++) {
                Widget& kb = add(W_KEY, RectF(kx0 + k * (kw + gap), y + SF(5), kw, SF(30)), a, k);
                kb.title = g_acts[a].name; kb.help = g_acts[a].help;
            }
            y += SF(40);
        }
        // mouse
        y += SF(30);
        deco(D_H2, RectF(x, y, w, SF(22)), "MOUSE"); y += SF(28);
        {
            Widget& row = add(W_ROW, RectF(x - SF(12), y, w + SF(24), SF(ROWH)), -2);
            row.title = "Mouse sensitivity";
            row.help = "How fast the camera turns. It is also multiplied by the game's own Look Sensitivity option "
                       "(Options \xE2\x80\xBA Controls; the middle setting = 1x).";
            deco(D_LABEL, RectF(x, y, w - SF(CTLW) - SF(16), SF(ROWH)), "Sensitivity");
            Widget& sl = add(W_SLIDER, RectF(x + w - SF(CTLW), y + SF(12), SF(CTLW), SF(ROWH - 24)), SL_SENS);
            sl.title = row.title; sl.help = row.help; y += SF(ROWH);
        }
        {
            Widget& row = add(W_ROW, RectF(x - SF(12), y, w + SF(24), SF(ROWH)), -3);
            row.title = "Invert vertical look";
            row.help = "Moving the mouse forward looks down. Works on top of the game's own Invert option.";
            deco(D_LABEL, RectF(x, y, w - SF(CTLW) - SF(16), SF(ROWH)), "Invert vertical look");
            Widget& tg = add(W_TOGGLE, RectF(x + w - SF(48), y + SF(14), SF(48), SF(24)), TG_INVERT);
            tg.title = row.title; tg.help = row.help; y += SF(ROWH);
        }
        {
            Widget& row = add(W_ROW, RectF(x - SF(12), y, w + SF(24), SF(ROWH)), -4);
            row.title = "Mouse aiming";
            row.help = "Direct: the mouse turns the camera 1:1, like a PC shooter. Classic: the mouse pretends to "
                       "be the right stick, with the PS3's acceleration.";
            deco(D_LABEL, RectF(x, y, w - SF(CTLW) - SF(16), SF(ROWH)), "Mouse aiming");
            Widget& d = add(W_DROP, RectF(x + w - SF(CTLW), y + SF(9), SF(CTLW), SF(ROWH - 18)), DD_MLOOK);
            d.title = row.title; d.help = row.help; y += SF(ROWH);
        }
        {
            Widget& row = add(W_ROW, RectF(x - SF(12), y, w + SF(24), SF(ROWH)), -5);
            row.title = "Free the mouse cursor";
            row.help = "Press in game to let the cursor leave the window (for example to use another program); "
                       "press again to capture it.";
            deco(D_LABEL, RectF(x, y, w - SF(CTLW) - SF(16), SF(ROWH)), "Free the mouse cursor");
            Widget& kb = add(W_KEY, RectF(x + w - kw, y + SF(11), kw, SF(30)), -2, 0);
            kb.title = row.title; kb.help = row.help; y += SF(ROWH);
        }
        // controller
        y += SF(30);
        deco(D_H2, RectF(x, y, w, SF(22)), "CONTROLLER"); y += SF(28);
        deco(D_NOTE, RectF(x, y, w, SF(40)), "Controllers work automatically. Their buttons are set in the game's "
                                             "own Options \xE2\x80\xBA Controls screen.");
        y += SF(44);
        layout_cat_groups(x, y, w, P_CONTROLS, false);
        y += SF(26);
        Widget& b = add(W_BUTTON, RectF(x, y, SF(210), SF(38)), BT_RESET_CTL);
        b.title = "Restore default controls"; b.help = "Puts every key, mouse and controller option back to its default.";
        Widget& b2 = add(W_BUTTON, RectF(x + SF(222), y, SF(190), SF(38)), BT_CONTROLS_TXT);
        b2.title = "Edit controls.txt"; b2.help = "Opens the controls file in Notepad, for anything the page doesn't cover "
                                                  "(one key pressing several buttons, for example).";
        y += SF(38);
    } else if (g_page == P_TROPHIES) {
        lt_layout(x, y, w);
    } else if (g_page == P_TOOLS) {
        extra_layout_tools(x, y, w);
    } else {
        deco(D_H1, RectF(x, y, w, SF(34)), "About"); y += SF(44);
        deco(D_ABOUT_TITLE, RectF(x, y, w, SF(24)), "Resistance 3 \xE2\x80\x94 native PC port"); y += SF(30);
        deco(D_ABOUT_TEXT, RectF(x, y, w, SF(48)),
             "A static recompilation of the PS3 game: its code was translated to run natively on Windows, with a "
             "Vulkan renderer (RPCS3's RSX core) and keyboard & mouse controls.");
        y += SF(58);
        deco(D_ABOUT_TEXT, RectF(x, y, w, SF(48)),
             "Work in progress. It has been tested on one PC and parts of the game are untested and "
             "may crash or look wrong.");
        y += SF(52);
        {
            const std::string ver = dist_read_line(path_of("VERSION"));
            deco(D_ABOUT_TEXT, RectF(x, y, w, SF(44)),
                 "Version " + (ver.empty() ? std::string("unknown") : ver) + " (beta).  This port uses NVIDIA DLSS. "
                 "NVIDIA, GeForce RTX and NVIDIA DLSS are trademarks of NVIDIA Corporation. Third-party "
                 "licences: THIRD_PARTY_NOTICES.md.");
        }
        y += SF(54);
        deco(D_H2, RectF(x, y, w, SF(22)), "IN-GAME KEYS"); y += SF(30);
        static const char* keys[][2] = {
            { "Alt + Enter", "Switch between windowed and fullscreen" },
            { "F8", "Free / capture the mouse cursor (changeable on the Controls page)" },
            { "F10", "Quit to desktop (asks first; waits for a save in progress)" },
            { "F3", "Show / hide the performance overlay" },
            { "View + Menu", "Hold 1.5 s on a controller: quit to desktop (asks first)" },
            { "F12 / PrtScn", "Screenshot (PNG in the screenshots folder)" },
            { "F9", "More PC options: colour filter, HDR, key and controller layout (also in the F10 menu)" },
            { "F7", "Photo mode: pause and hide the overlays for a clean F12 screenshot" },
            { "Hold Space / A", "Hold 1 s during a movie to skip it" },
        };
        for (auto& k : keys) {
            deco(D_LABEL, RectF(x, y, SF(130), SF(30)), k[0]);
            deco(D_LABEL_DIM, RectF(x + SF(140), y, w - SF(140), SF(30)), k[1]);
            y += SF(32);
        }
        y += SF(24);
        deco(D_H2, RectF(x, y, w, SF(22)), "FOLDERS"); y += SF(30);
        Widget& b1 = add(W_BUTTON, RectF(x, y, SF(170), SF(38)), BT_FOLDER);
        b1.title = "Open game folder"; b1.help = "The port's folder: settings, controls.txt and the build.";
        Widget& b2 = add(W_BUTTON, RectF(x + SF(182), y, SF(150), SF(38)), BT_LOGS);
        b2.title = "Open logs"; b2.help = "Log files from the last run. Attach logs\\run.log when reporting a bug.";
        y += SF(50);
        Widget& b3 = add(W_BUTTON, RectF(x, y, SF(170), SF(38)), BT_SAVES);
        b3.title = "Open save folder";
        b3.help = "Your saved games: gamedata\\dev_hdd0\\home\\00000001\\savedata in the game folder. "
                  "Copy this folder to back them up.";
        Widget& b4 = add(W_BUTTON, RectF(x + SF(182), y, SF(190), SF(38)), BT_SHOTS);
        b4.title = "Open screenshots"; b4.help = "Screenshots taken with F12 / PrtScn in game.";
        Widget& br = add(W_BUTTON, RectF(x + SF(384), y, SF(190), SF(38)), BT_REPORT);
        br.title = "Report a problem";
        br.help = "Packs the last crash, logs, settings and PC info into a zip on your Desktop to attach to a bug report.";
        y += SF(38);
        y += SF(30);
        deco(D_H2, RectF(x, y, w, SF(22)), "RESET"); y += SF(30);
        Widget& b5 = add(W_BUTTON, RectF(x, y, SF(210), SF(38)), BT_RESET_ALL);
        b5.title = "Reset all to defaults";
        b5.help = "Puts every launcher setting back to its default: display and graphics (the recommendation for "
                  "your PC), PC options, keys and mouse. Saved games are not touched.";
        y += SF(38);
    }
    g_content_h = y + SF(40);
    const float maxs = std::max(0.0f, g_content_h - g_view.Height);
    g_scroll[g_page] = std::max(0.0f, std::min(g_scroll[g_page], maxs));
}

// widget rect in client coordinates
static RectF wr(const Widget& w)
{
    RectF r = w.r;
    if (w.scroll) r.Y -= g_scroll[g_page];
    return r;
}
static bool visible(const Widget& w)
{
    if (!w.scroll) return true;
    RectF r = wr(w);
    return r.Y + r.Height > g_view.Y && r.Y < g_view.Y + g_view.Height;
}
static int hit(float x, float y)
{
    for (int i = (int)g_w.size() - 1; i >= 0; i--) {
        const Widget& w = g_w[i];
        if (w.scroll && (y < g_view.Y || y >= g_view.Y + g_view.Height)) continue;
        RectF r = wr(w);
        if (x >= r.X && x < r.X + r.Width && y >= r.Y && y < r.Y + r.Height) return i;
    }
    return -1;
}

// ---- painting -------------------------------------------------------------
// The sidebar art: an original picture (a ruined town at dusk under an alien spire's beam), drawn by
// tools/art/make_r3_art.py and embedded as RCDATA 2 in launcher.rc. Nothing from the game is shipped.
// The scaled, faded picture is kept between paints and redone when the size changes.
static Bitmap* g_art;
static Bitmap* g_art_scaled;
static void load_art()
{
    HRSRC r = FindResourceA(nullptr, MAKEINTRESOURCEA(2), MAKEINTRESOURCEA(10) /* RT_RCDATA */);
    if (!r) return;
    const DWORD n = SizeofResource(nullptr, r);
    HGLOBAL h = LoadResource(nullptr, r);
    const void* src = h ? LockResource(h) : nullptr;
    if (!src || !n) return;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!mem) return;
    memcpy(GlobalLock(mem), src, n);
    GlobalUnlock(mem);
    IStream* st = nullptr;
    if (FAILED(CreateStreamOnHGlobal(mem, TRUE, &st))) { GlobalFree(mem); return; }
    Bitmap* b = Bitmap::FromStream(st);
    // GDI+ may read the stream lazily: make a decoded copy and let the stream go
    if (b && b->GetLastStatus() == Ok && b->GetWidth() >= 64 && b->GetHeight() >= 64) {
        Bitmap* c = new Bitmap(b->GetWidth(), b->GetHeight(), PixelFormat32bppPARGB);
        { Graphics cg(c); cg.DrawImage(b, 0, 0, b->GetWidth(), b->GetHeight()); }
        g_art = c;
    }
    delete b;
    st->Release();
}
static void draw_sidebar_art(Graphics& g, float sw, float ch)
{
    if (!g_art) return;
    const float top = std::max(SF(285), g_nav_end - SF(40));
    const int aw = (int)sw, ah = (int)(ch - top);
    if (aw < 16 || ah < 16) return;
    if (!g_art_scaled || (int)g_art_scaled->GetWidth() != aw || (int)g_art_scaled->GetHeight() != ah) {
        delete g_art_scaled;
        g_art_scaled = new Bitmap(aw, ah, PixelFormat32bppPARGB);
        Graphics cg(g_art_scaled);
        cg.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        cg.SetPixelOffsetMode(PixelOffsetModeHalf);
        // the spire and the town (in 480x1000 picture units), cropped to fill the area around the spire
        // (x = 307), centred between the beam and the rooftops; a taller window shows more of both
        const float kx = g_art->GetWidth() / 480.f, ky = g_art->GetHeight() / 1000.f;
        float cw = 480, chh = cw * ah / aw;
        if (chh > 1000) { chh = 1000; cw = chh * aw / ah; }
        const float cx = std::min(480.f - cw, std::max(0.f, 307 - cw / 2)), cy = (1000 - chh) * 0.55f;
        ImageAttributes ia; ia.SetWrapMode(WrapModeTileFlipXY);   // no dark fringe at the edges
        cg.DrawImage(g_art, RectF(0, 0, (REAL)aw, (REAL)ah), cx * kx, cy * ky, cw * kx, chh * ky, UnitPixel, &ia);
        // darkened, and faded in from the sidebar's color under the menu
        const float fade = SF(140), dark = 0.15f;
        for (int y = 0; y < ah; y++) {
            float a = std::min(1.f, y / fade);
            a = a * a * (3 - 2 * a);
            SolidBrush b(Color((BYTE)(255 * (1 - a * (1 - dark))), C_SIDEBAR.GetR(), C_SIDEBAR.GetG(), C_SIDEBAR.GetB()));
            cg.FillRectangle(&b, 0, y, aw, 1);
        }
    }
    g.DrawImage(g_art_scaled, 0.f, top);
}

static void draw_chevron(Graphics& g, float cx, float cy, float sz, const Color& c, bool up = false)
{
    Pen p(c, SF(1.8)); p.SetLineJoin(LineJoinRound); p.SetStartCap(LineCapRound); p.SetEndCap(LineCapRound);
    PointF pts[3] = { PointF(cx - sz, cy + (up ? sz / 2 : -sz / 2)), PointF(cx, cy + (up ? -sz / 2 : sz / 2)),
                      PointF(cx + sz, cy + (up ? sz / 2 : -sz / 2)) };
    g.DrawLines(&p, pts, 3);
}
static float slider_frac(int) { return (float)((g_msens - 0.1) / (4.0 - 0.1)); }
static std::wstring slider_text(int)
{
    wchar_t b[64];
    swprintf(b, 64, L"%.2fx", g_msens);
    return b;
}
static std::string key_text(int a, int k)
{
    if (a == -2) return g_mrelease.empty() ? "" : pretty(g_mrelease);
    return g_bind[a][k].empty() ? "" : pretty(g_bind[a][k]);
}

static void paint(HDC hdc)
{
    RECT cr; GetClientRect(g_hw, &cr);
    Bitmap bmp(cr.right, cr.bottom, PixelFormat32bppPARGB);
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    layout();
    const float cw = (float)cr.right, ch = (float)cr.bottom;
    { SolidBrush b(C_BG); g.FillRectangle(&b, 0.f, 0.f, cw, ch); }

    // ---- content (clipped to the viewport) ----
    g.SetClip(g_view);
    const float sc = g_scroll[g_page];
    for (int i = 0; i < (int)g_w.size(); i++) {      // row highlights under the labels
        const Widget& w = g_w[i];
        if (w.t != W_ROW || !visible(w)) continue;
        RectF r = wr(w);
        bool lit = i == g_hot && !w.disabled;
        if (!lit && g_hot >= 0 && g_w[g_hot].scroll && g_w[g_hot].t != W_ROW && !g_w[g_hot].disabled) {
            RectF h = wr(g_w[g_hot]);
            lit = h.Y >= r.Y && h.Y + h.Height <= r.Y + r.Height + 0.5f && h.X >= r.X;
        }
        if (lit) fill_rr(g, r, SF(8), Color(255, 31, 33, 37));
    }
    for (const Deco& d : g_deco) {
        RectF r = d.r; r.Y -= sc;
        if (r.Y + r.Height < g_view.Y || r.Y > g_view.Y + g_view.Height) continue;
        switch (d.kind) {
        case D_H1: text(g, W(d.s), f_h1, C_TEXT, r); break;
        case D_INTRO: text(g, W(d.s), f_small, C_DIM, r); break;
        case D_H2: {
            text(g, W(d.s), f_h2, C_ACCENT, r);
            break; }
        case D_KEYCOL: text(g, W(d.s), f_tiny, C_FAINT, r, AL_C); break;
        case D_LABEL: text(g, W(d.s), f_body, C_TEXT, r); break;
        case D_LABEL_DIM: text(g, W(d.s), f_body, C_FAINT, r); break;
        case D_NOTE: text(g, W(d.s), f_small, C_DIM, r, AL_L, false, true); break;
        case D_ABOUT_TITLE: text(g, W(d.s), f_btn, C_TEXT, r); break;
        case D_ABOUT_TEXT: text(g, W(d.s), f_body, C_DIM, r, AL_L, false, true); break;
        case D_PANEL: fill_rr(g, r, SF(8), C_PANEL); break;
        case D_REC: text(g, W(d.s), f_body, C_TEXT, r, AL_L, true, false); break;
        case D_TROPHY: lt_paint(g, d.s, r); break;
        }
    }
    for (int i = 0; i < (int)g_w.size(); i++) {
        const Widget& w = g_w[i];
        if (!w.scroll || !visible(w)) continue;
        RectF r = wr(w);
        const bool hot = i == g_hot && !w.disabled;
        switch (w.t) {
        case W_ROW: break;
        case W_DROP: {
            const bool open = g_dd.open && g_dd.kind == w.id && (w.id != DD_SETTING || g_dd.idx == w.sub);
            fill_rr(g, r, SF(6), w.disabled ? C_PANEL : (hot || open) ? C_CTL_HOT : C_CTL);
            if (open) stroke_rr(g, r, SF(6), C_ACCENT, SF(1.2));
            const int cur = dd_sel(w.id, w.sub);
            const std::string lab = dd_label(w.id, w.sub, cur);
            const bool rec = dd_recommended(w.id, w.sub, cur);
            const float tagw = rec ? SF(96) : 0;
            text(g, W(lab), f_body, w.disabled ? C_FAINT : C_TEXT, RectF(r.X + SF(14), r.Y, r.Width - SF(44) - tagw, r.Height));
            if (rec) text(g, L"recommended", f_tiny, w.disabled ? C_FAINT : C_ACCENT, RectF(r.X + r.Width - SF(36) - tagw, r.Y, tagw, r.Height), AL_R);
            draw_chevron(g, r.X + r.Width - SF(20), r.Y + r.Height / 2, SF(5), w.disabled ? C_FAINT : C_DIM, open);
            break; }
        case W_SLIDER: {
            const float f = slider_frac(w.id);
            const float tx = r.X + SF(8), tw = r.Width - SF(100), ty = r.Y + r.Height / 2;
            fill_rr(g, RectF(tx, ty - SF(2), tw, SF(4)), SF(2), C_CTL_HOT);
            fill_rr(g, RectF(tx, ty - SF(2), tw * f, SF(4)), SF(2), C_ACCENT);
            const float kr = (hot || g_drag == w.id) ? SF(9) : SF(8);
            SolidBrush kb(g_drag == w.id ? C_ACCENT_HOT : C_TEXT);
            g.FillEllipse(&kb, tx + tw * f - kr, ty - kr, 2 * kr, 2 * kr);
            text(g, slider_text(w.id), f_body, C_TEXT, RectF(r.X + r.Width - SF(84), r.Y, SF(84), r.Height), AL_R);
            break; }
        case W_TOGGLE: {
            const bool on = w.id == TG_INVERT ? g_minv != 0 : false;
            fill_rr(g, r, r.Height / 2, on ? C_ACCENT : (hot ? C_CTL_HOT : C_CTL));
            if (!on) stroke_rr(g, r, r.Height / 2, C_BORDER, SF(1));
            const float d = r.Height - SF(8);
            SolidBrush kb(on ? C_ON_ACCENT : C_DIM);
            g.FillEllipse(&kb, on ? r.X + r.Width - d - SF(4) : r.X + SF(4), r.Y + SF(4), d, d);
            break; }
        case W_KEY: {
            const bool cap = (w.id == g_cap_a && w.sub == g_cap_k);
            const std::string t = key_text(w.id, w.sub);
            if (cap) fill_rr(g, r, SF(6), C_ACCENT_DIM);
            else if (t.empty()) { if (hot) fill_rr(g, r, SF(6), C_CTL); stroke_rr(g, r, SF(6), hot ? C_BORDER : Color(255, 44, 47, 52), SF(1)); }
            else fill_rr(g, r, SF(6), hot ? C_CTL_HOT : C_CTL);
            if (cap) stroke_rr(g, r, SF(6), C_ACCENT, SF(1.4));
            if (cap) text(g, L"Press a key\x2026", f_small, C_ACCENT_HOT, r, AL_C);
            else if (t.empty()) text(g, L"\x2014", f_body, C_FAINT, r, AL_C);
            else text(g, W(t), f_body, C_TEXT, r, AL_C);
            break; }
        case W_BUTTON: {
            fill_rr(g, r, SF(6), hot ? C_CTL_HOT : C_CTL);
            stroke_rr(g, r, SF(6), C_BORDER, SF(1));
            text(g, W(w.title), f_btn, C_TEXT, r, AL_C);
            break; }
        default: break;
        }
    }
    g.ResetClip();

    // scrollbar
    if (g_content_h > g_view.Height + 1) {
        const float trackH = g_view.Height - SF(16);
        const float th = std::max(SF(40), trackH * g_view.Height / g_content_h);
        const float ty = g_view.Y + SF(8) + (trackH - th) * (g_scroll[g_page] / (g_content_h - g_view.Height));
        fill_rr(g, RectF(cw - SF(10), ty, SF(5), th), SF(2.5), Color(140, 120, 126, 134));
    }

    // ---- help strip ----
    {
        RectF fr(SF(SIDEBAR), ch - SF(FOOT), cw - SF(SIDEBAR), SF(FOOT));
        SolidBrush b(C_PANEL); g.FillRectangle(&b, fr);
        Pen ln(Color(255, 44, 47, 52), SF(1)); g.DrawLine(&ln, fr.X, fr.Y, fr.X + fr.Width, fr.Y);
        const float x = SF(SIDEBAR + PAD), w = fr.Width - SF(2 * PAD);
        std::wstring t1, t2;
        Color c1 = C_TEXT;
        if (g_cap_a != -1) {
            t1 = L"Press the key or mouse button to use";
            t2 = L"Any key works, including Esc. Click anywhere else to cancel.";
            c1 = C_ACCENT_HOT;
        } else if (!g_toast.empty() && GetTickCount() < g_toast_until) {
            t1 = g_toast; c1 = C_WARN;
        } else if (g_hot >= 0 && !g_w[g_hot].help.empty() && g_w[g_hot].t != W_NAV) {
            t1 = W(g_w[g_hot].title); t2 = W(g_w[g_hot].help);
        } else if (g_hot >= 0 && g_w[g_hot].t == W_KEY) {
            t1 = W(g_w[g_hot].title); t2 = L"Click to change. Right-click to clear.";
        } else {
            t2 = g_page == P_CONTROLS ? L"Hover over anything to see what it does. Changes are saved automatically."
               : L"Hover over a setting to see what it does. Changes are saved automatically and apply the next "
                 L"time the game starts.";
        }
        if (!t1.empty()) text(g, t1, f_btn, c1, RectF(x, fr.Y + SF(12), w, SF(22)));
        if (!t2.empty()) text(g, t2, f_small, C_DIM, RectF(x, fr.Y + (t1.empty() ? SF(14) : SF(36)), w, SF(36)),
                              AL_L, false, true);
    }

    // ---- sidebar ----
    {
        SolidBrush b(C_SIDEBAR); g.FillRectangle(&b, 0.f, 0.f, SF(SIDEBAR), ch);
        draw_sidebar_art(g, SF(SIDEBAR), ch);
        {   // the title, shrunk if the font is wider than the sidebar
            const float avail = SF(SIDEBAR - 46);
            RectF m; g.MeasureString(L"RESISTANCE", -1, f_title, PointF(0, 0), &m);
            if (m.Width > avail) {
                FontFamily ff; f_title->GetFamily(&ff);
                Font fsmall(&ff, f_title->GetSize() * avail / m.Width, f_title->GetStyle(), UnitPixel);
                text(g, L"RESISTANCE", &fsmall, C_TEXT, RectF(SF(24), SF(34), SF(SIDEBAR - 30), SF(44)));
            } else text(g, L"RESISTANCE", f_title, C_TEXT, RectF(SF(24), SF(34), SF(SIDEBAR - 30), SF(44)));
        }
        {   // a cyan rule with the "3" set into it
            SolidBrush rb(C_ACCENT_DIM); g.FillRectangle(&rb, SF(26), SF(86), SF(SIDEBAR - 56), SF(1.5));
            SolidBrush cb(C_SIDEBAR); g.FillRectangle(&cb, SF(22), SF(74), SF(30), SF(24));
            text(g, L"3", f_h1, C_ACCENT, RectF(SF(25), SF(72), SF(30), SF(28)), AL_L, true);
        }
        text(g, L"PC port \x00B7 work in progress", f_tiny, C_FAINT, RectF(SF(26), SF(98), SF(SIDEBAR - 30), SF(18)));
        for (int i = 0; i < (int)g_w.size(); i++) {
            const Widget& w = g_w[i];
            if (w.t == W_NAV) {
                const bool sel = w.id == g_page, hot = i == g_hot;
                if (sel) fill_rr(g, w.r, SF(8), Color(255, 28, 38, 44));
                else if (hot) fill_rr(g, w.r, SF(8), Color(255, 22, 29, 34));
                if (sel) fill_rr(g, RectF(w.r.X, w.r.Y + SF(10), SF(3), w.r.Height - SF(20)), SF(1.5), C_ACCENT);
                text(g, W(w.title), f_nav, sel ? C_TEXT : hot ? Color(255, 205, 208, 212) : C_DIM,
                     RectF(w.r.X + SF(18), w.r.Y, w.r.Width - SF(18), w.r.Height));
            } else if (w.t == W_PLAY) {
                const bool hot = i == g_hot, down = i == g_press;
                fill_rr(g, w.r, SF(8), down ? C_ACCENT : hot ? C_ACCENT_HOT : C_ACCENT);
                text(g, L"PLAY", f_play, C_ON_ACCENT, w.r, AL_C);
            }
        }
    }

    // ---- dropdown list (on top of everything) ----
    if (g_dd.open) {
        RectF lr = g_dd.list;
        { GraphicsPath p; round_rect(p, RectF(lr.X + SF(2), lr.Y + SF(4), lr.Width, lr.Height), SF(8));
          SolidBrush sh(Color(110, 0, 0, 0)); g.FillPath(&sh, &p); }
        fill_rr(g, lr, SF(8), Color(255, 44, 47, 53));
        stroke_rr(g, lr, SF(8), C_BORDER, SF(1));
        const int n = dd_count(g_dd.kind, g_dd.idx), cur = dd_sel(g_dd.kind, g_dd.idx);
        for (int i = 0; i < n; i++) {
            RectF ir(lr.X + SF(4), lr.Y + SF(4) + i * g_dd.item_h, lr.Width - SF(8), g_dd.item_h);
            if (i == g_dd.hot) fill_rr(g, ir, SF(5), C_CTL_HOT);
            if (i == cur) fill_rr(g, RectF(ir.X + SF(4), ir.Y + ir.Height / 2 - SF(7), SF(3), SF(14)), SF(1.5), C_ACCENT);
            const bool rec = dd_recommended(g_dd.kind, g_dd.idx, i);
            const float tagw = rec ? SF(96) : 0;
            text(g, W(dd_label(g_dd.kind, g_dd.idx, i)), f_body, i == cur ? C_TEXT : Color(255, 214, 216, 220),
                 RectF(ir.X + SF(16), ir.Y, ir.Width - SF(20) - tagw, ir.Height));
            if (rec) text(g, L"recommended", f_tiny, C_ACCENT, RectF(ir.X + ir.Width - SF(8) - tagw, ir.Y, tagw, ir.Height), AL_R);
        }
    }

    // ---- offline shader scan progress (on top of everything) ----
    if (g_scan.active) {
        std::string phase; unsigned d, t; bool ran;
        AcquireSRWLockShared(&g_scan_lock);
        phase = g_scan.phase; d = g_scan.done; t = g_scan.total; ran = g_scan.ran;
        ReleaseSRWLockShared(&g_scan_lock);
        if (ran || GetTickCount() - g_scan.t0 >= 400) {   // a quick "already done" check never flashes the panel
            SolidBrush dim(Color(175, 6, 8, 10)); g.FillRectangle(&dim, 0.f, 0.f, cw, ch);
            const float pw = std::min(cw - SF(80), SF(580)), ph = SF(196);
            RectF pr((cw - pw) / 2, (ch - ph) / 2, pw, ph);
            fill_rr(g, pr, SF(10), C_PANEL); stroke_rr(g, pr, SF(10), C_BORDER, SF(1));
            const float ix = pr.X + SF(24), iw = pr.Width - SF(48);
            text(g, L"Preparing shaders from your game files", f_h2, C_TEXT, RectF(ix, pr.Y + SF(18), iw, SF(28)));
            std::string sub = !ran ? std::string("Checking the shader cache...")
                            : t ? phase + "  " + std::to_string(std::min(d, t)) + " / " + std::to_string(t)
                                : (phase.empty() ? std::string("scan") : phase) + "...";
            text(g, W(sub), f_body, C_DIM, RectF(ix, pr.Y + SF(52), iw, SF(22)));
            RectF br(ix, pr.Y + SF(84), iw, SF(8));
            fill_rr(g, br, SF(4), C_CTL);
            if (t) fill_rr(g, RectF(br.X, br.Y, std::max(SF(8), br.Width * std::min(d, t) / t), br.Height), SF(4), C_ACCENT);
            else {   // total not known yet: a moving segment
                const float seg = br.Width / 4, pos = (float)((GetTickCount() / 8) % 1000) / 1000.f * (br.Width + seg) - seg;
                const float l = std::max(br.X, br.X + pos), r = std::min(br.X + br.Width, br.X + pos + seg);
                if (r > l) fill_rr(g, RectF(l, br.Y, r - l, br.Height), SF(4), C_ACCENT);
            }
            text(g, L"Once per install and after Rebuild shader cache: the shaders in your game files are added to the "
                    L"shader cache, so Compile shaders before playing covers the whole game. The game starts when done.",
                 f_small, C_FAINT, RectF(ix, pr.Y + SF(102), iw, SF(40)), AL_L, false, true);
            g_scan.skip_btn = RectF(pr.X + pr.Width - SF(24) - SF(150), pr.Y + ph - SF(50), SF(150), SF(34));
            fill_rr(g, g_scan.skip_btn, SF(7), C_CTL); stroke_rr(g, g_scan.skip_btn, SF(7), C_BORDER, SF(1));
            text(g, L"Skip for now", f_btn, C_TEXT, g_scan.skip_btn, AL_C);
        } else g_scan.skip_btn = RectF();
    }

    Graphics screen(hdc);
    screen.DrawImage(&bmp, 0, 0);
}

// ---- interaction ----------------------------------------------------------
static void invalidate() { InvalidateRect(g_hw, NULL, FALSE); }

static void open_dropdown(const Widget& w)
{
    RECT cr; GetClientRect(g_hw, &cr);
    RectF a = wr(w);
    g_dd.open = true; g_dd.kind = w.id; g_dd.idx = w.sub; g_dd.anchor = a;
    g_dd.item_h = SF(32);
    const int n = dd_count(w.id, w.sub);
    const float h = n * g_dd.item_h + SF(8);
    // over the box, with the current choice where the box is (like a Windows menu)
    const int cur = dd_sel(w.id, w.sub);
    float y = a.Y + (a.Height - g_dd.item_h) / 2 - SF(4) - cur * g_dd.item_h;
    y = std::max(SF(8), std::min(y, (float)cr.bottom - SF(8) - h));
    g_dd.list = RectF(a.X, y, a.Width, h);
    g_dd.hot = dd_sel(w.id, w.sub);
}
static void close_dropdown() { g_dd.open = false; g_dd.hot = -1; }
static int dd_hit(float x, float y)
{
    const RectF& lr = g_dd.list;
    if (x < lr.X || x >= lr.X + lr.Width || y < lr.Y + SF(4) || y >= lr.Y + lr.Height - SF(4)) return -1;
    const int i = (int)((y - lr.Y - SF(4)) / g_dd.item_h);
    return i < dd_count(g_dd.kind, g_dd.idx) ? i : -1;
}

static void slider_set(int id, float x)
{
    for (const Widget& w : g_w) if (w.t == W_SLIDER && w.id == id) {
        RectF r = wr(w);
        const float tx = r.X + SF(8), tw = r.Width - SF(100);
        float f = (x - tx) / tw; f = std::max(0.0f, std::min(1.0f, f));
        int st = (int)lround((0.1 + f * 3.9) * 20); g_msens = std::max(2, std::min(80, st)) / 20.0;
    }
}
static void slider_commit(int) { ctl_save(); }

static void capture_done(const char* key)
{
    if (g_cap_a == -1) return;
    if (key) {
        if (g_cap_a == -2) { g_mrelease = key; }
        else {
            // one key, one action: a key taken from another action moves here
            std::string from;
            for (int a = 0; a < NACT; a++) for (int k = 0; k < NSLOT; k++)
                if (!_stricmp(g_bind[a][k].c_str(), key) && !(a == g_cap_a && k == g_cap_k)) {
                    g_bind[a][k].clear(); if (!g_acts[a].hidden) from = g_acts[a].name; }
            g_bind[g_cap_a][g_cap_k] = key;
            if (!from.empty()) toast(pretty(key) + " was removed from \"" + from + "\" and now does \"" + g_acts[g_cap_a].name + "\".");
            else if (!_stricmp(key, g_mrelease.c_str()))
                toast("Note: " + pretty(key) + " also frees the mouse cursor in game.");
        }
        ctl_save();
    }
    g_cap_a = g_cap_k = -1;
    invalidate();
}

static const char* g_cmd = nullptr;     // command line (the --sim-* test hooks apply to re-detection too)

static void restore_graphics()
{
    detect_hardware(g_cmd);
    for (auto& s : g_set) s.sel = s.def;
    save_settings();
    toast("Recommended for your PC: " + g_rec_desc);
}

// x86-64-v3 (Haswell / Zen and newer: AVX2, FMA, BMI1/2, LZCNT, MOVBE, F16C) with OS AVX
// support -- what the shipped build needs. Without it run.bat starts
// build_baseline\Resistance3.exe when that build exists (build_cpu.bat baseline).
static bool cpu_is_x86_64_v3()
{
    int r[4];
    __cpuid(r, 0); if (r[0] < 7) return false;
    __cpuid(r, 1);
    const unsigned c1 = (unsigned)r[2];
    if (!(c1 >> 27 & 1) || !(c1 >> 28 & 1) || !(c1 >> 12 & 1) || !(c1 >> 22 & 1) || !(c1 >> 29 & 1)) return false;
    if ((_xgetbv(0) & 6) != 6) return false;
    __cpuidex(r, 7, 0);
    if (!((unsigned)r[1] >> 5 & 1) || !((unsigned)r[1] >> 8 & 1) || !((unsigned)r[1] >> 3 & 1)) return false;
    __cpuid(r, 0x80000001);
    return ((unsigned)r[2] >> 5 & 1) != 0;
}

static void play_launch()
{
    lx_auto_backup();                       // launcher_extra.inc
    SetEnvironmentVariableA("PORT_CPU_V3", cpu_is_x86_64_v3() ? "1" : "0");   // read by run.bat
    if ((INT_PTR)ShellExecuteA(g_hw, "open", path_of("run.bat").c_str(), NULL, g_dir, SW_HIDE) <= 32) {
        MessageBoxA(g_hw, "Could not start run.bat. Is the game built? (Run Setup.bat first.)", "Launcher", MB_ICONERROR);
        return;
    }
    DestroyWindow(g_hw);
}

// "Rebuild shader cache": deletes the driver-specific compiled caches, keeps the shader lists that say what to build
// (shadercache\rpcs3\shaders_cache\pipelines, raw, and the native renderer's variant / program lists). run.bat restores
// missing files from the %LOCALAPPDATA% backup, so the same files go there too.
#define SHC_GAME_EXE "Resistance3.exe"
#define SHC_BACKUP_DIR "Resistance3Port\\shadercache"
static const char* const g_shc_rebuild_files[] = {
    "rpcs3\\shaders_cache\\vk_pipeline_cache.bin",       // RPCS3 renderer: VkPipelineCache (driver blobs)
    "rpcs3\\shaders_cache\\vk_pipeline_cache.bin.tmp",
    "rpcs3\\shaders_cache\\r2_gpu_stamp.txt",            // which GPU / driver built it
    "rpcs3\\shaders_cache\\r2_pipelines.foz",            // Fossilize database (R2_FOSSILIZE)
    "rpcs3\\shaders_cache\\r3_pipelines.foz",
    "rpcs3\\shaders_cache\\shaderscan.stamp",         // offline shader scan done (runs again on the next Play)
    "vk_pipeline_cache.bin",                               // native Vulkan renderer: VkPipelineCache
};
static bool shc_game_running()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe; pe.dwSize = sizeof pe;
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        found = _wcsicmp(pe.szExeFile, L"" SHC_GAME_EXE) == 0;
    CloseHandle(snap);
    return found;
}
static void rebuild_shader_cache()
{
    if (shc_game_running()) {
        MessageBoxA(g_hw, "The game is running. Close it first, then rebuild the shader cache.", "Rebuild shader cache", MB_ICONINFORMATION);
        return;
    }
    if (MessageBoxA(g_hw, "Delete the compiled shader caches for your graphics driver?\n\n"
                    "The list of shaders the game has used is kept: the next time you play, every shader is compiled again "
                    "(with \"Compile shaders before playing\" on, before play begins - this can take a minute).\n\n"
                    "Deleted from shadercache\\ and its backup in %LOCALAPPDATA%\\" SHC_BACKUP_DIR ":\n"
                    "  rpcs3\\shaders_cache\\vk_pipeline_cache.bin, r2_gpu_stamp.txt, *.foz\n"
                    "  vk_pipeline_cache.bin (native Vulkan renderer)\n\n"
                    "With shaderscan.exe installed, the game files are also scanned for shaders again before the next start.",
                    "Rebuild shader cache", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    std::vector<std::string> roots = { path_of("shadercache") };
    char la[MAX_PATH];
    if (GetEnvironmentVariableA("LOCALAPPDATA", la, MAX_PATH)) roots.push_back(std::string(la) + "\\" SHC_BACKUP_DIR);
    int n = 0, failed = 0; unsigned long long bytes = 0;
    for (const std::string& r : roots)
        for (const char* f : g_shc_rebuild_files) {
            const std::string p = r + "\\" + f;
            WIN32_FILE_ATTRIBUTE_DATA fa;
            if (!GetFileAttributesExA(p.c_str(), GetFileExInfoStandard, &fa)) continue;
            if (DeleteFileA(p.c_str())) { n++; bytes += ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow; }
            else failed++;
        }
    char b[200];
    if (failed) snprintf(b, sizeof b, "Shader cache: %d file(s) deleted, %d could not be deleted (in use?).", n, failed);
    else if (n) snprintf(b, sizeof b, "Shader cache cleared (%d file(s), %.0f MB). Shaders are rebuilt the next time you play.", n, bytes / 1048576.0);
    else snprintf(b, sizeof b, "The shader cache was already empty. Shaders are built the next time you play.");
    toast(b);
}

// ---- offline shader scan (shaderscan.exe; contract: scratch\shaderscan_launcher_contract.md) ----
// When shaderscan.exe is next to the launcher (or in tools\), Play first lets it add the shaders found in the game
// files to shadercache\rpcs3\shaders_cache (once: it leaves shaderscan.stamp there, which Rebuild shader cache
// deletes), then starts the game, whose start-up preload ("Compile shaders before playing") compiles them. The scan
// runs on a worker thread and the window keeps pumping messages; Skip for now kills it (it runs again next time).
static std::string shaderscan_exe()
{
    for (const char* p : { "shaderscan.exe", "tools\\shaderscan.exe" })
        if (GetFileAttributesA(path_of(p).c_str()) != INVALID_FILE_ATTRIBUTES) return path_of(p);
    return {};
}
static void scan_line(const std::string& l, FILE* log)
{
    if (l.empty()) return;
    char ph[32] = ""; unsigned d = 0, t = 0;
    const bool prog = sscanf(l.c_str(), "@progress %31s %u %u", ph, &d, &t) == 3 ||
                      sscanf(l.c_str(), "[%31[^]]] %u/%u", ph, &d, &t) == 3;
    bool changed = false;
    AcquireSRWLockExclusive(&g_scan_lock);
    if (prog) { changed = g_scan.phase != ph; g_scan.phase = ph; g_scan.done = d; g_scan.total = t; }
    else if (!strncmp(l.c_str(), "@error ", 7)) g_scan.error = l.substr(7);
    else if (!strncmp(l.c_str(), "shaderscan: ", 12)) g_scan.error = l.substr(12);
    ReleaseSRWLockExclusive(&g_scan_lock);
    if (log && (!prog || changed || d == t)) { fprintf(log, "%s\n", l.c_str()); fflush(log); }
    if (prog) {
        static DWORD last = 0;
        const DWORD now = GetTickCount();
        if (changed || d == t || now - last >= 50) { last = now; PostMessageA(g_hw, WM_SCAN_PROGRESS, 0, 0); }
    }
}
// Runs one shaderscan command with stdout + stderr on a pipe; returns its exit code (-1: could not run / timed out).
static int scan_exec(std::string cl, DWORD timeout_ms, FILE* log)
{
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si = {}; si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = si.hStdError = wr;
    PROCESS_INFORMATION pi = {};
    if (log) { fprintf(log, "> %s\n", cl.c_str()); fflush(log); }
    if (!CreateProcessA(NULL, &cl[0], NULL, NULL, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, NULL, g_dir, &si, &pi)) {
        if (log) fprintf(log, "could not start it (error %lu)\n", GetLastError());
        CloseHandle(rd); CloseHandle(wr); return -1;
    }
    CloseHandle(wr); CloseHandle(pi.hThread);
    AcquireSRWLockExclusive(&g_scan_lock);
    g_scan.proc = pi.hProcess;
    if (g_scan.cancel) TerminateProcess(pi.hProcess, 1);
    ReleaseSRWLockExclusive(&g_scan_lock);
    bool timed_out = false;
    if (timeout_ms != INFINITE && WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1); timed_out = true;
    }
    std::string line; char buf[4096]; DWORD n = 0;
    while (ReadFile(rd, buf, sizeof buf, &n, NULL) && n)
        for (DWORD i = 0; i < n; i++) {
            if (buf[i] == '\r' || buf[i] == '\n') { scan_line(line, log); line.clear(); }
            else if (line.size() < 4096) line += buf[i];
        }
    scan_line(line, log);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    AcquireSRWLockExclusive(&g_scan_lock);
    g_scan.proc = NULL;
    ReleaseSRWLockExclusive(&g_scan_lock);
    CloseHandle(pi.hProcess);
    if (log) { fprintf(log, "exit %d%s\n", (int)code, timed_out ? " (timed out)" : ""); fflush(log); }
    return timed_out ? -1 : (int)code;
}
static DWORD WINAPI scan_thread(LPVOID)
{
    const std::string exe = shaderscan_exe(), out = path_of("shadercache\\rpcs3\\shaders_cache");
    CreateDirectoryA(path_of("logs").c_str(), NULL);
    FILE* log = fopen(path_of("logs\\shaderscan.log").c_str(), "w");
    // --check: 0 = this version already seeded the cache, 1 = run it; anything else (or > 10 s) = skip this time
    int rc = scan_exec("\"" + exe + "\" --check --out \"" + out + "\"", 10000, log);
    bool cancelled;
    AcquireSRWLockShared(&g_scan_lock); cancelled = g_scan.cancel; ReleaseSRWLockShared(&g_scan_lock);
    if (rc == 1 && !cancelled) {
        AcquireSRWLockExclusive(&g_scan_lock); g_scan.ran = true; g_scan.phase = "scan"; ReleaseSRWLockExclusive(&g_scan_lock);
        PostMessageA(g_hw, WM_SCAN_PROGRESS, 0, 0);
        rc = scan_exec("\"" + exe + "\" --game \"" + g_dir + "\" --out \"" + out + "\" --progress", INFINITE, log);
    } else rc = 0;
    g_scan.exit_code = rc;
    if (log) fclose(log);
    PostMessageA(g_hw, WM_SCAN_DONE, 0, 0);
    return 0;
}
static void scan_cancel()
{
    AcquireSRWLockExclusive(&g_scan_lock);
    g_scan.cancel = true;
    if (g_scan.proc) TerminateProcess(g_scan.proc, 1);
    ReleaseSRWLockExclusive(&g_scan_lock);
}
static void scan_finish()
{
    if (g_scan.thread) { WaitForSingleObject(g_scan.thread, INFINITE); CloseHandle(g_scan.thread); g_scan.thread = NULL; }
    KillTimer(g_hw, 2);
    g_scan.active = false;
    if (g_scan.quit) { DestroyWindow(g_hw); return; }
    play_launch();    // the game starts also when the scan failed or was skipped (logs\shaderscan.log has the details)
}

static void play()
{
    if (g_scan.active) return;
    save_settings(); ctl_save();
    if (!g_scan.checked && !shaderscan_exe().empty()) {
        g_scan.checked = true;
        g_scan.cancel = g_scan.ran = g_scan.quit = false;
        g_scan.phase.clear(); g_scan.error.clear(); g_scan.done = g_scan.total = 0;
        g_scan.t0 = GetTickCount();
        g_scan.active = true;
        g_scan.thread = CreateThread(NULL, 0, scan_thread, NULL, 0, NULL);
        if (g_scan.thread) { SetTimer(g_hw, 2, 100, NULL); InvalidateRect(g_hw, NULL, FALSE); return; }
        g_scan.active = false;
    }
    play_launch();
}

static void click(int i, float x, float y)
{
    const Widget& w = g_w[i];
    if (w.disabled) return;
    switch (w.t) {
    case W_NAV: if (g_page != w.id) { g_page = w.id; close_dropdown(); } break;
    case W_PLAY: play(); return;
    case W_DROP: open_dropdown(w); break;
    case W_SLIDER: g_drag = w.id; SetCapture(g_hw); slider_set(w.id, x); break;
    case W_TOGGLE: if (w.id == TG_INVERT) { g_minv = !g_minv; ctl_save(); } break;
    case W_KEY: g_cap_a = w.id; g_cap_k = w.sub; break;
    case W_BUTTON:
        if (extra_click(w.id, w.sub)) break;    // launcher_extra.inc
        if (w.id == BT_RESET_CTL) {
            if (MessageBoxA(g_hw, "Put every key, mouse and controller option back to its default?", "Controls",
                            MB_YESNO | MB_ICONQUESTION) == IDYES) { ctl_defaults(); ctl_save(); toast("Controls restored to their defaults."); }
        } else if (w.id == BT_RESET_GFX) restore_graphics();
        else if (w.id == BT_FOLDER) ShellExecuteA(g_hw, "open", g_dir, NULL, NULL, SW_SHOWNORMAL);
        else if (w.id == BT_REPORT) port_report_ui(g_hw);
        else if (w.id == BT_LOGS) { CreateDirectoryA(path_of("logs").c_str(), NULL);
                                    ShellExecuteA(g_hw, "open", path_of("logs").c_str(), NULL, NULL, SW_SHOWNORMAL); }
        else if (w.id == BT_CONTROLS_TXT) { ctl_save(); ShellExecuteA(g_hw, "open", "notepad.exe", path_of("controls.txt").c_str(), NULL, SW_SHOWNORMAL); }
        else if (w.id == BT_SAVES) {
            const char* p = getenv("PS3_GAMEDATA_ROOT");
            std::string d = (p && *p) ? std::string(p) : path_of("gamedata");
            for (const char* part : { "\\dev_hdd0", "\\home", "\\00000001", "\\savedata" }) {
                d += part; CreateDirectoryA(d.c_str(), NULL);
            }
            ShellExecuteA(g_hw, "open", d.c_str(), NULL, NULL, SW_SHOWNORMAL);
        } else if (w.id == BT_SHOTS) { CreateDirectoryA(path_of("screenshots").c_str(), NULL);
                                       ShellExecuteA(g_hw, "open", path_of("screenshots").c_str(), NULL, NULL, SW_SHOWNORMAL); }
        else if (w.id == BT_REBUILD_SHADERS) rebuild_shader_cache();
        else if (w.id == BT_RESET_ALL) {
            if (MessageBoxA(g_hw, "Put every launcher setting (display, graphics, PC options, keys and mouse) back to its "
                            "default? Saved games are not touched.", "Reset", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                ctl_defaults(); ctl_save(); restore_graphics(); toast("Everything reset to defaults.");
            }
        }
        break;
    default: break;
    }
    (void)y;
    invalidate();
}

static void set_hot(int i)
{
    if (i == g_hot) return;
    g_hot = i; invalidate();
}

static void scroll_by(float d)
{
    if (g_content_h <= g_view.Height) return;
    g_scroll[g_page] = std::max(0.0f, std::min(g_scroll[g_page] + d, g_content_h - g_view.Height));
    invalidate();
}

static void make_scale(HWND hw)
{
    UINT dpi = 96;
    typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
    static GetDpiForWindowFn fn = (GetDpiForWindowFn)(void*)GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow");
    if (fn && hw) dpi = fn(hw);
    else { HDC dc = GetDC(NULL); dpi = GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(NULL, dc); }
    g_s = dpi / 96.0;
    make_fonts();
}

static LRESULT CALLBACK wndproc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_scan.active) {   // offline shader scan running: only Skip for now / closing the window
        switch (msg) {
        case WM_LBUTTONDOWN:
            if (g_scan.skip_btn.Contains((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp))) scan_cancel();
            return 0;
        case WM_KEYDOWN: if (wp == VK_ESCAPE) scan_cancel(); return 0;
        case WM_MOUSEMOVE: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN: case WM_LBUTTONUP:
        case WM_MOUSEWHEEL: case WM_CHAR: return 0;
        case WM_CLOSE: g_scan.quit = true; scan_cancel(); return 0;
        default: break;
        }
    }
    switch (msg) {
    case WM_SCAN_PROGRESS: invalidate(); return 0;
    case WM_SCAN_DONE: scan_finish(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hw, &ps); paint(dc); EndPaint(hw, &ps); return 0; }
    case WM_SIZE: invalidate(); return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mi = (MINMAXINFO*)lp;
        mi->ptMinTrackSize.x = S(860); mi->ptMinTrackSize.y = S(600); return 0; }
    case 0x02E0: {   // WM_DPICHANGED
        g_s = HIWORD(wp) / 96.0; make_fonts();
        RECT* r = (RECT*)lp;
        SetWindowPos(hw, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        invalidate(); return 0; }
    case WM_TIMER:
        if (wp == 2) { invalidate(); return 0; }   // shader scan progress animation
        KillTimer(hw, 1); g_toast.clear(); invalidate(); return 0;
    case WM_MOUSEMOVE: {
        const float x = (float)GET_X_LPARAM(lp), y = (float)GET_Y_LPARAM(lp);
        TRACKMOUSEEVENT tme = { sizeof tme, TME_LEAVE, hw, 0 }; TrackMouseEvent(&tme);
        if (g_drag) { slider_set(g_drag, x); invalidate(); return 0; }
        if (g_dd.open) { const int h = dd_hit(x, y); if (h != g_dd.hot) { g_dd.hot = h; invalidate(); } return 0; }
        layout(); set_hot(hit(x, y));
        return 0; }
    case WM_MOUSELEAVE: if (!g_drag) set_hot(-1); return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN: {
        const float x = (float)GET_X_LPARAM(lp), y = (float)GET_Y_LPARAM(lp);
        layout();
        if (g_cap_a != -1) {
            // a click on the box being set binds that button; a click anywhere else cancels
            int i = hit(x, y);
            const bool on_box = i >= 0 && g_w[i].t == W_KEY && g_w[i].id == g_cap_a && g_w[i].sub == g_cap_k;
            if (!on_box && msg == WM_LBUTTONDOWN) { g_cap_a = g_cap_k = -1; invalidate(); return 0; }
            capture_done(msg == WM_LBUTTONDOWN ? "MOUSE1" : msg == WM_RBUTTONDOWN ? "MOUSE2" : msg == WM_MBUTTONDOWN ? "MOUSE3"
                         : GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? "MOUSE4" : "MOUSE5");
            return msg == WM_XBUTTONDOWN ? TRUE : 0;
        }
        if (g_dd.open) {
            const int h = dd_hit(x, y);
            if (h >= 0 && msg == WM_LBUTTONDOWN) dd_set(g_dd.kind, g_dd.idx, h);
            close_dropdown(); invalidate(); return 0;
        }
        const int i = hit(x, y);
        if (msg == WM_RBUTTONDOWN && i >= 0 && g_w[i].t == W_KEY) {     // right-click clears
            if (g_w[i].id == -2) { g_mrelease = "F8"; toast("The cursor key can't be empty: it's back to F8."); }
            else g_bind[g_w[i].id][g_w[i].sub].clear();
            ctl_save(); invalidate(); return 0;
        }
        if (msg == WM_LBUTTONDOWN && i >= 0) {
            if (g_w[i].t == W_PLAY) { g_press = i; SetCapture(hw); invalidate(); return 0; }
            click(i, x, y);
        }
        return 0; }
    case WM_LBUTTONUP: {
        const float x = (float)GET_X_LPARAM(lp), y = (float)GET_Y_LPARAM(lp);
        if (g_drag) { const int d = g_drag; g_drag = 0; ReleaseCapture(); slider_commit(d); invalidate(); return 0; }
        if (g_press >= 0) {
            const int p = g_press; g_press = -1; ReleaseCapture(); layout();
            if (hit(x, y) == p) { click(p, x, y); return 0; }
            invalidate();
        }
        return 0; }
    case WM_MOUSEWHEEL: {
        const int d = GET_WHEEL_DELTA_WPARAM(wp);
        if (g_cap_a != -1) { capture_done(d > 0 ? "WHEELUP" : "WHEELDOWN"); return 0; }
        if (g_dd.open) return 0;
        scroll_by(-d / 120.0f * SF(60));
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; ScreenToClient(hw, &pt);
        layout(); set_hot(hit((float)pt.x, (float)pt.y));
        return 0; }
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        if (g_cap_a != -1) {
            const char* n = vk_name((int)wp);
            if (n) capture_done(n);
            return 0;
        }
        if (wp == VK_ESCAPE && g_dd.open) { close_dropdown(); invalidate(); return 0; }
        if (wp == VK_PRIOR) { scroll_by(-g_view.Height * 0.8f); return 0; }
        if (wp == VK_NEXT)  { scroll_by(g_view.Height * 0.8f); return 0; }
        if (wp == VK_HOME)  { scroll_by(-1e6f); return 0; }
        if (wp == VK_END)   { scroll_by(1e6f); return 0; }
        if (wp == VK_RETURN && !g_dd.open) { play(); return 0; }
        if (msg == WM_SYSKEYDOWN) break;
        return 0; }
    case WM_SYSKEYUP: case WM_SYSCHAR: if (g_cap_a != -1) return 0; break;
    case WM_SYSCOMMAND: if ((wp & 0xFFF0) == SC_KEYMENU && g_cap_a != -1) return 0; break;   // F10 / Alt while capturing
    case WM_KILLFOCUS: if (g_cap_a != -1) { g_cap_a = g_cap_k = -1; invalidate(); } if (g_dd.open) { close_dropdown(); invalidate(); } return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(hw, msg, wp, lp);
}

static void dark_titlebar(HWND hw)
{
    BOOL on = TRUE;
    if (FAILED(DwmSetWindowAttribute(hw, 20, &on, sizeof on)))   // DWMWA_USE_IMMERSIVE_DARK_MODE
        DwmSetWindowAttribute(hw, 19, &on, sizeof on);            // older Windows 10 builds
    COLORREF cap = RGB(17, 18, 20);
    DwmSetWindowAttribute(hw, 35, &cap, sizeof cap);              // DWMWA_CAPTION_COLOR (Windows 11)
}

// "Launcher.exe --play" (RecompLauncher, recomplauncher.json): no window. Rewrites settings.bat from settings.ini (the
// RecompLauncher may have changed it), runs the shader scan, starts run.bat hidden and waits until the game has closed.
// Exit codes: 0 = played, 3 = not built yet (run Setup), 4 = run.bat could not start.
static int play_headless()
{
    load_settings();
    save_settings();
    if (GetFileAttributesA(path_of("build\\Resistance3.exe").c_str()) == INVALID_FILE_ATTRIBUTES) return 3;
    if (!shaderscan_exe().empty()) scan_thread(NULL);
    lx_auto_backup();
    SetEnvironmentVariableA("PORT_CPU_V3", cpu_is_x86_64_v3() ? "1" : "0");   // read by run.bat
    SHELLEXECUTEINFOA se = { sizeof se };
    se.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    se.lpVerb = "open";
    const std::string bat = path_of("run.bat");
    se.lpFile = bat.c_str();
    se.lpDirectory = g_dir;
    se.nShow = SW_HIDE;
    if (!ShellExecuteExA(&se) || !se.hProcess) return 4;
    WaitForSingleObject(se.hProcess, INFINITE);   // run.bat waits for the game, then backs up the shader cache
    CloseHandle(se.hProcess);
    return 0;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR cmd, int show)
{
    // per-monitor DPI awareness, so text is sharp at 125% / 150% scaling
    typedef BOOL (WINAPI *SetCtx)(HANDLE);
    if (SetCtx f = (SetCtx)(void*)GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDpiAwarenessContext"))
        f((HANDLE)-4);   // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    else SetProcessDPIAware();

    GetModuleFileNameA(NULL, g_dir, MAX_PATH);
    if (char* sl = strrchr(g_dir, '\\')) *sl = 0;
    g_cmd = cmd;
    register_options();
    register_pc_options();
    register_input_options();
    register_extra_options();               // launcher_extra.inc
    dist_init();                            // launcher_dist.inc: "updates" option, Wine / Deck choices
    detect_hardware(cmd);
    lx_framegen_filter();                   // launcher_extra.inc: Frame Generation choices for this GPU
    // non-interactive: "Launcher.exe --detect [--sim-gpu "name" --sim-vram GB --sim-cores N --sim-threads N
    // --sim-screen WxH --sim-hz N]" prints what was detected and recommended; "--table" prints the mapping for
    // a set of example PCs. Nothing is saved and no window opens.
    if (cmd && (strstr(cmd, "--detect") || strstr(cmd, "--table"))) {
        FILE* out = stdout;
        if (!GetStdHandle(STD_OUTPUT_HANDLE) || GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_UNKNOWN) {
            if (AttachConsole(ATTACH_PARENT_PROCESS)) freopen_s(&out, "CONOUT$", "w", stdout);
        }
        if (strstr(cmd, "--table")) {
            struct Ex { const char* gpu; double vram; int cores, threads, w, h, hz; };
            static const Ex ex[] = {
                { "Intel(R) UHD Graphics 620", 0.5, 4, 4, 1920, 1080, 60 },
                { "Intel(R) UHD Graphics 770", 0.5, 4, 8, 1920, 1080, 60 },
                { "AMD Radeon(TM) 780M Graphics", 2, 8, 16, 1920, 1200, 120 },
                { "NVIDIA GeForce GTX 1050 Ti", 4, 4, 4, 1920, 1080, 60 },
                { "NVIDIA GeForce GTX 1650", 4, 4, 8, 1920, 1080, 60 },
                { "AMD Radeon RX 580", 8, 6, 12, 1920, 1080, 60 },
                { "NVIDIA GeForce GTX 1070", 8, 6, 12, 2560, 1440, 144 },
                { "NVIDIA GeForce RTX 3050", 8, 6, 12, 1920, 1080, 144 },
                { "NVIDIA GeForce RTX 2060", 6, 6, 12, 1920, 1080, 144 },
                { "NVIDIA GeForce RTX 3060", 12, 6, 12, 2560, 1440, 144 },
                { "NVIDIA GeForce RTX 3070", 8, 8, 16, 2560, 1440, 165 },
                { "AMD Radeon RX 6700 XT", 12, 8, 16, 2560, 1440, 144 },
                { "NVIDIA GeForce RTX 4060", 8, 8, 16, 1920, 1080, 144 },
                { "NVIDIA GeForce RTX 4070", 12, 8, 16, 1920, 1080, 144 },
                { "NVIDIA GeForce RTX 4070 SUPER", 12, 8, 16, 2560, 1440, 165 },
                { "NVIDIA GeForce RTX 4080", 16, 8, 16, 2560, 1440, 240 },
                { "NVIDIA GeForce RTX 3080", 10, 8, 16, 3840, 2160, 60 },
                { "NVIDIA GeForce RTX 4090", 24, 16, 32, 3840, 2160, 144 },
                { "AMD Radeon RX 7900 XTX", 24, 8, 16, 3840, 2160, 144 },
                { "Intel(R) Arc(TM) A770 Graphics", 16, 6, 12, 2560, 1440, 60 },
            };
            for (const Ex& e : ex) {
                r2ac::Hardware h;
                strcpy_s(h.gpu, e.gpu);
                h.vendor = r2ac::has(e.gpu, "NVIDIA") ? 0x10DE : r2ac::has(e.gpu, "AMD") ? 0x1002 : 0x8086;
                h.vram_gb = e.vram; h.cores = e.cores; h.threads = e.threads; h.ram_gb = 16;
                h.screen_w = e.w; h.screen_h = e.h; h.refresh = e.hz;
                r2ac::classify(h);
                r2ac::Settings s = r2ac::recommend(h);
                char a[512], b[512];
                r2ac::describe_hw(h, a, sizeof a); r2ac::describe(s, b, sizeof b);
                fprintf(out, "%s\n    -> %s\n    [fps=%s res=%s fsr=%s upscale=%s aniso=%s filter=%s sharpen=%s cpu=%s shader=%s]\n",
                        a, b, s.fps, s.res, s.fsr, s.upscale, s.aniso, s.filter, s.sharpen, s.cpu, s.shader);
            }
        } else {
            for (auto& s : g_set) s.sel = s.def;
            fprintf(out, "Detected:    %s\nRecommended: %s\n\nsettings.bat:\n", g_hw_desc.c_str(), g_rec_desc.c_str());
            write_env(out);
        }
        fflush(out);
        return 0;
    }
    // test hook: "Launcher.exe --resave" reads settings.ini and writes settings.ini and settings.bat again (no window)
    if (cmd && strstr(cmd, "--resave")) { load_settings(); return save_settings() ? 0 : 1; }
    if (cmd && strstr(cmd, "--play")) return play_headless();
    load_settings();
    ctl_load();

    GdiplusStartupInput gsi; GdiplusStartup(&g_gdip, &gsi, NULL);
    load_art();

    WNDCLASSEXA wc = { sizeof wc };
    wc.lpfnWndProc = wndproc; wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(1)); if (!wc.hIcon) wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.lpszClassName = "PortLauncher3";
    RegisterClassExA(&wc);

    make_scale(NULL);
    // 940 x 640 (logical), shrunk to fit the work area
    RECT wa; SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
    const DWORD style = WS_OVERLAPPEDWINDOW;
    RECT r = { 0, 0, S(940), S(640) };
    AdjustWindowRect(&r, style, FALSE);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    ww = std::min(ww, (int)(wa.right - wa.left)); wh = std::min(wh, (int)(wa.bottom - wa.top));
    g_hw = CreateWindowExA(0, wc.lpszClassName, "Resistance 3", style,
                           wa.left + (wa.right - wa.left - ww) / 2, wa.top + (wa.bottom - wa.top - wh) / 2,
                           ww, wh, NULL, NULL, inst, NULL);
    make_scale(g_hw);
    dark_titlebar(g_hw);
    // test hook: "Launcher.exe --page N" opens on that page (used for screenshots)
    if (cmd && strstr(cmd, "--page ")) { int p = atoi(strstr(cmd, "--page ") + 7); if (p >= 0 && p < P_COUNT) g_page = p; }
    if (cmd && strstr(cmd, "--scroll ")) g_scroll[g_page] = (float)atof(strstr(cmd, "--scroll ") + 9);
    ShowWindow(g_hw, show);
    UpdateWindow(g_hw);
    dist_start(g_hw);                       // launcher_dist.inc: update check, Deck controller
    port_report_start(g_hw, g_dir);         // port_report.cpp

    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageA(&m); }
    delete g_art_scaled; delete g_art;
    GdiplusShutdown(g_gdip);
    return 0;
}
