# Resistance 3 — native PC port/recomp (work in progress)

A static recompilation of the PS3 game *Resistance 3* into a native Windows program. No emulation of the CPU: the game's PowerPC and SPU code is translated to C/C++ and compiled for your PC. Graphics go through RPCS3's Vulkan renderer, with NVIDIA DLSS / AMD FSR upscaling, 60 fps, keyboard and mouse controls, and higher resolutions.

> ⚠️ **Not Perfect.** I have only tested it **on my PC** (Ryzen 5 5600X, GeForce RTX 5070, 32 GB RAM, Windows 11, 1080p screen) and haven't beaten the game. Only the intro and the opening Haven level have been played; past that it's untested, so things could go wrong. Some crashes or freezes may happen.

> **This repository contains no game files and no game code.** You need your own copy of the game. The setup builds everything game-specific on your PC from that copy.

## What you need

- A PC running Windows 10 or 11 (64-bit).
- A CPU with AVX2: Intel 4th gen or newer, or any AMD Ryzen. Older CPUs without AVX2 won't run it.
- A Vulkan 1.3 GPU. DLSS needs a GeForce RTX card; everything else (including FSR) works on AMD, NVIDIA and Intel. Only tested on NVIDIA so far.
- Your own dump of **Resistance 3, US disc (BCUS98176), version 1.00**. This is the folder that contains `PS3_GAME`, the same kind of folder RPCS3 uses. Other regions and patched versions are not supported yet.
- About 50 GB of free disk space: ~36 GB for the game files and the rest for the build tools and the build.
- An internet connection for the setup.

## Install

1. Download this repository: click on Code at the top, then Download ZIP, then unzip it somewhere with no spaces in the path, for example `C:\Games\Resistance3Port`.
2. Double-click **`Setup.bat`**. It will:
   - install the free build tools if they're missing (Visual Studio 2022 Build Tools, LLVM, CMake, Ninja, Python, Git), using `winget`;
   - ask you to pick your game folder, then copy it into the port folder (or move it, if it's on the same drive);
   - decrypt the game's program file, using the application keys from RPCS3's public source code;
   - download RPCS3's source at a fixed version and apply this port's renderer patch (`ps3recomp/patches/r3-rpcs3.patch`);
   - generate the game code from your copy and build the game.

   The first run takes a while: tool installs plus a 20–60 minute build. At the end, Setup offers to put a "Resistance 3" shortcut to the launcher on your desktop.
3. Play with **`Launcher.exe`**. It detects your PC and recommends settings (**Auto-detect**), and has all the settings (display mode, resolution, frame rate, VSync, anti-aliasing, upscaling, texture filtering, sharpening, PC options) and the key bindings. See [Launcher](#launcher).

You only need to run Setup again after downloading a newer version of the port; otherwise just use the launcher.

**First launch:** the game shows its own one-time setup screens and an autosave notice. The first time you load into a level, expect some stutter while the graphics shaders are compiled; after that, shaders are cached.

## Controls

Keyboard and mouse are supported out of the box: WASD move, Space jump, C/Ctrl crouch, Shift sprint, direct mouse look, Mouse1 fire, Mouse2 aim, Mouse3/F secondary fire, R reload, V/Mouse4 melee, G/Mouse5 grenade, 1/2 grenade type, Q / wheel weapon select, E use, Esc pause, Tab select, F8 releases the mouse. Controllers work too. All keyboard and mouse bindings can be changed in the launcher (Controls page, two keys per action; saved in `controls.txt`).

**Button prompts follow the device you used last.** Touch the keyboard or mouse and every on-screen prompt shows your keys; press a button or move a stick on a controller and they switch to that controller's buttons. Switching is instant and works anywhere, mid-game or in a menu. With an Xbox / XInput controller (or any non-PlayStation pad) the prompts show Xbox buttons, drawn by the port as simple coloured letters (no copied artwork):

| PS3 | Xbox | PS3 | Xbox |
|---|---|---|---|
| Cross | A | L1 / R1 | LB / RB |
| Circle | B | L2 / R2 | LT / RT |
| Square | X | L3 / R3 | LS / RS (stick click) |
| Triangle | Y | Select / Start | View / Menu |
| D-pad | D-pad | Left / right stick | Left / right stick |

If a Sony DualShock 3/4 or DualSense is plugged in (detected by its USB/Bluetooth ID; it is used through DS4Windows / Steam Input as XInput) the PlayStation symbols are kept. The launcher's **Controller button prompts** option forces Xbox or PlayStation.

Where the prompts are replaced: Resistance 3's menus and HUD are Scaleform (Flash) movies, and every button symbol in them, on every screen, is one of the game's 18 `img://iconN` button pictures. The port swaps those pictures for the device in use, which covers the title / "Press Start" screen, the main menu, the lobby and options, the pause menu, loading-screen tips, the HUD and tutorial hints. Text that names a button inside a sentence (tutorials, hints) uses a symbol character; on keyboard that is rewritten to the key's name. PlayStation Move pictures are left alone. Prompts name the game's default button layout; if you remap buttons in the game's own options, they still name the default ones. `R2_KEY_PROMPTS=0` turns all of this off (PS3 symbols always).

**Mouse look is direct:** raw mouse counts turn the camera's yaw and pitch each frame (× the launcher's sensitivity, with its invert option), not an emulated right stick: no acceleration, dead zone or speed cap. While aiming down sights the turn rate is scaled by the scope zoom (`R2_ZOOM_SENS`: 1 = default, 0 = no scaling, up to 2). Mouse movement made while the camera isn't yours (menus, pause, cutscenes) is dropped, so the view doesn't jump afterwards. The launcher's **Mouse aiming = Classic** switches back to stick emulation. The game has no mouse pointer in its menus; use the keyboard (arrows / Enter / Esc) or a controller. F8 frees the cursor.

**Aiming and sticks** (launcher, Controls page):

| Option | Values | Variable |
|---|---|---|
| Vertical mouse sensitivity | **Same as horizontal**, 50 %, 75 %, 125 %, 150 %, 200 % | `PAD_MOUSE_SENS_Y=<0.1..4>` (multiplies the up/down mouse speed, direct and classic mouse aiming) |
| Aiming sensitivity | **Game default**, 50 %, 75 %, 125 %, 150 % | `R2_ADS_SENS=<0.1..3>`: extra mouse multiplier while aiming down the sights or through a scope (on top of the zoom scaling) |
| Aim mode | **Hold**, Toggle | `PAD_AIM_TOGGLE=1`: the Aim key / left bumper (L1) toggles aiming; menus release it. Keyboard, mouse and controller |
| Stick deadzone | **Default** (XInput standard, per axis), Small 8 %, Medium 15 %, Large 25 %, Very large 35 % | `PAD_DEADZONE=<0..0.9>` (radial) |
| Stick response curve | **Default**, Linear, Precise (1.6), Very precise (2.2), Fast (0.7) | `PAD_CURVE=<0.5..3>`: stick travel past the deadzone raised to this power |

**Controllers can be plugged in and out at any time** (XInput, up to four); no restart. An empty controller slot is re-checked about once a second.

## PC features

- **PC menu:** press **F10**, or hold **View + Menu** (Select + Start) on a controller for 1.5 s, for a small overlay menu: **Resume**, **PC Options** and **Quit to Desktop**. The game is paused while it is open. *PC Options* changes, live: frame rate, upscaler (DLSS / FSR 3.1 / off; applies at the next start), mouse sensitivity, field of view and controller prompts (Auto / Keyboard keys / Xbox / PlayStation). Each change is saved where the launcher keeps it (`settings.ini` + `settings.bat`, `controls.txt`). Arrow keys / D-pad move, Enter / A select, Esc / B back. The game's own pause and main menus are Scaleform (Flash) movies the port doesn't edit, so a small hint, *F10: PC menu*, appears while the pause menu is up and once at start. `R2_MENU_PC=0` turns the menu off (F10 / View+Menu then go straight to the quit box), and `R2_MENU_PC_HINT=0` hides the hint (it is off by default in exclusive fullscreen).
- **PC wording:** the game's English text is adjusted where it talks about the console: "Press and hold the PS button to quit the application" → "Press F10 to quit the game", "Insufficient Hard Disk Drive space" → "Insufficient disk space", "Do not switch off the power when the HDD access indicator is flashing" → "Do not close the game while it is saving", and "wireless controller" → "controller". Only the text in memory is changed (the game files aren't touched). `R2_PC_TEXT=0` turns it off.
- **Quit to desktop:** *Quit to Desktop* in the PC menu (or F10 / View + Menu with `R2_MENU_PC=0`). A small box asks *Quit to desktop?*: Enter / Space / A = yes, Esc / Backspace / B = no. While it is shown the game is paused and gets no input. On yes (and also when the window is closed with Alt+F4 or the X button) the port waits for a save in progress to finish (up to 15 s, "Saving..."), flushes open files and exits. The game's own menus have no Quit entry; adding one would mean patching the game's Flash menus without being able to test them, so the PC menu carries it instead.
- **Alt+Enter** switches between borderless fullscreen and windowed. **Alt+Tab** is safe in borderless and windowed mode.
- **Monitor** (launcher, Display › PC): which screen the window opens on (`RSX_MONITOR=<n>`, 1-based in Windows' order; empty = primary).
- **When the game is in the background** (launcher): *Pause and mute* (default) freezes the game and silences it when the window loses focus; *Mute only*; *Keep running* (`R2_BACKGROUND=pause|mute|run`). On focus loss the cursor is always released and shown; it is also shown while the quit box is up or the game is paused this way, and captured/hidden again when you click back in.
- **Skip intro movies** (launcher, default on): the studio logo video and the opening movie are skipped, so the game starts at the title screen (`SKIP_INTRO=1`, set by `run.bat`; `SKIP_INTRO=0` to watch them).
- **Frame rate in menus** (launcher): caps the frame rate at 60 or 30 fps whenever you are not controlling your character (menus, loading screens, cutscenes); gameplay keeps the normal frame rate (`PORT_MENU_FPS=<fps>`, minimum 30; empty/0 = off, the default).
- **Screenshots:** **F12** or **PrtScn** saves `screenshots\R3_<date>_<time>.png` in the game folder (a short "Screenshot saved" note appears). The launcher's About page has **Open screenshots**.
- **Saves** are in the game folder: `gamedata\dev_hdd0\home\00000001\savedata\BCUS98176_SAVE_0` (game settings are saved there too, as on the PS3). Back them up by copying that folder. **Open save folder** (launcher, About) opens it. `PS3_GAMEDATA_ROOT=<dir>` moves the whole `gamedata` tree elsewhere (saves then go to `<dir>\dev_hdd0\home\00000001\savedata`). The port's own settings are `settings.ini` / `settings.bat` and `controls.txt` in the game folder.
- **Reset all to defaults** (launcher, About) resets display, graphics, PC options, keys and mouse (saves untouched). **Auto-detect** (Display/Graphics) resets only display and graphics; **Restore default controls** only the controls.
- **Crash and hang reports:** if the game crashes, a minidump is written to `logs\crash\R3_<date>_<time>_crash.dmp` and a one-line summary is added to `logs\crash\crashes.txt`. If no game frame is drawn for 90 s while the game isn't paused, an `R3_<date>_<time>_hang.dmp` with every thread's state is written once per stall and the game keeps running. Send these with the session's log when reporting a problem. `R2_HANG_S=<seconds>` (≥ 10) changes the hang limit, `R2_CRASH_DUMPS=0` turns reports off, `R2_CRASH_DUMPS=full` writes full-memory dumps (large).
- **Renderer fatal errors:** if the RSX renderer thread hits a fatal error, the reason is printed to the console/log as `[rpcs3] thread '...' terminated due to fatal error: ...` (and to `logs\rpcs3_rsx.log`). The previous run's `rpcs3_rsx.log` is kept as `logs\rpcs3_rsx.prev.log`, so it survives one restart. A dead renderer thread looks like a hang (the game stops drawing but keeps running), so include these lines when reporting one.
- **Performance overlay:** **F3** shows or hides frame rate, frame time (average and worst) and, with the Compatible renderer, draws per frame and the RSX thread's time per draw, in a corner of the game window. Launcher *Performance overlay*: Off (F3 shows it, the default), On at start, Disabled (F3 does nothing) = `R2_OVERLAY` unset / `1` / `0`; `R2_OVERLAY_POS=tl|tr|bl|br` picks the corner (top left by default). Not visible in exclusive fullscreen. While it is shown the renderer's counters are on (as with `R2_RSX_PROF=1`, without the log output).
- **Trophies as local achievements:** when the game awards a trophy the port shows a small notification in the corner of the window and records it in `%USERPROFILE%\Saved Games\Resistance 3\trophies.json` (name, grade, date). The trophy list (59 trophies, comm id `NPWR01841`) is read at run time from your own game files (`gamefiles\PS3_GAME\TROPDIR\NPWR01841_00\TROPHY.TRP`); nothing of it ships with the port. The platinum trophy is awarded when every trophy it counts is unlocked. The runtime's own record stays in `gamedata\trophies\NPWR01841.json`. The launcher has a **Trophies** page listing them (unlocked first; hidden ones stay hidden until earned). `R2_TROPHY_TOAST=0` (launcher *Trophy notifications: Off*) records them without the notification.
- **Mods folder:** launcher *Mods folder: On* (`R2_MODS=1`) loads replacement game files from the `mods` folder next to the game; `R2_MODS=<folder>` uses another folder. Each sub-folder is one mod laid out like the game's `PS3_GAME` folder: `mods\<mod>\USRDIR\...` replaces the same file under `gamefiles\PS3_GAME\USRDIR`. Names are matched without regard to case; when two mods replace the same file, the one whose folder name sorts last wins; folders starting with `_` are skipped (a quick way to disable one). Every replaced file is listed in `logs\run.log` as `[mods]` lines.
- **Ultrawide:** not available yet. Resistance 2's port widens the 3D view on 21:9 / 32:9 screens by hooking the game's camera-frustum setters; the matching functions in Resistance 3 could not be identified with certainty, so wider screens show the 16:9 picture with side bars.
- These window features (F10, F3, F12, pause on focus loss, monitor choice) work with both renderers. Exclusive fullscreen may hide the quit box and give black screenshots; use borderless.

## Launcher

`Launcher.exe` has its own icon and artwork (original, drawn by `tools\art\make_r3_art.py`; nothing from the game). It writes your choices to `settings.ini` (its own) and `settings.bat` (`set "NAME=value"` lines that `run.bat` loads before starting the game), and the key bindings to `controls.txt`.

**Recommended for this PC / Auto-detect.** On first start the launcher detects your GPU (name, video memory, RTX or not, integrated or not), CPU cores/threads, RAM and screen (resolution and refresh rate), and shows a *Recommended* panel with a Low / Medium / High / Ultra tier and the settings it would use. **Auto-detect** applies them (display and graphics only). The same table is used by the game when `R2_PRESET=auto|low|medium|high|ultra` is set: it fills in every setting you did not set yourself. GPUs with DLSS get DLSS, others FSR 3.1 (or FSR 1 scaling on weak integrated GPUs). From a command prompt, `Launcher.exe --detect` prints what it found and the `settings.bat` it would write, and `Launcher.exe --table` prints the recommendation for a list of typical PCs (`--sim-gpu "<name>" --sim-vram <GB> --sim-cores <n> --sim-threads <n> --sim-ram <GB> --sim-screen <w>x<h>@<hz>` simulate other hardware).

**1080p rendered, DLSS output at 4K (supersampled on a 1080p screen):** Render resolution **3840 x 2160**, Anti-aliasing **NVIDIA DLSS**, Upscaling **Performance (50%)**, i.e. `RSX_RES_SCALE=3`, `RSX_FSR=2`, `RSX_UPSCALE=0.5`. The 3D scene is drawn at 1920x1080 and DLSS reconstructs 3840x2160, which is then scaled to your screen. This is the launcher's default. If DLSS isn't available (not an RTX card, or the DLSS runtime is missing) the port uses AMD FSR 3.1 instead, and if FSR 3.1 can't start either, FSR 1 spatial upscaling.

### All launcher options

| Page | Option | Values (default first or marked) | Environment variable |
|---|---|---|---|
| Display | Display mode | Borderless, Exclusive fullscreen, Windowed | `RSX_DISPLAY` |
| Display | Window size | 1280x720 … 3840x2160 (**1920x1080**) | `RSX_WINDOW` |
| Display | Frame rate | 30, **60**, 120, 144, Match display, Just under refresh (G-Sync / FreeSync), Uncapped | `R2_FPS` (`30`, `<fps>`, `refresh`, `vrr`, `0`) |
| Display | VSync | **Off**, On, Adaptive, Auto (On when the frame rate matches the display) | `RSX_VSYNC` |
| Display | CPU profile | **Auto**, Low (4 cores), Mid (6 cores), High (8+) | `PORT_CPU_PROFILE` |
| Display › PC | Monitor | **Primary**, Monitor 1…n | `RSX_MONITOR` |
| Display › PC | When the game is in the background | **Pause and mute**, Mute only, Keep running | `R2_BACKGROUND` |
| Display › PC | Skip intro movies | **On**, Off | `SKIP_INTRO` |
| Display › PC | Frame rate in menus | **Same as in game**, 60 fps, 30 fps | `PORT_MENU_FPS` |
| Display › PC | Controller button prompts | **Auto**, Xbox, PlayStation, Keyboard keys | `R2_PAD_GLYPHS` (`xbox` / `ps` / `keys`) |
| Display › PC | Presentation mode (Compatible renderer) | **Default (follow VSync)**, Immediate (VRR / tearing), Mailbox, FIFO | `R2_PRESENT` (`immediate` / `mailbox` / `fifo`) |
| Display › PC | Performance overlay | **Off (F3 shows it)**, On at start, Disabled | `R2_OVERLAY` (unset / `1` / `0`) |
| Display › PC | Trophy notifications | **On**, Off | `R2_TROPHY_TOAST` (`0` = off) |
| Display › PC | Mods folder | **Off**, On | `R2_MODS` (`1` / `<folder>`) |
| Graphics | Render resolution | 720p (PS3) … **3840x2160** | `RSX_RES_SCALE` (1, 1.5, 2, 2.5, **3**) |
| Graphics | Anti-aliasing | Off, AMD FSR 3.1, **NVIDIA DLSS** | `RSX_FSR` (0 / 1 / **2**) |
| Graphics | Upscaling | Native (DLAA), Quality 67%, Balanced 58%, **Performance 50%** | `RSX_UPSCALE` (1, 0.667, 0.58, **0.5**) |
| Graphics | Texture filtering | **Trilinear**, Anisotropic 2x/4x/8x/16x | `RSX_ANISO` |
| Graphics | Scaling (to the screen) | **Smooth**, Sharp pixels, AMD FSR 1 | `RSX_FILTER` (`linear` / `point` / `fsr`) |
| Graphics | Sharpening | Off, **Low**, Medium, High | `RSX_SHARPEN` (0 / 0.25 / 0.5 / 0.8) |
| Graphics | Renderer | **Compatible (RPCS3)**, Native Vulkan (experimental) | `R2_RSX` (`rpcs3` / `vulkan`) |
| Graphics | Shader compilation | **Asynchronous**, Asynchronous + interpreter, Synchronous | `R2_SHADER_MODE` (`async` / `interp` / `sync`) |
| Graphics › Graphics quality | Field of view (experimental) | **Original**, Wider (0.85), Widest (0.7) | `R2_PATCH_FOV_VALUE` |
| Controls | Keys (two per action), Mouse sensitivity, Invert vertical look, Mouse aiming (Direct / Classic), Free the mouse cursor key (F8) | | `controls.txt` |
| Controls › Aiming and sticks | Vertical mouse sensitivity, Aiming sensitivity, Aim mode, Stick deadzone, Stick response curve | see *Controls* above | `PAD_MOUSE_SENS_Y`, `R2_ADS_SENS`, `PAD_AIM_TOGGLE`, `PAD_DEADZONE`, `PAD_CURVE` |
| Trophies | The game's trophy list and what you have unlocked | | |
| About | Open save folder, Open screenshots, Open logs folder, Reset all to defaults | | |

With `R2_PRESET`, any setting you set yourself (in `settings.bat` or the environment) wins over the preset.

## Graphics options

- **Renderer:** the default is the *Compatible* renderer: the game's RSX (PS3 GPU) command stream is fed to RPCS3's RSX core and Vulkan renderer, built into the game as a library (`R2_RSX=rpcs3`). It uses RPCS3's settings from `rpcs3_config.yml` (next to the game, or `R2_RSX_CONFIG=<file>`), with the port's settings on top. *Native Vulkan* (`R2_RSX=vulkan`) is the port's own experimental renderer.
- **Frame rate:** 60 fps by default (the game was 30 fps on PS3). The game's own frame limiter is replaced by a precise PC-side one (high-resolution timer plus a short spin), the same idea as RPCS3's "Unlock FPS" patch, and RPCS3's vblank follows the chosen rate. The game clock's 1/75 s minimum step is lowered too, so 120/144 fps and Uncapped run at the right speed; cutscenes that use a fixed step per frame are kept at real-time speed. 30 fps uses the game's original limiter. *Just under refresh* (`R2_FPS=vrr`) caps a little below the display's refresh rate (e.g. 138 fps at 144 Hz) so a G-Sync / FreeSync display stays in its variable-refresh range. `R2_PACE=0` uses the older, less precise wait; `R2_PACE_SPIN_US=<µs>` (default 500) sets how long the limiter spins before a frame.
- **Display mode:** borderless fullscreen, exclusive fullscreen or windowed, with VSync off / on / adaptive / auto.
- **Anti-aliasing → NVIDIA DLSS** uses DLSS on GeForce RTX cards and falls back to FSR 3.1, then FSR 1, on other GPUs. The DLSS SDK is downloaded by Setup into `tools\dlss` (if that fails, the port still builds without it). `RSX_DLSS_PRESET=<letter>` forces a DLSS model preset.
- **Upscaling** (with FSR or DLSS on): Native (DLAA), Quality, Balanced or Performance. Menus and HUD stay at full resolution.
- Resistance 3 renders each frame across the console's buffer flip (a geometry pre-pass, then lighting and a material pass). The upscaler follows that, so DLSS / FSR run on the finished image, not the G-buffer. The upscaler locks onto the scene size once it is stable, so it isn't reset by menus or small passes, and briefly backs off instead of resetting when it can't find the scene for a frame or two.
- **CPU profile** (Display page): Auto picks from the cores the game can use (logged as `[cpu]`). Low (≤4 cores) turns off the timing spin-waits, folds the Vulkan submit thread into the render thread and uses fewer shader-compiler threads. Mid (6 cores) shortens the spin-waits. High changes nothing. The port also spreads its main, RSX and FIFO threads over separate cores, keeps them on P-cores on hybrid Intel CPUs and lowers background threads. Advanced: `PORT_CPU_PROFILE=auto|low|mid|high|off` (settings you set yourself always win); `PORT_THREAD_PLACEMENT=0` turns off thread placement; `PS3_PRIO_BOOST=0/1` forces priority boosts off or on; `PORT_MAIN_CPU=<n>` pins the main thread to CPU n.
- **Low-end PCs:** use **Auto-detect**. On CPUs with fewer than 8 threads the port stops raising thread priorities and pinning threads to cores, and its busy-wait loops give the CPU to other threads. If a 4-core PC still stutters, use 30 fps.

### Renderer switches (advanced, `settings.bat`)

All optional; unset = the default shown. Each is logged at start-up in the `[r2rsx] perf:` line.

| Variable | Effect |
|---|---|
| `R2_RSX_PROF=1` | Every 5 s, prints a block of `[r2rsx-prof]` lines to the log: CPU time of each RSX thread, the renderer's frame phases, draw and framebuffer-bind time, command-queue flushes (by caller), ZCULL report waits, memory faults that needed a GPU readback, and vertex uploads. It also prints the RSX thread's busy time without DLSS, the time spent outside draws (flushes, flips, ZCULL, blits, labels), deferred ZCULL labels, and program-cache hits (`MRU hits`, `pipeline memo hits`, `re-sent FP kept`, `re-sent VP kept`; `program slow-path loads` counts the full cache lookups that remain). Further lines split the framebuffer-bind locks (discard / orphan / colour / depth, worst and slow rebinds), the texture cache (mutex wait, invalidations, relocks) and Windows `VirtualProtect` calls, the flip (frame-limiter sleep vs present), the program phase (analysis, lookup, constants, and the rest of the program environment: DLSS/FSR draw hook, vertex/fragment/texture env, `bind_uniform`) and the texture cache's blocking GPU readbacks. For finding what limits the frame rate. |
| `R2_SHADER_THREADS=<n>` | Background shader-compiler threads (default: what the CPU profile leaves free). |
| `R2_SHADER_CACHE=0` | Don't keep compiled shaders on disk (`shadercache\rpcs3`). |
| `R2_MTRSX=0/1` | RPCS3's multithreaded RSX (guest→host copies on a helper thread). Default: on. |
| `R2_ASYNC_TEX=0/1` | Asynchronous texture streaming. |
| `R2_HOST_LABELS=0/1` | RPCS3's host label synchronisation. |
| `R2_WCB=0/1` | Write colour buffers back to memory (needed for some effects; default: the config file's value). |
| `R2_FBB_KEEP_WCB=1` | Restores RPCS3's original behaviour of writing a render target back when it is re-bound as the same surface (the port skips that redundant copy). |
| `R2_ZCULL=sync` / `relaxed` / `off` | Occlusion queries. *relaxed* (the default, and RPCS3's recommended setting for Resistance 3) reads only finished results and never makes the renderer wait for the GPU, so visibility can lag one frame. *sync* is strict: every report waits for the GPU (the first bench spent ~8.5 ms per frame on that). *off* reports everything visible. In strict mode, report reads are batched, the wait sleeps on the GPU fence instead of polling, and pending memory-fault flushes are serviced while waiting. |
| `R2_ZCULL_DEFER=0` | In strict mode, label writes that would wait for pending ZCULL reports are deferred and written once the reports finish (default on; `0` restores the immediate wait). |
| `R2_ZCULL_FLIP_LAG=<n>` | **Experimental, may hang (default 0 = off).** In strict ZCULL mode, deferred labels still waiting on the GPU at a flip are carried over up to `n` flips instead of being waited for at every flip; they are also released whenever the RSX command queue runs dry. With `1` the game hung ~30 s into Haven in one bench, so leave it off. |
| `R2_MM_SKIPSAME=0` | The renderer asks Windows for the current protection first and skips `VirtualProtect` when the pages already have it (default on); `0` always calls it. |
| `R2_MM_INCR=0` | The RSX helper thread applies queued memory-protection changes one block at a time and lets the RSX thread in between (default on); `0` applies the whole queue in one lock hold. |
| `R2_FBO_COMMIT_FLUSH=1` | When a new depth or colour target is bound over memory that holds an older target fully inside it, the older one is dropped instead of being read back from the GPU (the new target overwrites those bytes; partly covered ones are still read back). This removed ~5 ms per frame of GPU stalls in Resistance 2. `1` restores RPCS3's readback. |
| `R2_FBB_KEEP=1` | Test switch, off by default: keeps a depth or colour target bound when the next draw doesn't use it but it is exactly the bound one, saving a framebuffer rebind. A pass that samples that target then reads a copy, which may look different. |
| `R2_PROG_HINT=0` | The shader-program lookup cache is normally dropped only when the shader code or its inputs change; `0` drops it on every state change, as RPCS3 does. |
| `R2_FP_REBIND=0` | The game re-sends the same fragment program many times per frame (about 200 times per frame in Haven), which made the renderer treat it as new shader code and look it up again. The port compares the re-sent program with the one in use (skipping the embedded constants) and only refreshes the constants; `0` restores RPCS3's behaviour. |
| `R2_PIPE_MEMO=0` | When only render state changed and the result is the pipeline used by the previous draw, it is reused without a cache lookup; `0` turns this off. |
| `R2_VP_REBIND=0` | The same for vertex programs: a vertex program uploaded or selected again with the same code is kept without being analysed and looked up again; `0` restores RPCS3's behaviour. |
| `R2_PROG_MRU=0` | Turns off the small most-recently-used table that lets the renderer find an already-compiled shader program without hashing its code. |
| `R2_PRESENT=immediate` / `mailbox` / `fifo` | Vulkan present mode (overrides `RSX_VSYNC`; launcher *Presentation mode*). `immediate` (or `vrr` / `tearing`) never waits: best with G-Sync / FreeSync and `R2_FPS=vrr`. |
| `R2_ZCULL_ACCURATE=0/1` | Precise Z-pass counts. |
| `R2_MSAA=auto` | MSAA in the game's own render targets (only with FSR/DLSS off). |
| `R2_HIGHP_Z=1` | Force a high-precision depth buffer. |
| `R2_LOD_BIAS=<x>` | Texture LOD bias (negative = sharper). |
| `R2_GPU_TEX_SCALING=1` | Use the GPU for texture scaling. |
| `RSX_VBLANK_US=<µs>` | Override the vblank period (normally derived from the frame rate). |

## RPCS3 patches

The community RPCS3 patches for Resistance 3 (RPCS3's `patch.yml`) were checked against this game's program file: disc **BCUS98176 / BCES01118 1.00**, PPU hashes `502feb2351d2b637a2704d5a50cb44bc1b781532` (US) and `2701cf372814d59f26e771dc9394323d4f799682` (EU), which share every address. The PowerPC code is recompiled, so patches to instructions become hooks (`tools/hooks/r3.json`), and data patches are written into game memory at boot (`project/r3_patches.cpp`), only if the original value is there; another version is never patched at the wrong place (a `[patch] ... skipped` line is logged instead).

| Patch (author) | What it does | Guest address | How it's ported | Setting | Default |
|---|---|---|---|---|---|
| Unlock FPS (Whatcookie, boma): limiter wait | Removes the frame limiter's wait | `0x00736C44` | Hook in `func_00736BAC` (`r2_frame_period`, `project/stubs.cpp`): frames are paced on the PC side | `R2_FPS` | 60 |
| Unlock FPS: frame-time floor | 0.001 s minimum frame time → 0.0001 | `0x00F02AD4` (float) | Data patch at boot (and again by the limiter) | Automatic when `R2_FPS` ≠ 30; `R2_PATCH_FPS_FLOORS=1` also at 30 | follows `R2_FPS` |
| Unlock FPS: game-clock clamp | The game clock's 1/75 s lower clamp | `0x007EEAC8` | Hooks in `func_007EEA44` (`r3_dt_floor` / `r3_dt_ceil`): the floor follows the frame rate, and fixed-step cinematics keep real-time speed | follows `R2_FPS` | on above 30 fps |
| Extended FOV (illusion) | Wider field of view (the constant in the projection; lower = wider). On Resistance 2 the same kind of patch misaligns shadows slightly, so it is experimental | `0x00F0608C` (float 1.0 → 0.7) | Data patch at boot | Launcher *Field of view*, or `R2_PATCH_FOV=1` with `R2_PATCH_FOV_VALUE=0.3..1.0`; `R2_PATCH_FOV=0` forces it off | off |

Not ported: patches for other versions (1.05 and later, PSN) and cheat patches. No community patch for this game exists for resolution (the port's render-resolution setting does that), motion blur, depth of field or film grain.

## Performance work

Performance fixes have been applied as far as possible without changing how the game looks:

- **SPU (the PS3's job processors):** faster lock-line reservation and acknowledgement, cheaper waits on SPU channels and direct block-to-block jumps in the translated SPU code (`SPU_FAST_LLAR`, `SPU_FAST_ACK`, `SPU_FAST_WAIT`, `SPU_FAST_HOP`: all on by default, `=0` turns one off for testing; `SPU_FAST_LLPOLL=1` is an opt-in back-off for lock-line polling).
- **Spin-waits:** the game's main-thread loops that spin while waiting for SPU jobs execute a CPU pause hint each lap instead of hammering the core (`R2_SPIN_PAUSE=0` turns it off), and give the core away on CPUs with few threads.
- **Memory faults:** the handler that commits guest memory on first touch looks up a bitmap instead of asking Windows, which makes the common case much cheaper.
- **Measured (in-game bench, Haven, RTX 5070, 60 fps target, DLSS 1080p → 4K):** 44 fps at first, limited by the RSX thread (ZCULL report waits cost 8.5 ms per frame); with relaxed ZCULL and deferred labels 55 fps, ZCULL down to 0.24 ms per frame. The remaining costs were framebuffer-bind locking, shader-program lookups and FIFO command processing; the program lookups were cut since (re-sent programs kept, previous pipeline reused), and Resistance 2's renderer rounds 4 and 5 (framebuffer-bind memory protection, texture-cache readbacks) are included. Latest bench: **59.5 fps** in Haven (1,145 draws per frame) at about 10.5 µs of RSX-thread time per draw (from 19.7 at first), stable over a 12-minute session; the DLSS per-draw hook went from 2.9 to 0.2 µs. Lower per-draw cost helps low-end CPUs the most.
- **Network thread:** the game's HTTP thread polled `cellHttpClientPollConnections`, which wasn't implemented and returned at once, so it used a whole CPU core; it now waits for a connection like the real function, and with no connection returns the library's "no connection" error so the thread sleeps between polls.
- **Renderer:** thread counts sized to the CPU, idle back-off in the RSX threads, frame pacing mirrored into RPCS3's vblank, skipped redundant render-target write-backs, relaxed ZCULL by default (no GPU stalls for occlusion results; strict mode batches reads, sleeps on the GPU fence and defers labels), shader-program lookups that skip re-hashing (`R2_PROG_HINT`, `R2_PROG_MRU`), re-sent fragment and vertex programs kept instead of analysed and looked up again (`R2_FP_REBIND`, `R2_VP_REBIND`) and the previous draw's pipeline reused when only state changed (`R2_PIPE_MEMO`), fewer and cheaper `VirtualProtect` calls (`R2_MM_SKIPSAME`, `R2_MM_INCR`), no blocking GPU readback of render targets that a new target fully overwrites (`R2_FBO_COMMIT_FLUSH`), deferred ZCULL labels carried over a flip in strict mode (`R2_ZCULL_FLIP_LAG`), cheaper DLSS camera-matrix matching, and fewer DLSS resets (see the table above).

## Status

This is a work in progress, tested on a single PC.

- Runs at about 52–60 fps in Haven on my PC (CPU-bound; the GPU is at ~40%).
- Performance is mostly limited by the game's SPU job system, so a decent CPU helps most.
- There are occasional freezes and other bugs.

If you hit a problem, let me know how to reproduce it, and if you can, send the log from the `logs\` folder for that session (the launcher has a button for it). That helps a ton.

## How it works

- `ps3recomp/` is a modified copy of [ps3recomp](https://github.com/sp00nznet/ps3recomp) by sp00nznet (MIT licence). It holds the runtime, the PS3 system libraries (audio, input, file system, SPU scheduling) and the lifters that translate PowerPC and SPU code into C/C++.
- Graphics: the game's RSX command stream is fed to RPCS3's RSX core and Vulkan renderer, built as a library. This repository only contains the patch to RPCS3 (`ps3recomp/patches/r3-rpcs3.patch`); Setup downloads the original source and applies it. The patch adds DLSS / FSR upscaling, display modes and fixes for running outside the emulator.
- `project/` is the game-specific part of the port: its hooks, the frame limiter, mouse look, button prompts, the RPCS3 patches (`r3_patches.cpp`), the PC features (`r3_pcport.cpp`: quit box, screenshots, focus pause), crash reports (`r3_crash.inc`), the launcher and the DLSS bridge.
- `tools/art/make_r3_art.py` draws the launcher's and the game's icons and the launcher artwork (Python + Pillow).
- `tools/r3_regen.py` runs the lifters on your decrypted `EBOOT.elf`, then re-applies this port's own edits to the generated code (`tools/hooks/`).

## Known Bugs

- 60 fps and above change the game's timing; if something (physics, a scripted sequence) looks wrong, try 30 fps in the launcher. 120 fps and above are less tested than 60.
- Exclusive fullscreen may hide the quit box and give black screenshots; use borderless.
- Occasional freezes or crashes are possible, especially past the opening level.

## Credits and licences

- The Resistance 2 PC port, the base of this one (same runtime, launcher, DLSS bridge and fixes).
- [Killzone PC ports](https://github.com/drakolordx7/KillzoneRecomp) by drakolordx7: ideas for the launcher and quality-of-life features, with the maintainer's permission.
- [Resistance: Fall of Man PC port](https://github.com/Psyykosis/Resistance-Fall-of-Man-Recomp): this port builds on its runtime, tools and research (SPURS/SPU job handling, frame pacing, input, audio and video fixes).
- ps3recomp, by Ned Heller / sp00nznet: MIT.
- RPCS3 (RSX core and Vulkan renderer), downloaded during setup and patched: GPL-2.0. Because the built game links RPCS3 code, **the built program is distributed under the GPL-2.0**; the patch in this repository is GPL-2.0 too.
- AMD FidelityFX SDK (FSR 3.1): MIT, © Advanced Micro Devices.
- NVIDIA DLSS SDK v310.9.16, downloaded during setup into `tools/dlss/`: NVIDIA RTX SDK licence (`tools/dlss/LICENSE.txt`), not part of this repository.
- FFmpeg 7.1.1 (gyan.dev build), downloaded during setup for the game's videos: GPL/LGPL, not part of this repository.
- RPCS3's `key_vault.cpp`, downloaded during setup and used only to decrypt your own copy.
- The port's own code: MIT, see `LICENSE`.

*Resistance 3* is © Sony Interactive Entertainment, developed by Insomniac Games. This is an unofficial fan project, not affiliated with or endorsed by Sony or Insomniac. Please own the game you play.
