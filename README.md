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
3. Play with **`Launcher.exe`**. It has the settings (display mode, resolution, frame rate, VSync, anti-aliasing, upscaling, texture filtering, sharpening) and the key bindings.

You only need to run Setup again after downloading a newer version of the port; otherwise just use the launcher.

**First launch:** the game shows its own one-time setup screens and an autosave notice. The first time you load into a level, expect some stutter while the graphics shaders are compiled; after that, shaders are cached.

## Controls

Keyboard and mouse are supported out of the box: WASD move, Space jump, C/Ctrl crouch, Shift sprint, direct mouse look, Mouse1 fire, Mouse2 aim, Mouse3/F secondary fire, R reload, V/Mouse4 melee, G/Mouse5 grenade, 1/2 grenade type, Q / wheel weapon select, E use, Esc pause, Tab select, F8 releases the mouse. On-screen prompts show key names when you're on keyboard. Controllers work too. All keyboard and mouse bindings can be changed in the launcher.

## Graphics options

- **Frame rate:** 60 fps by default (the game was 30 fps on PS3). The game's own frame limiter is replaced by a precise PC-side one, the same idea as RPCS3's "Unlock FPS" patch. 30 fps is still available in the launcher. Don't push past 60: the game's input handling breaks above ~75 fps. Cutscenes and videos keep their normal speed.
- **Display mode:** borderless fullscreen, exclusive fullscreen or windowed, with VSync off / on / adaptive.
- **Anti-aliasing → NVIDIA DLSS** uses DLSS on GeForce RTX cards and falls back to FSR 3.1 on other GPUs. The DLSS SDK is downloaded by Setup into `tools\dlss` (if that fails, the port still builds without it).
- **Upscaling** (with FSR or DLSS on): Native (DLAA), Quality, Balanced or Performance. The default is DLSS Performance at a 4K render resolution, which is then downscaled to your screen: sharper than native at about the cost of native. Menus and HUD stay at full resolution.
- Resistance 3 renders each frame across the console's buffer flip (a geometry pre-pass, then lighting and a material pass). The upscaler follows that, so DLSS / FSR run on the finished image, not the G-buffer.

## Status

This is a work in progress, tested on a single PC.

- Runs at about 52–60 fps in Haven on my PC (CPU-bound; the GPU is at ~40%).
- Performance is mostly limited by the game's SPU job system, so a decent CPU helps most.
- There are occasional freezes and other bugs.

If you hit a problem, let me know how to reproduce it, and if you can, send the log from the `logs\` folder for that session (the launcher has a button for it). That helps a ton.

## How it works

- `ps3recomp/` is a modified copy of [ps3recomp](https://github.com/sp00nznet/ps3recomp) by sp00nznet (MIT licence). It holds the runtime, the PS3 system libraries (audio, input, file system, SPU scheduling) and the lifters that translate PowerPC and SPU code into C/C++.
- Graphics: the game's RSX command stream is fed to RPCS3's RSX core and Vulkan renderer, built as a library. This repository only contains the patch to RPCS3 (`ps3recomp/patches/r3-rpcs3.patch`); Setup downloads the original source and applies it. The patch adds DLSS / FSR upscaling, display modes and fixes for running outside the emulator.
- `project/` is the game-specific part of the port: its hooks, the 60 fps limiter, mouse look, the launcher and the DLSS bridge.
- `tools/r3_regen.py` runs the lifters on your decrypted `EBOOT.elf`, then re-applies this port's own edits to the generated code (`tools/hooks/`).

## Known Bugs

- White single-pixel "sparkles" on some surfaces. This is a long-standing RPCS3 bug with Insomniac's engine ([RPCS3 #11001](https://github.com/RPCS3/rpcs3/issues/11001) / [#7076](https://github.com/RPCS3/rpcs3/issues/7076)) and happens in every graphics mode.
- 60 fps changes the game's timing; if something (physics, a scripted sequence) looks wrong, try 30 fps in the launcher.
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
