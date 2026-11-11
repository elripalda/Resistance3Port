# Resistance 3 PC port: community beta release notes

**Version:** 0.2.1-beta (GitHub pre-release). **Release date:** _to be filled in on release_.

A native PC port of *Resistance 3* (PS3) built by static recompilation: the game's PowerPC and SPU code is translated to C/C++ and compiled for Windows. Graphics go through RPCS3's Vulkan renderer, built into the game as a library.

> **No game files and no prebuilt game are included.** This release is the source code plus Setup. You need your own legally dumped copy of the game (see *Install*); Setup builds the game on your PC from that copy.

This is a **beta**. Only the intro and the opening Haven level have been played through so far, on one PC (Ryzen 5 5600X, GeForce RTX 5070, Windows 11). Expect bugs, and please report them (see *Reporting problems*).

## 0.2.1-beta (setup hotfix)

Setup fixes only; the game itself is unchanged from 0.2.0.

- **Progress bar during the build.** The bar no longer stops partway while the game compiles; it follows the build to the end, so Setup no longer looks stuck.
- **The installer now opens the Setup window** (with the progress bar and your game already filled in) instead of a console window. Click **Install** there.
- **Much smaller build logs.** The compiler was set to report every possible warning, which made `logs\build.log` grow past 150 MB and slowed the build. It now reports normal warnings only.
- **Low-RAM PCs:** if the build runs out of memory, Setup again detects it and retries with fewer parallel jobs (this check could not read the build log in 0.2.0).

If 0.2.0 is already built and runs, you don't need this update.

## What's in this beta

### Launcher
- `Launcher.exe` holds all settings and key bindings. **Auto-detect** checks your CPU, GPU and screen and picks recommended settings.
- Pages: Display, Graphics, Upscaling & Frame Gen, Performance, Controls, Audio, Interface & Notifications, Advanced, **Trophies**, **Saves & tools** (save backup and restore) and About (open the logs, saves and screenshots folders, report a problem). `Launcher.exe --page N` opens one (0–10).
- Includes a log button for bug reports, and an update check that only opens the Releases page and never downloads anything. Turn the update check off on the Advanced page or with `PORT_NO_UPDATE_CHECK=1`.

### Graphics
- **Upscaling and anti-aliasing:**
  - NVIDIA DLSS / DLAA on GeForce RTX cards. This is experimental, see *Known issues*.
  - AMD FSR 3.1 on any GPU.
  - Off.
  - If DLSS can't start, for example on a non-NVIDIA GPU, the port falls back to FSR 3.1.
- **Intel XeSS 2 (experimental):** pick *Intel XeSS* in the launcher (Compatible renderer; `RSX_FSR=4`). It runs on any GPU. If it can't start, or on the native renderer, the port uses FSR 3.1. Setup downloads the XeSS SDK after you accept Intel's licence.
- **Render resolution:** from 720p (PS3 native) up to 3840x2160 (`RSX_RES_SCALE`). Upscaling ratios are Native, 67, 58, 50 and 33 % (`RSX_UPSCALE`).
  - The default is the *1080p rendered → DLSS 4K* preset. On a 1080p screen the result is supersampled.
- **Frame rate targets:** 30, **60** (default), 120, 144, *Match display*, *Just under refresh* (for G-Sync / FreeSync) and Uncapped (`R2_FPS`).
  - The game clock follows real time, so 60 fps and above don't speed the game up.
  - An optional lower cap applies in menus and cutscenes (`PORT_MENU_FPS`).
- **VRR / G-Sync fix:** VSync *Auto* and *Adaptive* now use FIFO or FIFO-relaxed presentation instead of MAILBOX. G-Sync / FreeSync doesn't follow MAILBOX.
  - The *Presentation mode* option (`R2_PRESENT=immediate|mailbox|fifo`) overrides this.
  - The *Just under refresh* frame rate with *Immediate* gives tear-free VRR without VSync latency.
- **Refresh rate switch:** in exclusive fullscreen, the display switches to the refresh rate that best fits the frame rate cap. Experimental and off by default (`PORT_REFRESH_SWITCH=1`): every switch blanks the screen and can glitch audio.
- **Shader precompile:** with *Compile shaders before playing* (default on), every known pipeline is built before the first frame and a *Compiling shaders N / M* panel shows progress. Before Play, the launcher also runs the offline shader scan (shaderscan) with its own progress panel.
  - Shader caches are built on your PC and never shipped.
  - The cache is backed up to `%LOCALAPPDATA%\Resistance3Port\shadercache`.
- **Frame Generation** (experimental, off by default; Upscaling & Frame Gen page, `R3_DLSS_FG`, `R3_FG_BACKEND=auto|dlss|fsr3`): NVIDIA DLSS Frame Generation 2x on an RTX 40, up to 4x on an RTX 50 (needs NVIDIA DLSS); other GPUs get AMD FSR 3 2x.
- **Optional extras:**
  - HDR output (`R3_HDR`; experimental, off by default, needs Windows HDR)
  - NVIDIA Reflex (`R3_REFLEX`; experimental, off by default)
  - depth-of-field, film grain and motion-blur toggles (`R3_FX_*`; on by default, which is the game's own look)
  - colour-blind filters (`R3_COLORBLIND`)

### Controls
- **Mouse and keyboard:** raw mouse look (it doesn't emulate the stick) and rebindable keys (`controls.txt`, `mouselook.cfg`).
- **Controllers through SDL3:** Xbox, PlayStation (DualShock 4 / DualSense), Switch Pro and others (`PAD_BACKEND=sdl3`, the default; `xinput` is still available).
  - Optional gyro aiming (`PAD_GYRO`).
  - Light bar and Nintendo button layout options.
- **Button prompts follow your device** (`R2_PAD_GLYPHS=auto`): keyboard keys, or Xbox / PlayStation buttons. You can force one in the launcher.
- **Local split-screen co-op** for two players: two controllers, or one controller plus keyboard and mouse.

### PC features
- **PC menu:** F10, or hold View + Menu on a controller. It has Resume, PC Options and **Quit to Desktop** (with confirmation, and it waits for a save in progress to finish).
- **PC wording:** text that refers to the console is changed to PC terms. For example, "Press and hold the PS button…" becomes "Press F10 to quit the game". Only the text in memory changes. `R3_PC_TEXT=0` turns this off.
- **Screenshots** with F12 or PrtScn go to `screenshots\`. **F3** toggles the performance overlay.
- **Menus, overlay and notifications are drawn into the game image** (`PORT_OSD`), so they also show in exclusive fullscreen. The movie-skip hint can be turned off (*Show cutscene skip hint*, `PORT_SKIP_HINT=0`).
- Alt+Enter toggles windowed mode. You can choose the monitor, and pause or mute when the game is in the background.
- **Trophies** are recorded locally, with on-screen notifications. A **mods folder** loads replacement game files.
- **Crash and hang reports** are written to `logs\crash\`.

### Audio
- **Time stretching** (SoundTouch, `PORT_AUDIO_STRETCH=1`, default on): when the frame rate drops, audio slows slightly instead of crackling or gapping.

### Diagnostics
- **Hitch logger** (`PORT_HITCH_LOG=1`, default on; threshold `PORT_HITCH_MS`, default 25 ms): each slow frame writes one `[hitch]` line to `logs\run.log` showing where the time went. Please include it when you report stutter.

## What you download

- **`Resistance3Port-Setup-0.2.0-beta.exe`** from the GitHub release: an installer with the port's source, scripts and `Launcher.exe`. It installs the files and starts Setup.
- or the **source code** (the release's source archive or `git clone` at the tag), then `Setup.bat`.

Either way, the game itself is built on your PC; no built game program is published. Setup downloads the build tools, RPCS3's source, FFmpeg (an LGPL build), shaderscan and, if you accept their licences, the NVIDIA DLSS, Streamline (DLSS Frame Generation) and Intel XeSS SDKs.

## Requirements

- Windows 10 or 11, 64-bit.
- A CPU with **AVX2** (Intel 4th generation or newer, or any AMD Ryzen). The game is mostly CPU-bound, so a faster CPU helps the most.
- A **Vulkan 1.3** GPU with a current driver. DLSS needs a GeForce RTX card. FSR 3.1 works on AMD, NVIDIA and Intel. Only NVIDIA has been tested so far.
- The Microsoft Visual C++ 2015–2022 x64 runtime, version 14.40 or newer.
- Your own copy of **Resistance 3, US disc (BCUS98176), version 1.00**. Other regions, digital (PSN) copies and patched versions are not supported.
- About **50 GB** of free disk space: about 37 GB for the game files plus the build and the shader cache.
- For the source / installer setup: an internet connection and the free build tools. Setup installs the tools itself.

## Install

1. **Dump your own game.** Use a decrypted ISO, or a game folder containing `PS3_GAME` (the same kind of folder RPCS3 uses), from your own disc. Encrypted ISOs need decrypting first, for example with PS3Dec and the disc key. Don't use compressed archives, PKG files or update folders.
2. **Install the port.** Either:
   - run the installer, `Resistance3Port-Setup-<version>.exe`, which installs to `C:\Games\Resistance3Port` by default and then asks for your game; or
   - download the source code (the release's source archive, or `git clone` at the release tag) into a folder **without spaces** (for example `C:\Games\Resistance3Port`) and double-click `Setup.bat`.
3. **Setup** looks for your game and lists the copies it finds (common folders and RPCS3's game list). You can also enter a path, browse for it, or run `Setup.bat -Game "<ISO or folder>"`. It then:
   - checks the copy (title ID, version, size and the known file hash) and explains what is wrong if it can't be used;
   - copies or moves it to `gamefiles\PS3_GAME` inside the port folder;
   - decrypts the game's program, `EBOOT.BIN`, to `game\EBOOT.elf`, using the public application keys from RPCS3's source, and checks it is the supported version;
   - generates and builds the game-specific code from your copy. This takes 20–60 minutes the first time.
4. **Play** with `Launcher.exe`.

**How the port finds your game:**
- `run.bat`, which the launcher starts, runs the game with `game\EBOOT.elf` and points the game's virtual disc at `gamefiles` (`PS3_VFS_ROOT`).
- Saves go in `gamedata\dev_hdd0\home\00000001\savedata` (`PS3_GAMEDATA_ROOT` moves them).
- Settings and controls are also copied to `%USERPROFILE%\Saved Games\Resistance 3\config`. Put an empty `portable.txt` next to `Launcher.exe` to turn that off.
- Logs are in `logs\`.

**First launch:** before the game starts, the launcher prepares the shader cache from your game files (a progress panel; **Skip for now** or Esc skips it). The game then compiles its known shaders behind a **Compiling shaders N / M** panel, which can take a minute or two. Later starts are fast. Updating your GPU driver rebuilds the cache once.

## Known issues

- **DLSS is experimental.** Some textures can still shuffle or shimmer, and dark patches can appear on some surfaces. Fixes are in progress. If they bother you, use AMD FSR 3.1 in the launcher.
- **Ambient occlusion is disabled with DLSS** in this beta (also when DLSS falls back to FSR 3.1), because it caused doubled characters and dark patches. It still works with FSR or no upscaler.
- **Aiming down sights and some heavy fights can slow down.** The aim slowdown is much lower than before but not gone. Please send `logs\run.log` if you see it.
- **Frame Generation is experimental and barely tested** (DLSS on RTX 40/50, FSR 3 elsewhere; off by default). Expect ghosting and HUD or menu artifacts; reports are welcome.
- **Changing Frame Generation in-game** can double the sound and then leave constant static. Change it in the launcher, or restart the game after changing it.
- **Occasional microstutter**, even when the shader cache is complete. If you see it, please send the `[hitch]` lines from `logs\run.log`.
- **The first launch prepares the shader cache**: a shader scan panel, then a **Compiling shaders N / M** panel, so the first start takes noticeably longer (see *Install*).
- 120 fps and above are less tested than 60. If physics or a scripted sequence looks wrong, try 60 or 30 fps.
- Exclusive fullscreen can give black F12 screenshots, and the screen goes black briefly at start and when switching windows. Borderless avoids both.
- Occasional freezes or crashes are possible, especially past the opening level, which is untested.
- There is no ultrawide support yet. 21:9 and 32:9 screens show the 16:9 picture with side bars.
- Everything else: **TBD after testing**.
- What's planned next: [ROADMAP.md](ROADMAP.md).

## Reporting problems

Please read **[REPORTING.md](REPORTING.md)** before reporting. In short: say what you did and how to reproduce it, and attach the session's log from `logs\` (the launcher has a button for it) plus any crash or hang report in `logs\crash\`.

## Licences

The port is MIT-licensed (see `LICENSE`). Its RPCS3 patch is GPL-2.0, and the game you build links RPCS3, so that build is GPL-2.0; don't redistribute it with the DLSS files. See `THIRD_PARTY_NOTICES.md` for the full list. This port uses NVIDIA DLSS. *Resistance 3* is a trademark of its owners. This project is not affiliated with or endorsed by them.
