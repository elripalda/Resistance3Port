# Resistance 3 — native PC port/recomp (work in progress)

A static recompilation of the PS3 game *Resistance 3* into a native Windows program. No emulation of the CPU: the game's PowerPC and SPU code is translated to C/C++ and compiled for your PC. Graphics go through RPCS3's Vulkan renderer, with NVIDIA DLSS / AMD FSR upscaling, 60 fps, keyboard and mouse controls, and higher resolutions.

> ⚠️ **Not Perfect.** I have only tested it **on my PC** (Ryzen 5 5600X, GeForce RTX 5070, 32 GB RAM, Windows 11, 1080p screen) and haven't beaten the game. Only the intro and the opening Haven level have been played; past that it's untested, so things could go wrong. Some crashes or freezes may happen.

> **No game files and no translated game code are distributed, in this repository or in the installer.** You need your own copy of the game. Setup decrypts its program on your PC, translates it into C/C++ and builds the game there; the generated code and the built game never leave your PC, and no prebuilt game program is released. The repository holds the port's own code plus the small references it needs to find its place in the game's program: function addresses, patch values and short context lines from the generated code (`tools/hooks/` and `project/r3_patches.cpp`).

## What you need

- A PC running Windows 10 or 11 (64-bit).
- A CPU with AVX2: Intel 4th gen or newer, or any AMD Ryzen. Older CPUs without AVX2 won't run it.
- A Vulkan 1.3 GPU. DLSS needs a GeForce RTX card; everything else (including FSR) works on AMD, NVIDIA and Intel. Only tested on NVIDIA so far.
- Your own dump of **Resistance 3, US disc (BCUS98176), version 1.00**. This is the folder that contains `PS3_GAME`, the same kind of folder RPCS3 uses. Other regions and patched versions are not supported yet.
- About 50 GB of free disk space: ~37 GB for the game files and the rest for the build tools and the build.
- An internet connection for the setup.

## Install

**Quick install (recommended):**

1. Download the release zip from the Releases page and unzip all of it to a folder with no spaces in the path, for example `C:\Games\Resistance3Port`.
2. Run **`Setup.exe`** in that folder. (It is not code-signed: if Windows says *"Windows protected your PC"*, click **More info → Run anyway**.)
3. Pick your game: **ISO...** or **Folder...**. Setup checks the copy at once and says what to do if it can't be used (see *If your game copy is not accepted* below).
4. Click **Install** and wait. Setup installs the missing build tools (one administrator prompt), copies your game files, builds the game with as many parallel jobs as your RAM allows, adds Start-menu and desktop shortcuts and starts the launcher. It shows the progress and the time left. If it stops (error, sleep, power cut), run `Setup.exe` again and click **Resume**.

The release contains no game code: the game is always built from your own copy. Requirements, disk space, build times, uninstalling and troubleshooting (SmartScreen, antivirus, low RAM, no winget): [docs/INSTALL.md](docs/INSTALL.md).

### Manual install (advanced)

**With the Inno installer** (if a `-Setup-<version>.exe` is on the Releases page): download `Resistance3Port-Setup-<version>.exe` from the Releases page and run it. It installs the port (default `C:\Games\Resistance3Port`), adds Start-menu and desktop shortcuts, and asks for your game. It lists the copies it finds on the PC (common folders and RPCS3's game list); you can also type or paste a path, or use the **ISO...** / **Folder...** buttons. It checks the copy before going on and says what is wrong and what to do if it can't be used (see *If your game copy is not accepted* below). At the end it opens **Setup**, the window with the progress bar, with your game already filled in: click **Install** there (step 2 below). The installer contains only the port; the game files always come from your own copy. Use a decrypted ISO or a game folder (the same kind RPCS3 uses). Tick **Portable mode** to keep settings only in the install folder.

**By hand:**

1. Download this repository: click on Code at the top, then Download ZIP, then unzip it somewhere with no spaces in the path, for example `C:\Games\Resistance3Port`.
2. Double-click **`Setup.bat`**. It will:
   - install the free build tools if they're missing (Visual Studio 2022 Build Tools, LLVM, CMake, Ninja, Python, Git), using `winget`;
   - look for your game (an ISO or a game folder), check it, then copy it into the port folder (or move a folder, if it's on the same drive);
   - decrypt the game's program file, using the application keys from RPCS3's public source code;
   - download RPCS3's source at a fixed version and apply this port's renderer patch (`ps3recomp/patches/r3-rpcs3.patch`);
   - generate the game code from your copy and build the game.

   The first run takes a while: tool installs plus a 20–60 minute build. At the end, Setup offers to put a "Resistance 3" shortcut to the launcher on your desktop.
3. Play with **`Launcher.exe`**. It detects your PC and recommends settings (**Auto-detect**), and has all the settings (display mode, resolution, frame rate, VSync, anti-aliasing, upscaling, texture filtering, sharpening, frame generation, controls, notifications, mods and updates) and the key bindings. See [Launcher](#launcher).

You only need to run Setup again after downloading a newer version of the port; otherwise just use the launcher.

**Choosing the game in Setup.bat:** it lists the copies it found, numbered. Type a number, or type or paste the path of the ISO or game folder (you can also drag it into the window). **B** opens a file picker (Windows only). `Setup.bat -Game "<path>"` skips the question. The path can be an ISO, the game folder, or a file inside it. An ISO is opened by Windows, copied from and closed again; if Windows can't open it, Setup reads it directly.

### If your game copy is not accepted

The installer and Setup check the copy before copying anything. The supported version is **Resistance 3 (US disc, BCUS98176)**, about 34.6 GB.

| Message | What to do |
|---|---|
| compressed archive (.zip, .7z, .rar) | Extract it first, then pick the ISO or folder inside. |
| PlayStation Store package (.pkg) / game update folder | Pick the disc copy. Game updates are not needed. |
| not &lt;game&gt; / from another region | Use the supported disc version listed above. |
| PlayStation Store version | Only the disc version is supported. |
| encrypted disc image / PARAM.SFO is not valid | Decrypt the ISO first (for example PS3Dec with the disc key from the `.dkey` file), or use a game folder from a PS3 or RPCS3. |
| program file was decrypted or replaced (plain ELF) | Use the original, unmodified files. Setup decrypts them itself. |
| files are missing / smaller than expected | The copy is incomplete. Copy or dump it again. |
| different from the known file (warning) | The copy may be modified. You can continue; Setup stops later if the decrypted program is not the supported version. |
| not enough free space | Free up space, or install on another drive. |

The checks run on your PC only. Nothing is uploaded.

**Updates:** the launcher checks this repository's Releases page once at start and says when a newer version is out (it only opens the download page; nothing is downloaded or replaced). Turn it off with the launcher's *Check for updates* option (Advanced page) or the environment variable `PORT_NO_UPDATE_CHECK=1`. The installed version is in the `VERSION` file.

**Settings and controls** (`settings.ini`, `settings.bat`, `controls.txt`, `mouselook.cfg`) are also kept in `%USERPROFILE%\Saved Games\Resistance 3\config`. Missing files are restored from there when the launcher or game starts, so they survive reinstalling or updating the port. To keep everything in the port folder only, create an empty file named `portable.txt` next to `Launcher.exe` (the installer's *Portable mode* does this). Saves are still kept in the port folder (`gamedata`); the uninstaller asks before deleting it, and its default is to keep it.

**No console window:** the launcher starts the game without a console. The game's output goes to `logs\run.log`.

**First launch:** the game shows its own one-time setup screens and an autosave notice. Before the game starts, the launcher prepares the shader cache from your game files (a progress panel; **Skip for now** or Esc skips it), and the game then compiles every known shader behind a **Compiling shaders N / M** panel (see *Shader precompile*), so levels play without shader stutter from the first visit. Shaders are cached for later starts.

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

With the XInput backend (`PAD_BACKEND=xinput`), a Sony DualShock 3/4 or DualSense that is plugged in (detected by its USB/Bluetooth ID, used through DS4Windows / Steam Input as XInput) keeps the PlayStation symbols; with SDL3 (default) the prompts follow the controller type, see *Controllers (SDL3)* below. The launcher's **Controller button prompts** option forces Xbox or PlayStation.

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

**Controllers (SDL3).** The port reads controllers through SDL3 (built in, no extra DLL): Xbox, DualShock 4, DualSense, DualShock 3 (with its Windows driver), Switch Pro and Joy-Con work directly. **DS4Windows and Steam Input are no longer needed** (they still work; if they expose a virtual Xbox pad next to the real controller, the port ignores the duplicate so it doesn't become a ghost player 2). Controllers can be plugged in and out at any time, no restart, up to 8. Rumble works on every pad that has it (DualSense included), and a DualShock 4 / DualSense light bar shows the player's colour (player 1 blue, player 2 red). If SDL3 can't start, the port falls back to XInput automatically (`logs\run.log` shows `[cellPad] controllers: SDL3` or `SDL3 could not start`).

**Button prompts by controller type:** with *Controller button prompts = Auto*, a PlayStation pad (DualShock 3/4, DualSense) shows PlayStation symbols and an Xbox or other pad shows Xbox letters. Switch controllers are read by button position (the bottom button is Cross); with *Nintendo button layout = Nintendo style* A (right) confirms and B cancels as on a Switch, and the prompts show the Xbox-style letters. Under Steam Input the real controller type is used when SDL reports it; otherwise set the prompts by hand.

**Gyro aiming** (DualShock 4, DualSense, Switch Pro, Joy-Con; off by default): turning the controller turns the view, added directly to the camera like the mouse (smooth, not limited by the stick). *While aiming* only uses it with the Aim button (L1 / LB) held. Leave the controller still for about a second after it connects so the resting drift is calibrated. In split-screen co-op each player's gyro moves only their own view (as a right-stick input, like the second player's mouse).

| Option (launcher Controls page / in game *More PC Options*) | Values | Variable |
|---|---|---|
| Controller support | **SDL3**, XInput | `PAD_BACKEND=sdl3\|xinput` (restart). `sdl2` is accepted and uses SDL3 |
| Gyro aiming | **Off**, On, While aiming | `PAD_GYRO=0\|1\|aim` |
| Gyro sensitivity | **2x**, 0.5x-6x | `PAD_GYRO_SENS=<0.05..20>` (view degrees per controller degree) |
| Gyro vertical | **Normal**, Inverted | `PAD_GYRO_INVERT_Y=1` (on top of the game's own invert setting) |
| Gyro turning axis | **Controller**, Player | `PAD_GYRO_SPACE=local\|player` (player = yaw around gravity, works flat or upright) |
| Gyro calibration | **On connect**, Off | `PAD_GYRO_CALIBRATE=0` turns off the 1 s still-calibration |
| Light bar | **Player colour**, Off | `PAD_LIGHTBAR=0` |
| Nintendo button layout | **By position**, Nintendo style | `PAD_NINTENDO_SWAP=1` |
| (no launcher option) | **On** | `PAD_DEDUP=0` keeps virtual duplicates (Steam Input / DS4Windows / ViGEm) as separate controllers |
| (no launcher option) | **360** | `PAD_GYRO_STICK_DPS=<deg/s>`: gyro speed that equals a full right stick (co-op / classic mouse aiming, where gyro drives the stick) |

With `PAD_BACKEND=xinput` the old behaviour returns: XInput only (up to four), PlayStation pads through DS4Windows / Steam Input, no gyro or light bar.

## Local co-op

Two players on one PC, split-screen, as on PS3. No PSN account is needed: the port provides a second local user.

**Starting it:** plug in two controllers (or one, with *Player 2 uses keyboard and mouse*). Main menu → **Campaign** → the co-op / split-screen option. On the lobby screen, press **START on the second controller** (or Enter / Esc on the keyboard for a keyboard player 2): on PS3 this opens the "select a user" box; the port answers it at once with a local user named **Player 2**, and player 2 is in. Start the chapter and the screen splits.

Resistance 3 counts at most two local players, ignores PlayStation Move pads, and won't let one user join twice (so player 2 is always the second local user).

**Who is who:**

- Player 1 = the keyboard and mouse plus the first controller found (as in single player).
- Player 2 = the next controller. A controller keeps its player while it stays plugged in. A controller plugged in later takes the lowest free player; when one is unplugged, its player has no input until it is plugged back.
- With two local players the direct mouse look is off. The mouse then moves the keyboard player's view like a right stick (it would otherwise turn both heroes).

**Options** (launcher, Controls page; or `settings.bat`):

| Option | Values | Variable |
|---|---|---|
| Local co-op (split-screen) | **On**, Off | `PAD_COOP=0`: the game sees only player 1. Extra controllers (or duplicate virtual pads from Steam Input / DS4Windows) can't join |
| Player 1 controller | **Automatic**, Controller 1-8 | `PAD_P1_CONTROLLER=<1..8>`: the controller that is player 1 (SDL3: in the order they were connected; XInput: 1-4, Windows' XInput order) |
| Player 2 controller | **Automatic**, Controller 1-8 | `PAD_P2_CONTROLLER=<1..8>`: the controller that is player 2 (numbered as above) |
| Player 2 uses keyboard and mouse | **Off**, On | `PAD_P2_KEYBOARD=1`: player 1 = the first controller only, player 2 = keyboard and mouse |
| (no launcher option) | **2** | `PS3_LOCAL_USERS=<1..4>`: local users the game sees. `1` = player 2 can't sign in. `PS3_USER2_NAME=<name>` renames player 2 |

`logs\run.log` shows which controller each player got (`[cellPad] player 2 <- controller 2`) and when the game counts two local players (`[coop] local players: 2`).

**Known limits:** Player 2's progress and settings are not saved separately (as on PS3 for a guest player). If you use NVIDIA DLSS, it may not work right in split-screen. Turn it off (or use FSR / no upscaler) for co-op if the image smears or one half looks wrong.

## PC features

- **PC menu:** press **F10**, or hold **View + Menu** (Select + Start) on a controller for 1.5 s, for a small overlay menu: **Resume**, **PC Options** and **Quit to Desktop**. The game is paused while it is open. *PC Options* changes, live: frame rate, upscaler (DLSS / FSR 3.1 / off; applies at the next start), mouse sensitivity, field of view and controller prompts (Auto / Keyboard keys / Xbox / PlayStation). Each change is saved where the launcher keeps it (`settings.ini` + `settings.bat`, `controls.txt`). Arrow keys / D-pad move, Enter / A select, Esc / B back. The game's own pause and main menus are Scaleform (Flash) movies the port doesn't edit, so a small hint, *F10: PC menu*, appears while the pause menu is up and once at start. `R2_MENU_PC=0` turns the menu off (F10 / View+Menu then go straight to the quit box), and `R2_MENU_PC_HINT=0` hides the hint (it is off by default in exclusive fullscreen).
- **PC wording:** the game's text is adjusted where it talks about the console (rules in `project\r3_pctext.cpp`). In English: the epilepsy warning's "BEFORE USING SEE THE INSTRUCTION MANUAL INCLUDED WITH YOUR PS3™ SYSTEM FOR MORE DETAILS." → "STOP PLAYING AND CONSULT A DOCTOR IF YOU FEEL UNWELL.", "Press and hold the PS button to quit the application" → "Press F10 to quit the game", "Insufficient Hard Disk Drive space" → "Insufficient disk space", "Do not switch off the power when the HDD access indicator is flashing" → "Do not close the game while it is saving", "Logging into PlayStation®Network" → "Signing in to online services", "PlayStation®Network account" → "online account", "PlayStation®Store" → "online store", "USB keyboard" → "keyboard", and "wireless controller" → "controller". Other languages: "PLAYSTATION®3" / "PS3" → "PC", "PlayStation®Network" → "Online", "PlayStation®Store" → "Store", and the PS button in quit messages → F10. Credits, trademark lines and PlayStation Move / Eye messages are left as they are. Only the text in memory is changed (the game files aren't touched). `R3_PC_TEXT=0` turns it off (`R2_PC_TEXT` is still read when `R3_PC_TEXT` is unset).
- **Quit to desktop:** *Quit to Desktop* in the PC menu (or F10 / View + Menu with `R2_MENU_PC=0`). A small box asks *Quit to desktop?*: Enter / Space / A = yes, Esc / Backspace / B = no. While it is shown the game is paused and gets no input. On yes (and also when the window is closed with Alt+F4 or the X button) the port waits for a save in progress to finish (up to 15 s, "Saving..."), flushes open files and exits. The game's own menus have no Quit entry; adding one would mean patching the game's Flash menus without being able to test them, so the PC menu carries it instead.
- **Alt+Enter** switches between borderless fullscreen and windowed. **Alt+Tab** is safe in borderless and windowed mode.
- **Monitor** (launcher, Display › Screen): which screen the window opens on (`RSX_MONITOR=<n>`, 1-based in Windows' order; empty = primary).
- **When the game is in the background** (launcher): *Pause and mute* (default) freezes the game and silences it when the window loses focus; *Mute only*; *Keep running* (`R2_BACKGROUND=pause|mute|run`). On focus loss the cursor is always released and shown; it is also shown while the quit box is up or the game is paused this way, and captured/hidden again when you click back in.
- **Skip intro movies** (launcher, default on): the studio logo video and the opening movie are skipped, so the game starts at the title screen (`SKIP_INTRO=1`, set by `run.bat`; `SKIP_INTRO=0` to watch them).
- **Frame rate in menus** (launcher): caps the frame rate at 60 or 30 fps whenever you are not controlling your character (menus, loading screens, cutscenes); gameplay keeps the normal frame rate (`PORT_MENU_FPS=<fps>`, minimum 30; empty/0 = off, the default).
- **Screenshots:** **F12** or **PrtScn** saves `screenshots\R3_<date>_<time>.png` in the game folder (a short "Screenshot saved" note appears). The launcher's About page has **Open screenshots**.
- **Saves** are in the game folder: `gamedata\dev_hdd0\home\00000001\savedata\BCUS98176_SAVE_0` (game settings are saved there too, as on the PS3). The launcher's **Saves & tools** page backs them up and restores them (see *PC extras*). **Open save folder** (launcher, About) opens it. `PS3_GAMEDATA_ROOT=<dir>` moves the whole `gamedata` tree elsewhere (saves then go to `<dir>\dev_hdd0\home\00000001\savedata`). The port's own settings are `settings.ini` / `settings.bat` and `controls.txt` in the game folder.
- **Reset all to defaults** (launcher, About) resets display, graphics, PC options, keys and mouse (saves untouched). **Auto-detect** (Display, Graphics and Upscaling & Frame Gen pages) resets only display and graphics; **Restore default controls** only the controls.
- **Crash and hang reports:** if the game crashes, a minidump is written to `logs\crash\R3_<date>_<time>_crash.dmp` and a one-line summary is added to `logs\crash\crashes.txt`. If no game frame is drawn for 90 s while the game isn't paused, an `R3_<date>_<time>_hang.dmp` with every thread's state is written once per stall and the game keeps running. Send these with the session's log when reporting a problem. `R2_HANG_S=<seconds>` (≥ 10) changes the hang limit, `R2_CRASH_DUMPS=0` turns reports off, `R2_CRASH_DUMPS=full` writes full-memory dumps (large).
- **Renderer fatal errors:** if the RSX renderer thread hits a fatal error, the reason is printed to the console/log as `[rpcs3] thread '...' terminated due to fatal error: ...` (and to `logs\rpcs3_rsx.log`). The previous run's `rpcs3_rsx.log` is kept as `logs\rpcs3_rsx.prev.log`, so it survives one restart. A dead renderer thread looks like a hang (the game stops drawing but keeps running), so include these lines when reporting one.
- **Performance overlay:** **F3** shows or hides frame rate, frame time (average and worst) and, with the Compatible renderer, draws per frame and the RSX thread's time per draw, in a corner of the game window. Launcher *Performance overlay*: Off (F3 shows it, the default), On at start, Disabled (F3 does nothing) = `R2_OVERLAY` unset / `1` / `0`; `R2_OVERLAY_POS=tl|tr|bl|br` picks the corner (top left by default). It is drawn into the game's own image (`PORT_OSD`), so it also shows in exclusive fullscreen. While it is shown the renderer's counters are on (as with `R2_RSX_PROF=1`, without the log output).
- **Trophies as local achievements:** when the game awards a trophy the port shows a small notification in the corner of the window and records it in `%USERPROFILE%\Saved Games\Resistance 3\trophies.json` (name, grade, date). The trophy list (59 trophies, comm id `NPWR01841`) is read at run time from your own game files (`gamefiles\PS3_GAME\TROPDIR\NPWR01841_00\TROPHY.TRP`); nothing of it ships with the port. The platinum trophy is awarded when every trophy it counts is unlocked. The runtime's own record stays in `gamedata\trophies\NPWR01841.json`. The launcher's **Trophies** page shows a summary at the top (earned / total, percentage, a progress bar and the count per grade), then every trophy with its icon from `TROPHY.TRP` (greyed until earned), name, description, grade and the date it was earned, newest first. Hidden trophies show as *Hidden trophy* until earned. It merges both records (a trophy counts if either has it; the earliest time wins), so the platinum the port awards also shows. Before anything is earned it says *Play the game to start earning trophies*. The page reads nothing until it is opened, re-reads only when those files change, and never writes to them. `R2_TROPHY_TOAST=0` (launcher *Trophy notifications: Off*) records them without the notification.
- **Mods folder:** launcher *Mods folder: On* (`R2_MODS=1`) loads replacement game files from the `mods` folder next to the game; `R2_MODS=<folder>` uses another folder. Each sub-folder is one mod laid out like the game's `PS3_GAME` folder: `mods\<mod>\USRDIR\...` replaces the same file under `gamefiles\PS3_GAME\USRDIR`. Names are matched without regard to case; when two mods replace the same file, the one whose folder name sorts last wins; folders starting with `_` are skipped (a quick way to disable one). Every replaced file is listed in `logs\run.log` as `[mods]` lines. Single members of a `.psarc` archive and single level assets (by tuid) can be replaced too: see [docs/MODDING.md](docs/MODDING.md).
- **Ultrawide:** not available yet. Resistance 2's port widens the 3D view on 21:9 / 32:9 screens by hooking the game's camera-frustum setters; the matching functions in Resistance 3 could not be identified with certainty, so wider screens show the 16:9 picture with side bars.
- These window features (F10, F3, F12, pause on focus loss, monitor choice) work with both renderers. The port's menus, overlay and notifications are drawn into the game's own image (`PORT_OSD`, default), so they also show in exclusive fullscreen; `PORT_OSD=layered` uses the old separate windows, which exclusive fullscreen hides. Exclusive fullscreen can still give black screenshots; borderless is recommended.

### PC extras

All of these are in the launcher (on the Display, Graphics, Upscaling & Frame Gen, Controls, Interface & Notifications and Advanced pages, see *All launcher options*, and the **Saves & tools** page) and most also in game: **F9**, or **F10 › More PC Options**. The in-game page writes the same `settings.ini` / `settings.bat` lines as the launcher. Rows marked *(next start)* apply after a restart; the rest apply at once. Code: `project\r3_pcextra.cpp` (game side), `project\launcher_extra.inc` (launcher), `ps3recomp\third_party\rpcs3\rpcs3\Emu\RSX\VK\VKR2PcExtras.cpp` (Vulkan HDR / colour-blind / Reflex). New switches use the `R3_` prefix.

- **Skip intro movies** — `SKIP_INTRO` (above, default on). The epilepsy / legal notice is a game screen that waits for a button and is not skipped (the code that sequences it was not located).
- **Key rebinding** — keyboard and mouse: launcher Controls page or in game *More PC Options › Key bindings* (Enter = press the new key, Esc = cancel; written to `controls.txt` and re-read at once). Controller: launcher *Controller buttons* or in game *More PC Options › Controller layout* — each Xbox button can act as another one (`PAD_MAP_<button>=<button>`, e.g. `PAD_MAP_A=B`), `PAD_SOUTHPAW=1` swaps the sticks. The remap is applied to the XInput state before the game sees it.
- **Automatic prompts** — `R2_PAD_GLYPHS=auto` (default): the prompts follow the last device used. Already in the port; listed for completeness.
- **Post-effect toggles** — `R3_FX_FILMGRAIN`, `R3_FX_DOF`, `R3_FX_MOTIONBLUR` (default `1` = the game's own look; `0` = off). Also in game (*More PC Options*, F9) and in the launcher; the in-game switch applies at once. Each frame the engine blends its post volumes and Lua `pfx_*` nodes into a per-view post block (0x12B3E80 + view × 0x230) and then calls one function per effect with a parameter struct. Each function returns early when its strength is zero, so the hooks in `tools/hooks/r3.json` zero only that strength before the function reads it, and the game takes its own "effect off" path:

  | Effect | Function | Field zeroed | Skip test in the game |
  |---|---|---|---|
  | Film grain | `func_00744A18` | p+0x08, grain intensity (block 0xEC) | ≤ 0 |
  | Camera motion blur | `func_007424F8` | p+0x14 (block 0xD8) | ≤ 0 |
  | Object (per-moby) motion blur | `func_00855A98` | p+0x0C, sample scale | < 0.01 |
  | Depth of field | `func_008557A8` | p+0x28 / p+0x38, near / far max blur (block 0xC4 / 0xD0) | both < 0.01 |

  Only the stack copy is changed, so turning an effect back on restores the level's exact values. The Lua `pfx_set_node_*` natives (registered at 0x8EA7E0 through `lis`/`addic` pairs, which is why a plain pointer search finds no references) feed the same block, so script-driven effects are covered too.
- **Colour-blind filter** — `R3_COLORBLIND=1|2|3` (protanopia / deuteranopia / tritanopia): daltonization in the final output pass (Compatible renderer). Applies at once.
- **Extra crosshair** — `R3_CROSSHAIR=dot|cross`: a small marker at the window centre during play (hidden in menus, the PC menu and photo mode).
- **Window** — `RSX_RESIZABLE` (default on): the windowed-mode window has a sizing border and the swapchain follows the new size. `RSX_ALT_ENTER` (default on): Alt+Enter switches windowed ↔ borderless fullscreen.
- **Frame rate** — the cap is the port's frame limiter (`R2_FPS`, launcher *Frame rate*); there is only one limiter. Above 60 fps the game clock follows real time through the ported Unlock-FPS hooks (frame-time floor and game-clock clamp, see *RPCS3 patches*), so the game does not speed up; frame-counted logic was not audited frame by frame (as UnleashedRecomp did for its game), so treat 120 / 144 / Uncapped as less tested than 60.
- **NVIDIA Reflex** — `R3_REFLEX=1` (`2` = + Boost), opt-in, Compatible renderer, GeForce with `VK_NV_low_latency2`: low-latency mode plus the Reflex sleep at the start of each frame. It does not cap the frame rate; no latency markers are sent. Does nothing when the extension is missing.
- **DLSS Frame Generation** — `R3_DLSS_FG=2|3|4` (launcher: Upscaling & Frame Gen › *Frame Generation*; `0`/unset = Off), Compatible renderer, GeForce RTX 40 (2x) or RTX 50 (up to 4x Multi Frame Generation), Anti-aliasing = NVIDIA DLSS, setup's optional NVIDIA Streamline download. The GPU inserts 1-3 generated frames between the game's own (`ps3recomp\third_party\rpcs3\rpcs3\Emu\RSX\VK\VKR2FrameGen.cpp`, NVIDIA Streamline 2.14.16, manual hooking). Smoother motion when the CPU is the limit; adds a little input lag, so Reflex is switched on with it (Streamline drives Reflex while FG is set up; `R3_REFLEX=2` = Boost). The frame-rate setting caps the game's own frames; the Reflex cap keeps rendered × multiplier under the refresh rate. The HUD is split from the scene in the renderer (HUD draws after the 3D pass go to a UI layer that is composited after generation), so HUD text does not warp; when a frame's HUD can't be separated cleanly it is passed through untouched. FG pauses in menus, movies, loading screens, split-screen, while the game is paused and on scRGB HDR, and turns itself off (one `[fg-vk] frame generation off: ...` log line) on other GPUs, without the DLLs, or with FSR / XeSS / no AA. Debug: `R3_DLSS_FG_HUD=0` (no HUD split), `R3_DLSS_FG_HUD_DEBUG=1` (show the HUD-less image), `R3_DLSS_FG_MENU_OFF=0`, `R3_DLSS_FG_CAP=0`, `R3_DLSS_FG_YFLIP=0`, `R3_DLSS_FG_DEBUG=1`. With a fixed-refresh display and no G-Sync, vsync is replaced by the Reflex cap and may tear.
- **FSR 3 Frame Generation (fallback)** — `R3_DLSS_FG=fsr3` (launcher: *Frame Generation* › *FSR 3 2x*, listed on GPUs that are not RTX 40/50), or `R3_FG_BACKEND=auto|dlss|fsr3` (auto = default: DLSS-G first, then FSR 3 2x when DLSS-G is unavailable; `dlss` = DLSS-G only; `fsr3` = FSR 3 only, also on RTX cards). AMD FidelityFX frame interpolation (FidelityFX API 1.1.4, `tools\ffx_vk\amd_fidelityfx_vk.dll`, MIT, shipped with the port) on any Vulkan GPU with a spare present-capable queue and timeline semaphores; 2x only. Needs Anti-aliasing = FSR, DLSS or XeSS (for depth and motion vectors) and SDR or HDR10 output. Uses the same inputs, HUD split (the HUD-less scene goes to interpolation, the HUD layer is composited on every shown frame), camera-cut resets and pause rules as DLSS-G. No Reflex while it is selected (FidelityFX's swapchain has no low-latency mode), so it adds a little more input lag than DLSS-G; the game's frame cap still applies to its own frames and FSR 3 paces the generated ones. Turning it on or off mid-game waits for the GPU once (a short hitch). HDR10 works on the usual A2B10G10R10 swapchain; a driver that only offers A2R10G10B10 for HDR10, and scRGB HDR, keep FG paused. Debug: `R3_DLSS_FG_DEBUG=2` (FidelityFX tear lines and reset markers); the HUD switches above apply too. Off reasons are logged as `[fg-vk] ...` lines.
- **HDR output** — `R3_HDR=1` (HDR10 / PQ) or `R3_HDR=scrgb` (FP16 linear), opt-in, Compatible renderer, Windows HDR must be on. The final pass converts the SDR image to linear and scales SDR white to `R3_HDR_PAPER` nits (default 200), then encodes PQ or scRGB; `R3_HDR_PEAK` (default 1000) is sent as `VK_EXT_hdr_metadata` and caps highlights. Falls back to SDR when the display/driver has no HDR surface format (logged). The F3 overlay, PC menu and screenshots stay SDR.
- **Hold to skip movies** — `R3_CUTSCENE_SKIP` (default on): hold Space / Enter / A / Menu for 1 s while a pre-rendered movie plays and the port presses the game's own skip button. Loading-screen movies and in-engine cutscenes are not skipped (not safe). The *Hold Space / A to skip* notification shown when a movie starts can be hidden with launcher *Show cutscene skip hint: Off* (`PORT_SKIP_HINT=0`, or `R3_SKIP_HINT=0`); holding still skips.
- **Subtitles** — not available: they are drawn by the game's Scaleform movies at a fixed size.
- **Discord Rich Presence** — `R3_DISCORD=1`, opt-in. Talks to the local Discord app over `\\.\pipe\discord-ipc-0..9` (no SDK): *Resistance 3* plus the current level (from the level folder the game opens; menus and lobby show *In the menus*). Needs an application id from discord.com/developers in `discord_appid.txt` in the game folder or `R3_DISCORD_APPID`. Silent when Discord isn't running (retries every minute).
- **Photo mode** — `R3_PHOTO` (default on): **F7** pauses the game and hides the port's overlays; **F12** takes the screenshot; F7 again resumes. No free camera and no separate HUD hide (the camera and HUD are not hooked).
- **Pause when the controller disconnects** — `R3_PAD_DISCONNECT_PAUSE` (default on): the game pauses with a message until the controller is back or Enter is pressed.
- **Benchmark** — launcher *Saves & tools › Run benchmark* (or `R3_BENCH=1`): a fixed input script from start-up into the campaign (`R3_BENCH_SCRIPT`, the `scratch\r3bench.ps1` route), measures `R3_BENCH_SECONDS` (default 60) and quits (`R3_BENCH_QUIT=0` keeps it running). Writes `logs\bench\R3_bench_<date>.txt` (average, 1 % and 0.1 % low fps, frame-time percentiles, draws per frame, µs per draw) and appends `logs\bench\bench_history.csv`. The button also sets `R2_BACKGROUND=run` for the run.
- **Save manager** — launcher **Saves & tools**: lists the saves (title, date, size), *Back up now*, per-backup *Restore* (current saves are backed up first) and *Delete*, and *Open* buttons. `R3_SAVE_BACKUP` (default on, newest 10 automatic backups kept): Play copies the save folder to `save_backups\<date>_auto` when it changed since the newest backup.

**How to check them in game:** F9 opens *More PC Options*; the colour-blind filter changes colours at once; F7 = photo mode; unplug the controller during play = pause message; hold Space during a movie = skip; drag the window edge in windowed mode; Alt+Enter; `logs\run.log` lines start with `[r3x]`.

## Launcher

`Launcher.exe` has its own icon and artwork (original, drawn by `tools\art\make_r3_art.py`; nothing from the game). It writes your choices to `settings.ini` (its own) and `settings.bat` (`set "NAME=value"` lines that `run.bat` loads before starting the game), and the key bindings to `controls.txt`.

**Recommended for this PC / Auto-detect.** On first start the launcher detects your GPU (name, video memory, RTX or not, integrated or not), CPU cores/threads, RAM and screen (resolution and refresh rate), and shows a *Recommended* panel with a Low / Medium / High / Ultra tier and the settings it would use. **Auto-detect** applies them (display and graphics only). The same table is used by the game when `R2_PRESET=auto|low|medium|high|ultra` is set: it fills in every setting you did not set yourself. GPUs with DLSS get DLSS, others FSR 3.1 (or FSR 1 scaling on weak integrated GPUs). From a command prompt, `Launcher.exe --detect` prints what it found and the `settings.bat` it would write, and `Launcher.exe --table` prints the recommendation for a list of typical PCs (`--sim-gpu "<name>" --sim-vram <GB> --sim-cores <n> --sim-threads <n> --sim-ram <GB> --sim-screen <w>x<h>@<hz>` simulate other hardware).

**1080p rendered, DLSS output at 4K (supersampled on a 1080p screen):** Render resolution **3840 x 2160**, Anti-aliasing **NVIDIA DLSS**, Upscaling **Performance (50%)**, i.e. `RSX_RES_SCALE=3`, `RSX_FSR=2`, `RSX_UPSCALE=0.5`. The 3D scene is drawn at 1920x1080 and DLSS reconstructs 3840x2160, which is then scaled to your screen. This is the launcher's default. If DLSS isn't available (not an RTX card, or the DLSS runtime is missing) the port uses AMD FSR 3.1 instead, and if FSR 3.1 can't start either, FSR 1 spatial upscaling.

### All launcher options

The options are sorted into pages, each with group headings (the same pages as the Resistance: Fall of Man launcher). `Launcher.exe --page N` opens on page N: 0 Display, 1 Graphics, 2 Upscaling & Frame Gen, 3 Performance, 4 Controls, 5 Audio, 6 Interface & Notifications, 7 Advanced, 8 Trophies, 9 Saves & tools, 10 About. The Display, Graphics and Upscaling & Frame Gen pages start with the *Recommended for this PC* panel; **Rebuild shader cache** is on the Performance page. Any option no group lists would appear on the Advanced page under *Other*.

| Page | Option | Values (default first or marked) | Environment variable |
|---|---|---|---|
| Display › Screen | Display mode | Borderless, Exclusive fullscreen, Windowed | `RSX_DISPLAY` |
| Display › Screen | Window size | 1280x720 … 3840x2160 (**1920x1080**) | `RSX_WINDOW` |
| Display › Frame rate | Frame rate | 30, **60**, 120, 144, Match display, Just under refresh (G-Sync / FreeSync), Uncapped | `R2_FPS` (`30`, `<fps>`, `refresh`, `vrr`, `0`) |
| Display › Frame rate | VSync | **Off**, On, Adaptive (FIFO relaxed), Auto (On when capped at or under the refresh) | `RSX_VSYNC` |
| Display › Screen | Monitor | **Primary**, Monitor 1…n | `RSX_MONITOR` |
| Display › Frame rate | Frame rate in menus | **Same as in game**, 60 fps, 30 fps | `PORT_MENU_FPS` |
| Display › Frame rate | Presentation mode (Compatible renderer) | **Default (follow VSync)**, Immediate (VRR / tearing), Mailbox, FIFO | `R2_PRESENT` (`immediate` / `mailbox` / `fifo`) |
| Display › Frame rate | Refresh rate switch (exclusive fullscreen) | **Off**, On | `PORT_REFRESH_SWITCH` (`1` = on, experimental) |
| Display › Screen | Resizable window | **On**, Off | `RSX_RESIZABLE` (`0` = off) |
| Display › Screen | Alt+Enter | **On**, Off | `RSX_ALT_ENTER` (`0` = off) |
| Display › HDR | HDR output (experimental) | **Off**, HDR10, scRGB | `R3_HDR` (`1` / `scrgb`) |
| Display › HDR | HDR paper white / peak brightness | **200** nits … / **1000** nits … | `R3_HDR_PAPER`, `R3_HDR_PEAK` |
| Graphics › Resolution | Render resolution | 720p (PS3) … **3840x2160** | `RSX_RES_SCALE` (1, 1.5, 2, 2.5, **3**) |
| Graphics › Quality | Texture filtering | **Trilinear**, Anisotropic 2x/4x/8x/16x | `RSX_ANISO` |
| Graphics › Resolution | Scaling (to the screen) | **Smooth**, Sharp pixels, AMD FSR 1 | `RSX_FILTER` (`linear` / `point` / `fsr`) |
| Graphics › Renderer | Renderer | **Compatible (RPCS3)**, Native Vulkan (experimental) | `R2_RSX` (`rpcs3` / `vulkan`) |
| Graphics › View | Field of view (experimental) | **Original**, Wider (0.85), Widest (0.7) | `R2_PATCH_FOV_VALUE` |
| Graphics › Quality | Shadow resolution | **Original**, 2x, 4x | `RSX_SHADOW_SCALE` (`2` / `4`) |
| Graphics › Quality | Effects resolution | **Original**, Full, Full (all) | `RSX_FX_FULLRES` (`1` / `2`) |
| Graphics › Quality | Ambient occlusion | **Off**, On (experimental) | `RSX_SSAO`. Ambient occlusion is disabled with DLSS in this beta (`RSX_SSAO_DLSS=1` forces it on). |
| Graphics › Effects and accessibility | Film grain | **On**, Off | `R3_FX_FILMGRAIN` (`0` = off) |
| Graphics › Effects and accessibility | Depth of field | **On**, Off | `R3_FX_DOF` (`0` = off) |
| Graphics › Effects and accessibility | Motion blur | **On**, Off | `R3_FX_MOTIONBLUR` (`0` = off) |
| Graphics › Effects and accessibility | Colour-blind filter | **Off**, Protanopia, Deuteranopia, Tritanopia | `R3_COLORBLIND` (`1` / `2` / `3`) |
| Upscaling & Frame Gen › Anti-aliasing and upscaling | Anti-aliasing | Off, AMD FSR 3.1, **NVIDIA DLSS** | `RSX_FSR` (0 / 1 / **2**) |
| Upscaling & Frame Gen › Anti-aliasing and upscaling | Upscaling | Native (DLAA), Quality 67%, Balanced 58%, **Performance 50%** | `RSX_UPSCALE` (1, 0.667, 0.58, **0.5**) |
| Upscaling & Frame Gen › Anti-aliasing and upscaling | Sharpening | Off, **Low**, Medium, High | `RSX_SHARPEN` (0 / 0.25 / 0.5 / 0.8) |
| Upscaling & Frame Gen › Frame generation and latency | NVIDIA Reflex (experimental) | **Off**, On, On + Boost | `R3_REFLEX` (`1` / `2`) |
| Upscaling & Frame Gen › Frame generation and latency | Frame Generation (RTX 40/50: DLSS-G, needs NVIDIA DLSS; other GPUs: FSR 3 2x) | **Off**, 2x, 3x, 4x, FSR 3 2x (only what the GPU supports) | `R3_DLSS_FG` (`2`/`3`/`4`/`fsr3`), `R3_FG_BACKEND` |
| Performance › Processor | CPU profile | **Auto**, Low (4 cores), Mid (6 cores), High (8+) | `PORT_CPU_PROFILE` |
| Performance › Shader cache | Shader compilation | **Asynchronous**, Asynchronous + interpreter, Synchronous | `R2_SHADER_MODE` (`async` / `interp` / `sync`) |
| Controls › Controller | Controller button prompts | **Auto**, Xbox, PlayStation, Keyboard keys | `R2_PAD_GLYPHS` (`xbox` / `ps` / `keys`) |
| Controls | Keys (two per action), Mouse sensitivity, Invert vertical look, Mouse aiming (Direct / Classic), Free the mouse cursor key (F8) | | `controls.txt` |
| Controls › Aiming and sticks | Vertical mouse sensitivity, Aiming sensitivity, Aim mode, Stick deadzone, Stick response curve | see *Controls* above | `PAD_MOUSE_SENS_Y`, `R2_ADS_SENS`, `PAD_AIM_TOGGLE`, `PAD_DEADZONE`, `PAD_CURVE` |
| Controls › Controller, Gyro | Controller support, Gyro aiming, Gyro sensitivity, Gyro vertical, Gyro turning axis, Gyro calibration, Light bar, Nintendo button layout | see *Controllers (SDL3)* under Controls | `PAD_BACKEND`, `PAD_GYRO`, `PAD_GYRO_SENS`, `PAD_GYRO_INVERT_Y`, `PAD_GYRO_SPACE`, `PAD_GYRO_CALIBRATE`, `PAD_LIGHTBAR`, `PAD_NINTENDO_SWAP` |
| Controls › Aiming and sticks | Extra crosshair | **Off**, Dot, Cross | `R3_CROSSHAIR` (`dot` / `cross`) |
| Controls › Controller | Pause when the controller disconnects | **On**, Off | `R3_PAD_DISCONNECT_PAUSE` (`0` = off) |
| Controls › Controller, Controller buttons | Swap sticks (southpaw), Button A … Right stick click | **Off** / **same button** | `PAD_SOUTHPAW`, `PAD_MAP_<button>=<button>` |
| Audio › In the background | When the game is in the background | **Pause and mute**, Mute only, Keep running | `R2_BACKGROUND` |
| Audio › Sound | Speaker layout | **5.1 surround**, 7.1 surround, Stereo | `PORT_SPEAKERS` (`8` / `2`) |
| Audio › Sound | Audio buffer | **Auto** (100 ms; 150 ms under 12 CPU threads), 80, 100, 150, 200 ms | `PORT_AUDIO_BUFFER_MS` |
| Interface & Notifications › Overlays | Performance overlay | **Off (F3 shows it)**, On at start, Disabled | `R2_OVERLAY` (unset / `1` / `0`) |
| Interface & Notifications › Notifications | Trophy notifications | **On**, Off | `R2_TROPHY_TOAST` (`0` = off) |
| Interface & Notifications › Photo mode and Discord | Discord Rich Presence | **Off**, On | `R3_DISCORD` (`1`), `R3_DISCORD_APPID` |
| Interface & Notifications › Photo mode and Discord | Photo mode (F7) | **On**, Off | `R3_PHOTO` (`0` = off) |
| Interface & Notifications › Movies | Hold to skip movies | **On**, Off | `R3_CUTSCENE_SKIP` (`0` = off) |
| Interface & Notifications › Movies | Show cutscene skip hint | **On**, Off | `PORT_SKIP_HINT` (`0` = off; `R3_SKIP_HINT` also read) |
| Advanced › Start-up | Skip intro movies | **On**, Off | `SKIP_INTRO` |
| Advanced › Mods and textures | Mods folder | **Off**, On | `R2_MODS` (`1` / `<folder>`) |
| Advanced › Mods and textures | Texture pack / Dump textures | **Off**, On | `RSX_TEXPACK`, `RSX_TEXDUMP` (`1` / `<folder>`) |
| Advanced › Saves | Back up saves before playing | **On (keep 10)**, keep 5, keep 30, Off | `R3_SAVE_BACKUP` (`5` / `30` / `0`) |
| Trophies | Progress summary (earned / total, %, per grade), every trophy with its icon, grade and unlock date | | read-only: `TROPHY.TRP`, `gamedata\trophies\NPWR01841.json`, `Saved Games\Resistance 3\trophies.json` |
| Saves & tools | Saved games, backups (Back up now / Restore / Delete / Open), Run benchmark | | `R3_BENCH`, `R3_BENCH_SECONDS`, `R3_BENCH_SCRIPT`, `R3_BENCH_QUIT` |
| About | Open save folder, Open screenshots, Open logs folder, Reset all to defaults | | |

With `R2_PRESET`, any setting you set yourself (in `settings.bat` or the environment) wins over the preset.

## Graphics options

- **Renderer:** the default is the *Compatible* renderer: the game's RSX (PS3 GPU) command stream is fed to RPCS3's RSX core and Vulkan renderer, built into the game as a library (`R2_RSX=rpcs3`). It uses RPCS3's settings from `rpcs3_config.yml` (next to the game, or `R2_RSX_CONFIG=<file>`), with the port's settings on top. *Native Vulkan* (`R2_RSX=vulkan`) is the port's own experimental renderer.
**Frame pacing, VSync and G-Sync / FreeSync** (Compatible renderer)

- Present mode per setting (the log prints it once: `[display] Vulkan present mode FIFO, 3840 x 2160`): VSync *Off* / `R2_PRESENT=immediate` = IMMEDIATE; *On* / `fifo` = FIFO; *Adaptive* = FIFO relaxed (MAILBOX if the driver has no FIFO relaxed); `R2_PRESENT=mailbox` = MAILBOX. *Auto* (`RSX_VSYNC=auto`) = FIFO whenever the frame rate is capped at or under the display refresh (the cap means it never waits on vblank) and Adaptive when uncapped or above the refresh.
- G-Sync / FreeSync follow FIFO, FIFO relaxed and IMMEDIATE. MAILBOX shows the newest frame at the fixed refresh, so VRR does not follow it: 60 fps on a 144 Hz screen then holds frames for 2, then 3 refreshes (judder) even with G-Sync on. Earlier builds mapped *Auto* and *Adaptive* to MAILBOX; they now use FIFO / FIFO relaxed.
- Without VRR, a fixed-refresh display is only even at a divisor of its rate (60 on 60 / 120 / 240 Hz, 72 or 48 on 144 Hz); the log warns (`[fps] note: ...`). **Refresh rate switch** — `PORT_REFRESH_SWITCH` (default off, `1` = on, experimental: every switch blanks the screen and can glitch audio; launcher *Display › Frame rate › Refresh rate switch*): in exclusive fullscreen (`RSX_DISPLAY=exclusive`) the display is switched to the nearest mode at the same resolution whose refresh is a multiple of the frame rate (`[fps] display switched 144->120 Hz`), and back to the desktop rate when the game loses focus, is minimised, leaves fullscreen, closes or crashes. Borderless and windowed never switch (use G-Sync / FreeSync or an even frame rate there). With VRR the switch is harmless but unnecessary; `0` avoids the mode-change flash.
- Popup windows over the game (performance overlay, extra crosshair, toasts) make Windows compose the screen, which can turn G-Sync off while they are visible.

**Hitch logger** — `PORT_HITCH_LOG` (default on, `0` = off; `PORT_HITCH_MS=<ms>`, default 25): every game frame over 25 ms (or over 2x the frame-time target) prints one `[hitch]` line with where that frame went (game, limiter, sync waits, pad, RSX flip interval, present / acquire / GPU wait / DLSS, pipeline compiles, texture uploads, audio wait, and `present-iv` = the longest present-to-present interval). Every 5 s: `[hitch] 5 s:` (CPU frame time p50 / p99 / max) and `[hitch] present 5 s:` (present-to-present interval p50 / p99 / max, `uneven` = intervals more than 4 ms off the median, and how many frames a fixed-refresh display would hold for 1 / 2 / 3 / 4+ refreshes; a mix of 2 and 3 = judder unless VRR is active). Since this build the `[hitch]` line also splits `guestwait` (the game thread's own guest waits) into spin / sleep / lwmutex / lwcond and names the guest call site that waited longest (`top 0x... X ms`), shows the game thread's CPU time (`game X (cpu Y)`: a gap = the thread was preempted), and breaks down the RSX thread: `fifo-idle` (waiting for commands), `label ms/n` (NV406E semaphore waits), `zcull ms/n` (occlusion report reads waiting for the GPU), `hard-sync ms/n`, `submits n` and `rsx-limit` (RPCS3 flip limiter sleep). Hot-thread log lines (`[pipe]`, `[fps]`, `[hitch]`) are queued to a writer thread (`PORT_LOG_SYNC=1` writes them at once).

**Frame pacing** — one pacer: when the frame rate is not 30, the game limiter (`R2_FPS`) paces the frame loop and the RPCS3 flip limiter is off (it was a second 60 Hz clock: the RSX thread slept in the flip while the game waited on it, then the next frame ran at once). `GCM_FRAME_LIMIT=vblank` restores the old double pacing.

**Game clock smoothing** — `PORT_DT_SMOOTH` (default on, `0` = off): above 30 fps the game clock steps by the mean of the last 8 frame times instead of each raw frame time, so an uneven pair of frames (e.g. 1 ms + 32 ms) on an even display no longer makes objects jerk; the total game time is unchanged.
- **Frame rate:** 60 fps by default (the game was 30 fps on PS3). The game's own frame limiter is replaced by a precise PC-side one (high-resolution timer plus a short spin), the same idea as RPCS3's "Unlock FPS" patch, and RPCS3's vblank follows the chosen rate. The game clock's 1/75 s minimum step is lowered too, so 120/144 fps and Uncapped run at the right speed; cutscenes that use a fixed step per frame are kept at real-time speed. 30 fps uses the game's original limiter. *Just under refresh* (`R2_FPS=vrr`) caps a little below the display's refresh rate (e.g. 138 fps at 144 Hz) so a G-Sync / FreeSync display stays in its variable-refresh range. `R2_PACE=0` uses the older, less precise wait; `R2_PACE_SPIN_US=<µs>` (default 500) sets how long the limiter spins before a frame.
- **Display mode:** borderless fullscreen, exclusive fullscreen or windowed, with VSync off / on / adaptive / auto.
- **Anti-aliasing → NVIDIA DLSS** uses DLSS on GeForce RTX cards and falls back to FSR 3.1, then FSR 1, on other GPUs. The DLSS SDK is downloaded by Setup into `tools\dlss` (if that fails, the port still builds without it). `RSX_DLSS_PRESET=<letter>` forces a DLSS model preset.
- **Upscaling** (with FSR or DLSS on): Native (DLAA), Quality, Balanced or Performance. Menus and HUD stay at full resolution.
- **Intel XeSS (experimental, Compatible renderer):** XeSS 2 (`rpcs3/Emu/RSX/VK/VKR2XeSS.cpp`, Vulkan `xessVK*` API, Intel's `libxess.dll`; runs on any GPU through DP4a) is hooked into the RPCS3 renderer's upscaler. Pick **Intel XeSS** in the launcher's upscaling option, or set `RSX_FSR=4`. If XeSS can't start, or on the native renderer, FSR 3.1 is used instead. The preset follows the Upscaling ratio (Native → Native AA, Quality → Ultra Quality, Balanced → Quality / Balanced, Performance → Balanced / Performance). `RSX_XESS_QUALITY=aa|uqp|uq|q|b|p|up` asks for a preset, `RSX_XESS_JITTER_SIGN=-1` / `RSX_XESS_JITTER_SIGN_Y=-1` flip the jitter, and `RSX_XESS_LOG=1` logs XeSS's own messages. Setup downloads the XeSS SDK (`tools/xess/`) after you accept Intel's licence.
- Resistance 3 renders each frame across the console's buffer flip (a geometry pre-pass, then lighting and a material pass). The upscaler follows that, so DLSS / FSR run on the finished image, not the G-buffer. The upscaler locks onto the scene size once it is stable, so it isn't reset by menus or small passes, and briefly backs off instead of resetting when it can't find the scene for a frame or two.
- **CPU profile** (Performance page): Auto picks from the cores the game can use (logged as `[cpu]`). Low (≤4 cores) turns off the timing spin-waits, folds the Vulkan submit thread into the render thread and uses fewer shader-compiler threads. Mid (6 cores) shortens the spin-waits. High changes nothing. The port also spreads its main, RSX and FIFO threads over separate cores, keeps them on P-cores on hybrid Intel CPUs and lowers background threads. Advanced: `PORT_CPU_PROFILE=auto|low|mid|high|off` (settings you set yourself always win); `PORT_THREAD_PLACEMENT=0` turns off thread placement; `PS3_PRIO_BOOST=0/1` forces priority boosts off or on; `PORT_MAIN_CPU=<n>` pins the main thread to CPU n.
- **Low-end PCs:** use **Auto-detect**. On CPUs with fewer than 8 threads the port stops raising thread priorities and pinning threads to cores, and its busy-wait loops give the CPU to other threads. If a 4-core PC still stutters, use 30 fps.

### Graphics enhancements (Compatible renderer, all off by default)

- **Shadow resolution** (`RSX_SHADOW_SCALE=2|4`): the game's shadow maps (square, depth-only render targets from 256 to 2048 pixels) are rendered at 2x or 4x resolution and sampled at that resolution; the slope-scaled depth bias is scaled to match so no new shadow acne appears. The log says `[r2gx] shadow map target ...` for each one found. Costs GPU time and video memory (4x on a 2048 map is an 8192 map).
- **Effects resolution** (`RSX_FX_FULLRES=1|2`): render targets at half (`1`) or also quarter (`2`) the scene size (particles, smoke, glow / bloom, blur and depth-of-field buffers) are drawn at the scene's resolution; the game's own upsample and composite passes read them through the renderer's scaling, so they stay aligned.
- **Ambient occlusion** (`RSX_SSAO=1`, experimental): a screen-space AO pass computed from the scene depth when the scene pass ends, before the game's post-processing. Needs anti-aliasing off or FSR (not `R2_MSAA`); in this beta it is also off whenever NVIDIA DLSS is selected (`RSX_SSAO_DLSS=1` forces it on). `RSX_SSAO_STRENGTH=0..1` (0.6), `RSX_SSAO_RADIUS=0.1..8` (1.0); `RSX_SSAO_DEBUG=1` shows only the AO term.
- **Texture packs** (`RSX_TEXDUMP=1`, `RSX_TEXPACK=1`): dump writes each texture once to `texdump\` as `<key>_<w>x<h>_<format>_m<mips>.dds` (DXT1/3/5, with mips) or `.png` (A8R8G8B8), listed in `texdump\manifest.tsv`; the key is a content hash of all mip levels plus size, format and mip count (stable across runs, not a memory address); copy edited files into `mods\textures\` (sub-folders allowed; the leading 16-digit hash is what matters) and turn on the pack. Replacements must keep the original size (and DXT format for `.dds`). A `<folder>` value uses another folder. With both off the upload path costs one `bool` test.
- Not done for Resistance 3: a shadow-safe FOV mode and longer LOD / draw distance (the camera and LOD setters are not identified in the current recompilation; Resistance 2 has both), the ultrawide HUD fix (`RSX_HUD_FIX` is built in but Resistance 3 has no Hor+ aspect option yet, so it has no effect), and higher-quality motion blur / depth of field beyond what *Effects resolution* gives (they can be turned off: *Post-effect toggles* above).

### Renderer switches (advanced, `settings.bat`)

All optional; unset = the default shown. Each is logged at start-up in the `[r2rsx] perf:` line.

| Variable | Effect |
|---|---|
| `R2_RSX_PROF=1` | Every 5 s, prints a block of `[r2rsx-prof]` lines to the log: CPU time of each RSX thread, the renderer's frame phases, draw and framebuffer-bind time, command-queue flushes (by caller), ZCULL report waits, memory faults that needed a GPU readback, and vertex uploads. It also prints the RSX thread's busy time without DLSS, the time spent outside draws (flushes, flips, ZCULL, blits, labels), deferred ZCULL labels, and program-cache hits (`MRU hits`, `pipeline memo hits`, `re-sent FP kept`, `re-sent VP kept`; `program slow-path loads` counts the full cache lookups that remain). Further lines split the framebuffer-bind locks (discard / orphan / colour / depth, worst and slow rebinds), the texture cache (mutex wait, invalidations, relocks) and Windows `VirtualProtect` calls, the flip (frame-limiter sleep vs present), the program phase (analysis, lookup, constants, and the rest of the program environment: DLSS/FSR draw hook, vertex/fragment/texture env, `bind_uniform`) and the texture cache's blocking GPU readbacks. For finding what limits the frame rate. |
| `R2_SHADER_THREADS=<n>` | Background shader-compiler threads (default: what the CPU profile leaves free). |
| `R2_SHADER_CACHE=0` | Don't keep compiled shaders on disk (`shadercache\rpcs3`). |
| `R2_MTRSX=0/1` | RPCS3's multithreaded RSX (guest→host copies on a helper thread). Default: on. |
| `R2_ASYNC_TEX=0/1` | Asynchronous texture streaming (off by default: it only moves the GPU copy to another queue, the CPU work stays on the render thread, and it stops texture memory reuse; the `r8 per frame` line shows how few textures are uploaded in play). |
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
| `R2_XFORM_MEMO=0` | Round 8, default on: transform constants unchanged since the last upload for the same vertex program (every constant write is tracked, and the old copy must still be reserved in the upload heap) are not uploaded again. Byte-identical output; `0` uploads them every draw as RPCS3 does. |
| `R2_IDX_DEDUP=0` | Round 8, default on: an index range identical (compared byte by byte) to one already converted this frame, with the same type and restart settings, reuses the converted copy. `0` converts every draw's indices as RPCS3 does. |
| `R2_FIFO_MERGE=1` / `<n>` | Round 8, default off: RPCS3 merges back-to-back draws once a frame has 2,000 draws; `1` lowers that to 400, `<n>` sets it. |
| `R2_SHADER_PRELOAD=0` | The start-up preload of every pipeline in the disk cache logs its progress (`[r2rsx] shader preload: ...`); `0` keeps the preload without the log lines. Pipelines first linked during play are logged (`[r2rsx] shader cache miss #n during play`) and preloaded from the next start. |
| `R2_VK_PIPE_CACHE=0` | Default on: the driver's pipeline cache is saved after the preload and at exit (`shaders_cache/vk_pipeline_cache.bin`, ignored unless it was written by the same GPU and driver build), so the preload reloads instead of recompiling. `0` = empty driver cache every run (RPCS3 behaviour). |
| `R2_VK_GPL=0` | Default on when the GPU supports `VK_EXT_graphics_pipeline_library` with fast linking (`[r2rsx] pipeline libraries (R2_VK_GPL): on`): a pipeline first needed during play is linked at once from separately cached parts and the optimised pipeline is compiled in the background and swapped in. Same shaders and state. `0` = the normal path. |
| `R2_PRELOAD_ASYNC=0` | Default on with the asynchronous shader mode: every cached shader is compiled before the first frame, but only the first `R2_PRELOAD_BOOT` (384) recorded pipelines are linked; `R2_PRELOAD_THREADS` (default CPU threads / 4, 1 to 4) background threads link the rest during play. `0` = everything before the first frame. |
| `R2_FOSSILIZE=record` | Records every pipeline (with its shaders, layouts and render pass) to `shaders_cache/r3_pipelines.foz` (Valve Fossilize, sessions append). Default `auto`: that file, if present, is replayed on low-priority threads after the start-up preload, so a new GPU or driver fills its pipeline cache in the background (`[r2foz] replay done: ...`). `0` = off, `R2_FOSSILIZE_DB=<path>` = another file. |
| `R2_PIPE_LOG=0` | Default on: a `[pipe]` line every 5 s with the render thread's waits on shader/pipeline compilation (ms, >2 ms histogram) and fast-link / background counts. |
| `R2_RSX_CENSUS=1` | With `R2_RSX_PROF=1`: adds `r9 census` lines (most frequent GPU commands with their same-value share, render-target, depth and texture formats, MRT counts, conditional rendering), used to choose game-specific fast paths. |
| `R2_PRESENT=immediate` / `mailbox` / `fifo` | Vulkan present mode (overrides `RSX_VSYNC`; launcher *Presentation mode*). `immediate` (or `vrr` / `tearing`) never waits: best with G-Sync / FreeSync and `R2_FPS=vrr`. `mailbox` is not followed by G-Sync / FreeSync (see *Frame pacing* below). |
| `R2_ZCULL_ACCURATE=0/1` | Precise Z-pass counts. |
| `R2_MSAA=auto` | MSAA in the game's own render targets (only with FSR/DLSS off). |
| `R2_HIGHP_Z=1` | Force a high-precision depth buffer. |
| `R2_LOD_BIAS=<x>` | Texture LOD bias (negative = sharper). |
| `R2_GPU_TEX_SCALING=1` | Use the GPU for texture scaling. |
| `RSX_VBLANK_US=<µs>` | Override the vblank period (normally derived from the frame rate). |

## Shader precompile and rebuilding the shader cache

Shader caches are never shipped with the port: they are built on your PC, from the shaders your game uses, the first time each one is seen. Launcher, Performance page, **SHADER CACHE**:

- **Compile shaders before playing** (default On, `R2_PRELOAD_BOOT=-1`): at start-up the Compatible renderer builds every pipeline in its disk cache before the game gets its first frame (the game's RSX context waits for it), so nothing seen before compiles during play. Progress: a centred **Compiling shaders N / M** panel with a progress bar, drawn into the game image (`R2_PRELOAD_OSD=0` uses a separate window, not shown over exclusive fullscreen) and log lines `[r2rsx] shader preload: compiling every cached pipeline before the game starts on N threads`, `compiling n/N pipelines`, then `done, every cached pipeline is ready`. A few seconds with a warm driver cache, a minute or two after a driver update or a rebuild. Off (`R2_PRELOAD_BOOT` unset, 384 by default): only the first 384 recorded pipelines are built before the first frame and the rest in the background while you play (can hitch briefly).
- **Shader compile threads** (`R2_PRELOAD_BOOT_THREADS`, default Auto = every logical core but one, 1 to 64): the threads that build the pipelines before the game starts.
- **Rebuild shader cache** (button, asks first; refused while `Resistance3.exe` is running) deletes the compiled, driver-specific part and keeps the list of shaders and pipelines to build, so the next start with Compile shaders before playing on rebuilds all of them. Deleted, from `shadercache\` and from its backup in `%LOCALAPPDATA%\Resistance3Port\shadercache` (`run.bat` restores missing files from there):
  - `rpcs3\shaders_cache\vk_pipeline_cache.bin` (and `.bin.tmp`): the Vulkan driver's pipeline cache (driver blobs)
  - `rpcs3\shaders_cache\r2_gpu_stamp.txt`: the GPU and driver that cache was built for
  - `rpcs3\shaders_cache\shaderscan.stamp`: makes the launcher run shaderscan again on the next Play (see below)
  - `rpcs3\shaders_cache\r2_pipelines.foz`, `r3_pipelines.foz`: Fossilize databases (`R2_FOSSILIZE`)
  - `vk_pipeline_cache.bin`: the native renderer's Vulkan pipeline cache

  Kept: `rpcs3\shaders_cache\pipelines\vulkan\...\*.bin` and `raw\*.vp` / `*.fp` (RPCS3's pipeline list and shader programs: what gets rebuilt) and the native renderer's `vk_pipeline_variants.bin`, `rsx_engine_programs.bin` and `pipelines_*.db` lists.
- **GPU or driver change:** `r2_gpu_stamp.txt` records vendor, device, driver version, pipeline cache UUID and GPU name when the driver cache is saved. When they differ at start-up the log says `[r2rsx] vk pipeline cache: GPU or driver changed since the cache was built (was: ..., now: ...)`, the old `vk_pipeline_cache.bin` is deleted and every pipeline is rebuilt from the kept list (before play with the option on).
- **Offline shader scan:** [shaderscan](https://github.com/miroslavdanutalexandru-alt/shaderscan) (GPL-2.0, its own repository) is downloaded by Setup / `build.bat` into `tools\shaderscan.exe`: `tools\get_shaderscan.ps1` fetches the release pinned in `tools\shaderscan.version` and checks its SHA-256 (offline: skipped; set `SHADERSCAN_EXE=<path>` to use your own build). It is not part of the port's source or Setup, and the launcher works without it. When `shaderscan.exe` is next to `Launcher.exe` (or in `tools\`), Play first runs `shaderscan --check --out shadercache\rpcs3\shaders_cache`; if that says the cache isn't seeded (first launch, a new shaderscan version, or after Rebuild shader cache), it runs `shaderscan --game <folder> --out ... --progress`, which extracts every RSX shader from your game files into the cache so Compile shaders before playing covers the whole game without a playthrough. The launcher shows a progress panel (Esc or **Skip for now** stops it; the game starts either way) and logs the tool's output to `logs\shaderscan.log`. It runs at most once per launcher session.
- **Object motion vectors** (`RSX_DLSS_OBJMV`, default Off, Compatible renderer): DLSS/FSR get per-object motion for moving characters and objects (sharper, less ghosting on them) at some CPU cost; Off = camera motion only.

## RPCS3 patches

The community RPCS3 patches for Resistance 3 (RPCS3's `patch.yml`) were checked against this game's program file: disc **BCUS98176 / BCES01118 1.00**, PPU hashes `502feb2351d2b637a2704d5a50cb44bc1b781532` (US) and `2701cf372814d59f26e771dc9394323d4f799682` (EU), which share every address. The PowerPC code is recompiled, so patches to instructions become hooks (`tools/hooks/r3.json`), and data patches are written into game memory at boot (`project/r3_patches.cpp`), only if the original value is there; another version is never patched at the wrong place (a `[patch] ... skipped` line is logged instead).

| Patch (author) | What it does | Guest address | How it's ported | Setting | Default |
|---|---|---|---|---|---|
| Unlock FPS (Whatcookie, boma): limiter wait | Removes the frame limiter's wait | `0x00736C44` | Hook in `func_00736BAC` (`r2_frame_period`, `project/stubs.cpp`): frames are paced on the PC side | `R2_FPS` | 60 |
| Unlock FPS: frame-time floor | 0.001 s minimum frame time → 0.0001 | `0x00F02AD4` (float) | Data patch at boot (and again by the limiter) | Automatic when `R2_FPS` ≠ 30; `R2_PATCH_FPS_FLOORS=1` also at 30 | follows `R2_FPS` |
| Unlock FPS: game-clock clamp | The game clock's 1/75 s lower clamp | `0x007EEAC8` | Hooks in `func_007EEA44` (`r3_dt_floor` / `r3_dt_ceil`): the floor follows the frame rate, and fixed-step cinematics keep real-time speed | follows `R2_FPS` | on above 30 fps |
| Extended FOV (illusion) | Wider field of view (the constant in the projection; lower = wider). On Resistance 2 the same kind of patch misaligns shadows slightly, so it is experimental | `0x00F0608C` (float 1.0 → 0.7) | Data patch at boot | Launcher *Field of view*, or `R2_PATCH_FOV=1` with `R2_PATCH_FOV_VALUE=0.3..1.0`; `R2_PATCH_FOV=0` forces it off | off |

Not ported: patches for other versions (1.05 and later, PSN) and cheat patches. No community patch for this game exists for resolution (the port's render-resolution setting does that), motion blur, depth of field or film grain (the port's own *Post-effect toggles* turn those off).

## Performance work

Performance fixes have been applied as far as possible without changing how the game looks:

- **SPU (the PS3's job processors):** faster lock-line reservation and acknowledgement, cheaper waits on SPU channels and direct block-to-block jumps in the translated SPU code (`SPU_FAST_LLAR`, `SPU_FAST_ACK`, `SPU_FAST_WAIT`, `SPU_FAST_HOP`: all on by default, `=0` turns one off for testing; `SPU_FAST_LLPOLL=1` is an opt-in back-off for lock-line polling). `PORT_SPU_SPINWAIT=old|backoff|wait` sets how a raw SPU spins in a blocking channel wait before it sleeps: `old` (default) is the original tight spin, `backoff` uses growing pause bursts and then yields the core after 20 µs, `wait` also sleeps after `PORT_SPU_SPINWAIT_US` (default 20); `backoff` and `wait` are experimental (no measured gain); `[spuwait]` lines every 10 s show waits, waits finished while spinning, sleeps, yields and wake-ups.
- **Spin-waits:** the game's main-thread loops that spin while waiting for SPU jobs execute a CPU pause hint each lap instead of hammering the core (`R2_SPIN_PAUSE=0` turns it off), and give the core away on CPUs with few threads.
- **Memory faults:** the handler that commits guest memory on first touch looks up a bitmap instead of asking Windows, which makes the common case much cheaper.
- **Measured (in-game bench, Haven, RTX 5070, 60 fps target, DLSS 1080p → 4K):** 44 fps at first, limited by the RSX thread (ZCULL report waits cost 8.5 ms per frame); with relaxed ZCULL and deferred labels 55 fps, ZCULL down to 0.24 ms per frame. The remaining costs were framebuffer-bind locking, shader-program lookups and FIFO command processing; the program lookups were cut since (re-sent programs kept, previous pipeline reused), and Resistance 2's renderer rounds 4 and 5 (framebuffer-bind memory protection, texture-cache readbacks) are included. Latest bench: **59.5 fps** in Haven (1,145 draws per frame) at about 10.5 µs of RSX-thread time per draw (from 19.7 at first), stable over a 12-minute session; the DLSS per-draw hook went from 2.9 to 0.2 µs. Lower per-draw cost helps low-end CPUs the most.
- **Network thread:** the game's HTTP thread polled `cellHttpClientPollConnections`, which wasn't implemented and returned at once, so it used a whole CPU core; it now waits for a connection like the real function, and with no connection returns the library's "no connection" error so the thread sleeps between polls.
- **Renderer:** thread counts sized to the CPU, idle back-off in the RSX threads, frame pacing mirrored into RPCS3's vblank, skipped redundant render-target write-backs, relaxed ZCULL by default (no GPU stalls for occlusion results; strict mode batches reads, sleeps on the GPU fence and defers labels), shader-program lookups that skip re-hashing (`R2_PROG_HINT`, `R2_PROG_MRU`), re-sent fragment and vertex programs kept instead of analysed and looked up again (`R2_FP_REBIND`, `R2_VP_REBIND`) and the previous draw's pipeline reused when only state changed (`R2_PIPE_MEMO`), fewer and cheaper `VirtualProtect` calls (`R2_MM_SKIPSAME`, `R2_MM_INCR`), no blocking GPU readback of render targets that a new target fully overwrites (`R2_FBO_COMMIT_FLUSH`), deferred ZCULL labels carried over a flip in strict mode (`R2_ZCULL_FLIP_LAG`), cheaper DLSS camera-matrix matching, and fewer DLSS resets (see the table above). Round 8 (after Resistance 1's own renderer): unchanged transform constants and repeated index ranges are not uploaded again (`R2_XFORM_MEMO`, `R2_IDX_DEDUP`), the shader preload is logged (`R2_SHADER_PRELOAD`), and `R2_RSX_PROF=1` prints where the end of each draw goes (`r8` lines: Vulkan recording, uploads, pipeline misses, FIFO redundancy); `R2_RSX_CENSUS=1` adds a usage census.

### CPU switches and builds

In heavy scenes the frame rate depends on the game's main PPU thread and **rawSPU0**, the SPU job kernel (the main thread waits on its jobs in `func_0082C190`). Env switches (unset = default):

- `PORT_SPU0_CPU=<n>`: with ≥ 12 logical CPUs, rawSPU0 gets logical CPU 6 and an empty SMT sibling, like the main thread (`PORT_MAIN_CPU`, default 2) and the RSX thread (`PORT_FIFO_CPU`, default 4). In the Compatible (RPCS3) renderer that RSX core goes to the thread named `rsx::thread`. `-1` turns this off, and `PORT_MAIN_CORE=0` turns off all pinning. Each pin is logged as an `[affinity] ... pinned` line.
- `PORT_PCORE_HOT=0`: on Intel hybrid CPUs (P + E cores), main, rawSPU0, `rsx::thread` and the FIFO / RSX engine threads get P-core thread CPU sets (`SetThreadSelectedCpuSets`), even with fewer than 4 P-cores. AMD and other uniform CPUs are not changed. `=0` turns this off.
- `PORT_KEEPER_MS=<ms>`: the thread-placement keepers rescan every 1–2 s while new threads appear, then every 5000 ms (default).
- `PORT_PIN_TOPO=0`: the default pin CPUs above (main, RSX, rawSPU0) are now taken from the detected CPU topology: the first logical CPU of physical cores 1, 2 and 3 (P-cores first on hybrid CPUs). On a 6-core/12-thread CPU that is still 2, 4 and 6; on CPUs without SMT it is 1, 2 and 3 instead of two hot threads sharing a core. `=0` uses the old fixed 2 / 4 / 6. The `[affinity] main thread ...` line shows the CPUs and the switch values.
- `PORT_REST_SMT=<0|1|2>`: where all other threads (SPURS lanes, rawSPU1, RSX workers, driver, audio) may run. `0` (default): only cores that have no pinned hot thread, so every hot thread keeps its SMT sibling idle (on 6C/12T that is mask `0xF03`). `1`: also the siblings of the rawSPU0 and RSX cores. `2`: also the main thread's sibling. Try `1` if the `[thrcpu]` lines show the SPURS lanes starved; it costs the hot threads some SMT throughput.
- `PS3_USLEEP_NOTIFY_PUT=0`: a short guest `usleep` poll (under 1 ms) already ends early when an SPU atomic writes guest memory (`PS3_USLEEP_NOTIFY`). It now also ends early on a plain SPU DMA PUT, so the main thread's SPU-job wait re-checks as soon as a job writes its result instead of finishing its sleep. It costs nothing while no thread is waiting. `=0` turns this off. Logged once as `[usleep] PS3_USLEEP_NOTIFY=1 PS3_USLEEP_NOTIFY_PUT=1 ...`.
- `SPU_INLINE_CH=0`: by default the lifted SPU code writes the MFC DMA parameter channels and reads the status channels inline instead of calling the channel runtime. `=0` restores the runtime calls. The build option is `-DPORT_SPU_INLINE_CH=OFF`.
- `R3_OCC_SKIP=0` (default): occlusion-query results (same as R2's `R2_OCC_SKIP`). `=1`: when last frame's GPU occlusion results are not ready yet, the game keeps the previous visibility for that set once instead of waiting, so the CPU and the renderer work in parallel. Off by default because nearby objects visibly pop in and out with it. `=0` is the original wait; `R3_OCC_WAIT_US=<n>` caps the wait at n µs. Logged every 10 s as `[occ]`. Not measured in R3 yet; in R2 the same change took a heavy fight from 35.7 to 46.1 fps.
- `R2_FIFO_INLINE=1`, `R2_DMA_INLINE_MAX=<bytes>`: renderer-thread changes shared with R2, off by default since a 2026-11-11 R2 freeze report (opt-in): an inline fast path for reading FIFO command arguments, and RSX DMA copies up to the given size run directly on the RSX thread instead of waking the Offloader thread (default `0` = the upstream 3.5 KB limit).
- `SPU_PROF=1` (also on with `R2_RSX_PROF=1`): prints a `[spu-prof]` line per SPU every 5 s with channel wait % (spin / sleep), GETLLAR / PUTLLC rates and failures, DMA MB/s, runtime channel calls and indirect branches.

Not done, and why:
- **Large pages for guest memory:** guest memory is committed lazily on fault, and the RPCS3 texture cache write-protects single 4 KB pages. Neither works on 2 MB pages. Large pages also need the *Lock pages in memory* right.
- **Running SPU jobs in parallel:** jobs in a list run in order and share data.

**Builds**: each build goes into its own folder, and `build.bat` is unchanged. `run.bat` runs `PORT_GAME_EXE` when it is set, for example `set PORT_GAME_EXE=build_cpu\Resistance3.exe`.

| Command | Output | Notes |
|---|---|---|
| `build_cpu.bat` | `build_cpu\Resistance3.exe` | AVX2 for the runtime and hand-written code, plus ThinLTO (`-flto=thin`; the toolchain is clang-cl, so `/GL` and `/LTCG` don't apply). The lifted PPU objects are reused from `build\`. |
| `build_cpu.bat baseline` | `build_baseline\Resistance3.exe` | No AVX. The launcher's CPU check selects it on CPUs without AVX2 (`PORT_CPU_V3=0`). |
| `build_cpu.bat tracy` | `build_tracy\Resistance3.exe` | [Tracy](https://github.com/wolfpld/tracy) zones from `PORT_TRACY_DIR` (default `D:\Games\refs\tracy`, v0.15). Connect the Tracy 0.15 viewer while the game runs; nothing is recorded until it connects. Zones: `syscall` / `hle` (each guest syscall or HLE import), `spu job` / `spu wait` (each SPU job and blocking channel wait), `spin` (each `r3_spin_relax` wait, merged from its laps), and `RSX flip` frame marks. Tracy's crash handler is off, because the port's exception handler commits guest memory. |

**PGO**:
1. Run `build_cpu.bat pgo-gen`.
2. Run `tools\pgo_train.bat` and play for 10–20 minutes, covering heavy fights, a level load, a checkpoint reload, cutscenes and menus.
3. Run `tools\pgo_merge.bat`. It writes `pgo\r3.profdata`.
4. Run `build_cpu.bat pgo-use`. It builds `build_pgo\Resistance3.exe`.

The CMake options behind these are `PORT_ARCH`, `PORT_LTO`, `PORT_LTO_PPU`, `PORT_PGO`, `PORT_PGO_PROFILE`, `PORT_PGO_PPU`, `PORT_PPU_PREBUILT_OBJ_DIR`, `PORT_SPU_INLINE_CH`, `PORT_TRACY` and `PORT_TRACY_DIR`.

## Audio

When the frame rate drops, the game's own audio mixing (on the recompiled PPU/SPU threads) can fall behind real time. The port therefore keeps a buffer of mixed audio between the game and the sound card, as RPCS3 does with its "audio buffering" and "time stretching" options:

- **Buffer:** mixed audio goes into a ring buffer with a target of `PORT_AUDIO_BUFFER_MS` (100 ms by default). The sound card is kept topped up from it, about 30 ms at a time.
- **Time stretching:** when the buffer drops below `PORT_AUDIO_STRETCH_THRESHOLD` % of the target, playback slows down through [SoundTouch](https://www.surina.net/soundtouch/) without changing the pitch, so a slow stretch of frames sounds slightly slower instead of gapping. Tempo returns to 1.0 as the buffer refills.
- **Underrun:** if the buffer still runs dry (a hitch longer than the buffer), the last few milliseconds fade out to silence and fade back in when audio returns. No repeated blocks, no clicks.
- **Mixer thread:** runs at TIME_CRITICAL priority with Windows MMCSS "Pro Audio" scheduling. It also stays off the cores reserved for the main game thread, the RSX thread and rawSPU0 (`PORT_MAIN_CPU`, `PORT_FIFO_CPU`, `PORT_SPU0_CPU`).
- **Late game audio:** when the game's next block of audio has not been written in time, the mixer waits up to `PORT_AUDIO_LATE_WAIT_MS` (22 ms) for it. The game's audio clock slips slightly and the buffer absorbs the slip.

| Variable | Default | Effect |
|---|---|---|
| `PORT_AUDIO_STRETCH` | `1` | SoundTouch buffer and time stretching. `0` = the previous audio output path, unchanged (the older built-in WSOLA stretcher; `PORT_AUDIO_WSOLA=0` turns that off too). |
| `PORT_AUDIO_BUFFER_MS` | `100` (`150` with fewer than 12 logical CPUs) | Target buffer, 20–500 ms. More = fewer underruns, more latency. |
| `PORT_AUDIO_STRETCH_THRESHOLD` | `75` | Stretch below this % of the target buffer. |
| `PORT_AUDIO_STRETCH_MIN` | `0.5` | Slowest tempo while stretching (0.1–1). |
| `PORT_AUDIO_FADE_MS` | `5` | Fade-out / fade-in length on an underrun. |
| `PORT_AUDIO_DEVICE_MS` | `30` | Audio handed to the sound card at once. |
| `PORT_AUDIO_CPU` | – | Pin the mixer thread to one logical CPU; `-1` leaves its affinity alone. |
| `PORT_AUDIO_MMCSS` | `1` | `0` = no MMCSS "Pro Audio" scheduling for the mixer thread. |
| `PORT_AUDIO_LATE_WAIT_MS` | `22` | Longest wait for a late block of game audio (0–40, `0` = off). |
| `PORT_AUDIO_LOG` | `5000` | Period of the `[audio]` log line in ms. `1` = every second (also with `R2_RSX_PROF=1`), `0` = off. |

**Reading the log.** At start-up, `logs\run.log` shows `[audio] output: ... SoundTouch stretch (...)` with the MMCSS state and the mixer thread's affinity. Every 5 seconds there is a line like:

```
[audio] 5.0 s: buffer min 82 avg 101 ms (target 100) | tempo min 1.00 avg 1.000 (stretched 0%) | underruns 0 (silence 0 ms, device dry 0) | ...
```

- `buffer min` well above 0 and `avg` near the target: healthy.
- `tempo min`/`avg` below 1.00 and `stretched` above 0 % during a frame-rate drop: the stretcher is covering it. Tempo should be back to 1.000 within a second or two.
- `underruns` and `device dry` should stay at 0. A non-zero count means a hitch longer than the buffer; try a larger `PORT_AUDIO_BUFFER_MS`.
- `ring overflow` at the end: the game produced audio faster than it was played, so the oldest audio was dropped (harmless after a long pause).

The offline test for this stage is `ps3recomp\libs\audio\tests\test_audio_out.cpp` (part of `scratch\enginetest\run_all.ps1`).
## Steam Deck / Linux

The port is a Windows program. On Linux and the Steam Deck it runs through Proton (Wine). This has not been tested on a real Deck yet, so treat it as experimental.

1. Install and build the port on a Windows PC (Setup needs Windows: it uses winget and Visual Studio). No file picker is needed: the installer and Setup find the game on their own, or take a typed path. Then copy the whole port folder to the Deck, for example to `/home/deck/Games/Resistance3Port` (or a microSD card).
2. In Desktop Mode, open Steam → **Games → Add a Non-Steam Game to My Library…**, click **Browse**, and pick `Launcher.exe` in the port folder.
3. Right-click the new entry → **Properties → Compatibility** → tick **Force the use of a specific Steam Play compatibility tool** and choose **Proton Experimental** (or Proton 9.0 or newer).
4. Launch options are not needed. `SteamDeck=1 %command%` forces the Deck preset on other Linux PCs.

What the port does under Proton:

- The launcher detects Proton and the Deck (its hardware line says `Steam Deck` or `Wine / Proton`). On the Deck, **Auto-detect** picks a Deck preset: PS3 resolution, FSR 1 scaling to a 1280x800 window, 30 fps.
- The frame rate list gets a **40** option. Set the Deck's screen to 40 Hz (Quick Access → Performance) to match it. The window list gets **1280 x 800** (the Deck's screen).
- DLSS is not used under Proton; choose FSR instead. The Vulkan renderer runs on the Deck's own Vulkan driver.
- In the launcher, the controller's **A** or **Start** button starts the game. In game, Steam Input presents the Deck's controls as an Xbox controller.
- The update check works the same way and can be turned off in the launcher.

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

- **DLSS is experimental.** Some textures can still shuffle or shimmer, and dark patches can appear on some surfaces. If they bother you, use AMD FSR 3.1.
- **Ambient occlusion is off whenever NVIDIA DLSS is selected** in this beta (also when DLSS falls back to FSR 3.1); it still works with FSR or no upscaler. `RSX_SSAO_DLSS=1` forces it on.
- **Aiming down sights and some heavy fights can slow down** (the game is CPU-bound there). Please send `logs\run.log` if you see it.
- **Frame Generation is experimental and barely tested** (DLSS on RTX 40/50, FSR 3 elsewhere; off by default). Expect ghosting and HUD or menu artifacts; reports are welcome.
- **Changing Frame Generation in-game** can double the sound and then leave constant static. Change it in the launcher, or restart the game after changing it.
- **Occasional microstutter**, even with a complete shader cache. The `[hitch]` lines in `logs\run.log` help.
- No ultrawide support yet: 21:9 and 32:9 screens show the 16:9 picture with side bars.
- 60 fps and above change the game's timing; if something (physics, a scripted sequence) looks wrong, try 30 fps in the launcher. 120 fps and above are less tested than 60.
- Exclusive fullscreen can give black F12 screenshots; use borderless.
- Occasional freezes or crashes are possible, especially past the opening level.

## Roadmap

What's planned after this beta, and how it relates to the issues above: [docs/ROADMAP.md](docs/ROADMAP.md).

## Credits and licences

- The Resistance 2 PC port, the base of this one (same runtime, launcher, DLSS bridge and fixes).
- [Killzone PC ports](https://github.com/drakolordx7/KillzoneRecomp) by drakolordx7: ideas for the launcher and quality-of-life features, with the maintainer's permission.
- [Resistance: Fall of Man PC port](https://github.com/Psyykosis/Resistance-Fall-of-Man-Recomp): this port builds on its runtime, tools and research (SPURS/SPU job handling, frame pacing, input, audio and video fixes).
- ps3recomp, by Ned Heller / sp00nznet: MIT.
- SoundTouch 2.4.1 by Olli Parviainen (audio time stretching), in `ps3recomp/third_party/soundtouch/`: LGPL-2.1 (`COPYING.TXT` there).
- at3_standalone, FFmpeg's ATRAC3 / ATRAC3plus decoders made standalone by the PPSSPP project (PPSSPP commit `ebc151f`), in `ps3recomp/third_party/at3_standalone/`: LGPL-2.1-or-later (`COPYING.LGPLv2.1` there). Like SoundTouch it is linked statically; this repository is the source you need to relink it with a modified version.
- Valve Fossilize (Vulkan pipeline recording and replay, `R2_FOSSILIZE`), with RapidJSON, miniz and dirent for Windows, in `ps3recomp/third_party/fossilize/`: MIT (`LICENSE` there, RapidJSON and dirent licences in their folders).
- RPCS3 (RSX core and Vulkan renderer), downloaded during setup and patched: GPL-2.0. Because the built game links RPCS3 code, **the built program is distributed under the GPL-2.0**; the patch in this repository is GPL-2.0 too.
- AMD FidelityFX SDK (FSR 3.1): MIT, © Advanced Micro Devices.
- NVIDIA DLSS SDK v310.9.16, downloaded during setup into `tools/dlss/` only after you accept NVIDIA's licence: NVIDIA RTX SDK licence (`tools/dlss/LICENSE.txt`), not part of this repository. It is used only by the separate DLSS bridge DLL that build.bat builds on your PC. This port uses NVIDIA DLSS; NVIDIA, GeForce RTX and NVIDIA DLSS are trademarks of NVIDIA Corporation.
- Intel XeSS SDK 2.1.1 (runtime `libxess.dll` and headers), downloaded during setup into `tools/xess/` only after you accept Intel's licence, and not part of this repository: Intel Simplified Software License (`tools/xess/LICENSE.txt`, third-party notices in `tools/xess/third-party-programs.txt`). `build.bat` copies the unmodified `libxess.dll` next to the game; reverse engineering it is not allowed. Needs the Visual C++ 2015-2022 runtime (14.40 or newer).
- NVIDIA Streamline SDK 2.14.16 (headers, `sl.*.dll`, `nvngx_dlssg.dll`, `NvLowLatencyVk.dll`), downloaded during setup into `tools/streamline/` only after you accept NVIDIA's licence, and not part of this repository. Headers: MIT (`tools/streamline/license.txt`); the DLLs are NVIDIA RTX SDK / Reflex redistributables (`tools/streamline/nvngx_dlss.license.txt`, `reflex.license.txt`, `3rd-party-licenses.md`). `build.bat` copies them unmodified next to the game.
- FFmpeg 7.1 (the BtbN `win64-lgpl-shared-7.1` build: unmodified `avcodec-61`, `avutil-59` and `swresample-5` DLLs), downloaded during setup into `tools/ffmpeg_bin/` for the game's videos and audio (the H.264, MPEG-2 video and AC-3 decoders): LGPL-3.0-or-later (`tools/ffmpeg_bin/LICENSE.txt`; `tools/ffmpeg_bin/SOURCE.txt` says where it came from and where its source is). It is loaded at run time and is not part of this repository. The FFmpeg headers in `ffmpeg/include/` are LGPL-2.1-or-later (see each file).
- RPCS3's `key_vault.cpp`, downloaded during setup and used only to decrypt your own copy.
- The port's own code: MIT, see `LICENSE`.

## Support

This port is free and always will be: no paid builds, no early access and no perks. It is a fan project made in my spare time.
If you enjoy it and feel like buying me a coffee, it's genuinely appreciated and keeps the late-night debugging going ☕
[buymeacoffee.com/damiro98](https://buymeacoffee.com/damiro98)

A tip supports my own time and tools, such as [shaderscan](https://github.com/miroslavdanutalexandru-alt/shaderscan). It is not a payment for the game or this port, and nothing is unlocked by it. You still need your own copy of the game.
The projects in the credits above, ps3recomp and RPCS3 especially, deserve the support just as much.

*Resistance 3* is © Sony Interactive Entertainment, developed by Insomniac Games. This is an unofficial fan project, not affiliated with or endorsed by Sony or Insomniac. Please own the game you play.
