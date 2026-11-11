# Third-party notices: Resistance 3 PC port (0.2.0-beta)

**How this port is distributed.** The release is source code plus Setup: this repository at the release tag (or the GitHub source archive), and `Resistance3Port-Setup-<version>.exe`, an installer that contains the same files plus the port's own `Launcher.exe`. **No built game program, no game data, and no NVIDIA, Intel or FFmpeg binaries are distributed by this project.** Setup runs on your PC: it reads your own copy of the game, downloads the optional SDKs below from their publishers (after you accept their licences) and builds `build\Resistance3.exe` there.

*Resistance 3* is © Sony Interactive Entertainment, developed by Insomniac Games. The game files, the decrypted program and the code generated from it come only from your own copy and stay on your PC. This is an unofficial fan project, not affiliated with or endorsed by Sony, Insomniac, NVIDIA, AMD or Intel.

The port's own code is MIT (see `LICENSE`).

## Shipped in the repository and the installer

| Component | Licence | Licence file | Where |
|---|---|---|---|
| The port's code, scripts and `Launcher.exe` | MIT | `LICENSE` | `project\`, `tools\`, root |
| ps3recomp (runtime, PS3 libraries, lifters) by Ned Heller / sp00nznet | MIT | `ps3recomp\LICENSE` | `ps3recomp\` |
| AMD FidelityFX SDK 1.1.4 (FSR 3.1 upscaling and FSR 3 frame interpolation / frame-generation swapchain), Vulkan: `amd_fidelityfx_vk.dll` and the unmodified `ffx_api` headers | MIT | `tools\ffx_vk\LICENSE.txt` | `tools\ffx_vk\` |
| RPCS3 patch (the changes to RPCS3's RSX core and Vulkan renderer) | **GPL-2.0** | `ps3recomp\third_party\rpcs3\LICENSE` (after Setup fetches RPCS3) | `ps3recomp\patches\` |
| SDL3 (controllers; linked statically) | Zlib | `ps3recomp\third_party\SDL3\LICENSE.txt` | `ps3recomp\third_party\SDL3\` |
| SoundTouch 2.4.1 (audio time stretching; linked statically) | LGPL-2.1 | `ps3recomp\third_party\soundtouch\COPYING.TXT` | `ps3recomp\third_party\soundtouch\` |
| at3_standalone (FFmpeg's ATRAC3 / ATRAC3plus decoders, from PPSSPP commit `ebc151f`; linked statically) | LGPL-2.1-or-later | `ps3recomp\third_party\at3_standalone\COPYING.LGPLv2.1` | `ps3recomp\third_party\at3_standalone\` |
| Valve Fossilize (pipeline recording), with RapidJSON, miniz and dirent | MIT (RapidJSON: MIT with BSD / JSON notes) | `ps3recomp\third_party\fossilize\LICENSE` and the files in its subfolders | `ps3recomp\third_party\fossilize\` |
| FFmpeg headers (`libavcodec`, `libavutil`, ...) | LGPL-2.1-or-later | the header of each file | `ffmpeg\include\` |

## Fetched or downloaded by Setup on your PC (not distributed by us)

| Component | Licence | Licence file after Setup | Notes |
|---|---|---|---|
| RPCS3, upstream commit `eee3f0f7c285f2b8aa18fec278a2146c5cd874eb`, and its submodules (glslang, yaml-cpp, AsmJit, Vulkan Memory Allocator, stb, pugixml, Vulkan-Headers) | GPL-2.0 (RPCS3); BSD / MIT / Zlib / Apache-2.0 (submodules; glslang has one GPL-3.0-with-Bison-exception generated file) | `ps3recomp\third_party\rpcs3\LICENSE` and each submodule's licence | The port's patch from `ps3recomp\patches\` is applied on top. |
| FFmpeg 7.1 (BtbN `win64-lgpl-shared-7.1` build, unmodified `avcodec-61`, `avutil-59`, `swresample-5`) | **LGPL-3.0-or-later** (configured with `--enable-version3`, without `--enable-gpl`) | `tools\ffmpeg_bin\LICENSE.txt`; `tools\ffmpeg_bin\SOURCE.txt` says where the build and its source come from | Pinned by SHA-256. Loaded at run time for the H.264, MPEG-2 video and AC-3 decoders. |
| shaderscan v1.0.0 (`tools\shaderscan.exe`, the launcher's optional offline shader-cache seeding; from https://github.com/miroslavdanutalexandru-alt/shaderscan) | **GPL-2.0-only** (it hashes and analyses shaders the way RPCS3 does); contains miniz (MIT) and the Fall of Man port's RSX translators (MIT) | `LICENSE` and `THIRD_PARTY_NOTICES.md` in the shaderscan repository, which also holds its source | Release exe, pinned by version and SHA-256 in `tools\shaderscan.version` and downloaded by `tools\get_shaderscan.ps1` (Setup / `build.bat`). A separate program that the launcher runs; not linked into the port or the game. Optional: skipped when offline. |
| NVIDIA DLSS SDK v310.9.16 (headers, `nvsdk_ngx_s.lib`, `nvngx_dlss.dll`) | NVIDIA RTX SDK licence | `tools\dlss\LICENSE.txt` | Optional; only after you accept the licence. Used only by `r2_dlss.dll`, a separate bridge DLL built from the port's MIT `project\r2_dlss.cpp` that the game loads at run time through a small C interface. |
| Intel XeSS SDK 2.1.1 (headers, `libxess.dll`) | Intel Simplified Software License | `tools\xess\LICENSE.txt`, `tools\xess\third-party-programs.txt` | Optional; only after you accept the licence. Unmodified; no reverse engineering. |
| NVIDIA Streamline SDK 2.14.16 (headers; `sl.interposer/common/dlss_g/reflex/pcl.dll`, `nvngx_dlssg.dll`, `NvLowLatencyVk.dll`) | Headers MIT; DLLs NVIDIA RTX SDK licence / NVIDIA Reflex licence | `tools\streamline\license.txt`, `tools\streamline\nvngx_dlss.license.txt`, `tools\streamline\reflex.license.txt`, `tools\streamline\3rd-party-licenses.md` | Optional (DLSS Frame Generation); only after you accept the licence. Unmodified, signed NVIDIA production DLLs. |
| RPCS3's `key_vault.cpp` (public application keys) | GPL-2.0 | as RPCS3 | Used only by Setup to decrypt your own copy; not part of the game program. |

## The game program you build

`build\Resistance3.exe`, built on your PC, links the RPCS3 renderer and is therefore covered by the **GPL-2.0**. Its complete corresponding source is this repository at the release tag plus RPCS3 at the commit above. SoundTouch and at3_standalone (LGPL) are linked statically; this source lets you relink them with modified versions. FFmpeg (LGPL) is loaded as separate, replaceable DLLs.

**If you redistribute a build you made yourself**, you take on the obligations of every licence above, including the GPL-2.0 source requirement. Do not redistribute it with `r2_dlss.dll` or `nvngx_dlss.dll`: the NVIDIA RTX SDK licence forbids making the SDK subject to an open-source licence (§4e) and has its own redistribution conditions. Never redistribute anything made from the game itself.

## Not used

- **NVIDIA Streamline:** not used.
- **SDL3.dll:** not needed, because SDL3 is linked statically.
- **Microsoft Visual C++ runtime:** not bundled. Install it from Microsoft (VC++ 2015–2022 x64, 14.40 or newer).
- **Vulkan loader (`vulkan-1.dll`):** comes with the GPU driver.

## Attributions

- **NVIDIA DLSS:** this port uses NVIDIA DLSS. NVIDIA, GeForce RTX and NVIDIA DLSS are trademarks of NVIDIA Corporation. The DLSS SDK licence (Supplement §7.1(b)) asks for this attribution on the splash screen, about box or credits; it appears in the README credits and the launcher's About page.
- AMD FidelityFX, Intel XeSS, NVIDIA Streamline, RPCS3, SDL, SoundTouch, at3_standalone, FFmpeg and Fossilize: credited in the README and covered by the licence files above.
