# Installing Resistance 3 (PC port)

The release zip contains the port's source code and `Setup.exe`. It contains no game code: Setup builds the
game on your PC from your own copy of the game (decrypted ISO or game folder, **Resistance 3, US disc,
BCUS98176**).

## Quick install

1. Download the release zip and unzip **all** of it to a folder with no spaces in the path, for example
   `C:\Games\Resistance3Port`. Don't run Setup from inside the zip viewer.
2. Run **`Setup.exe`**. If Windows shows *"Windows protected your PC"*, see [SmartScreen](#windows-protected-your-pc-smartscreen).
3. **Your game:** click **ISO...** or **Folder...** and pick your copy. Setup checks it at once and says what is
   wrong if it can't be used (see the README, *If your game copy is not accepted*).
4. Click **Install** and wait. Setup:
   - installs the missing build tools (one administrator prompt; see [Build tools](#build-tools));
   - copies the game files into the port folder;
   - runs `setup.ps1` (decrypts the game's program, generates the game code, builds the game and the launcher);
   - downloads [shaderscan](https://github.com/miroslavdanutalexandru-alt/shaderscan) (GPL-2.0, a separate tool with its own
     repository) into `tools\shaderscan.exe`: the release pinned in `tools\shaderscan.version`, checked by SHA-256. The
     launcher uses it to prepare the shader cache from your game files. Offline, this step is skipped and the game
     works without it; to use your own build, set `SHADERSCAN_EXE=<path to shaderscan.exe>` before Setup / `build.bat`.
   - adds Start-menu shortcuts (and a desktop one if ticked) and starts the launcher. The launcher can compile the
     shaders before you play, so the first level doesn't stutter.

Progress, the current step and an estimate of the time left are shown in the window. Everything is logged to
`logs\setup.log`. You can keep using the PC; Setup keeps it from going to sleep while it works.

## Requirements (measured)

| | Minimum | Recommended |
|---|---|---|
| Windows | 10 or 11, 64-bit | |
| RAM | 12 GB (slow build) | 16 GB or more |
| Free disk space, port drive | 50 GB | 60 GB |
| Free disk space, C: (build tools) | 10 GB | |
| Internet | for the build tools and RPCS3's source (about 3 GB of downloads) | |

Disk space on the port drive, measured on a complete install:

| Part | Size |
|---|---|
| Game files (`gamefiles\`) | 37.0 GB |
| Generated game code (`recompiled\`) | 3.9 GB |
| Build folders (`build\`, `build_rpcs3\`) | 4.7 GB |
| Port source, tools, RPCS3 source | 0.8 GB |
| **Total** | **about 46.5 GB** |

If you pick a game folder on the same drive as the port, Setup can move it instead of copying it, which saves
the 37 GB for the game files. An ISO is always copied from (it is left as it is).

The build tools take about 8 GB on C: (Visual Studio Build Tools with the Windows SDK about 5 GB, LLVM about
2.8 GB, CMake, Ninja, Python and Git about 0.5 GB), plus temporary installer files.

**Time:** installing the tools takes 10–30 minutes, copying the game 5–15 minutes. The build is CPU-bound: the
game code is about 100 CPU-minutes for the generated code alone (single files take up to 15 minutes each), plus
about 40 CPU-minutes for the rest. Measured on a 12-thread desktop CPU: about 45–60 minutes with 32 GB of RAM,
about 2 hours with 16 GB, 2–3 hours with 12 GB. Later runs only rebuild what changed.

## Memory and build speed

The generated game code is split into large `ppu_recomp_*.cpp` files. Each one needs up to **8 GB of RAM** to
compile (measured 3.7–8.4 GB). Compiling several at once on a 16 GB PC runs out of memory, so Setup:

- compiles these files in their own queue (a Ninja job pool) with
  `max(1, floor((RAM − 6 GB) / 8 GB))` files at once, never more than the free RAM allows;
- compiles the other files with more jobs, limited by the CPU cores and free RAM;
- shows its choice in the **Build** box (for example *"1 big game-code file at a time, -j 2"*);
- if a compile still runs out of memory, it retries automatically with half the jobs (and remembers that).

With 12 GB it builds one file at a time. Close browsers and games while it builds.

To choose yourself (manual builds or `build.bat`), set these environment variables before running
`setup.ps1`/`build.bat`: `PORT_PPU_JOBS` (big files at once), `PORT_JOBS` (other files), `RPCS3_JOBS` (the RPCS3
renderer). `PORT_PPU_GB` changes Setup's RAM estimate per big file. The CMake option
`-DPORT_PPU_JOB_POOL=OFF` turns the separate queue off (it needs CMake 4.2 or newer).

## Resume, repair and uninstall

- **Resume:** if Setup stops (error, power cut, sleep, you clicked **Stop**), run `Setup.exe` again; it remembers the
  game and where it stopped (`logs\setup_state.ini`) and shows **Resume**. Tools already installed are skipped, a
  half-copied game copy is finished, and the build continues with the files not yet compiled.
- **Repair or rebuild:** Start menu → *Resistance 3* → *Setup (repair, rebuild, uninstall)*, then **Install**.
- **Uninstall:** in Setup, click **Uninstall...**. It deletes the build output, the generated code, the downloaded
  SDKs and the shortcuts. It asks before deleting the game files (`gamefiles\`, default: keep). It **keeps** your
  saves (`gamedata\`), settings (`settings.ini`, `controls.txt`, `mouselook.cfg`), `%USERPROFILE%\Saved Games\Resistance 3`,
  screenshots and logs. The build tools stay installed (remove them in Windows *Settings → Apps* if you like). Then
  delete the port folder if you want it gone completely.

## Build tools

Setup checks for and installs only what the build needs:

| Tool | Why | winget ID |
|---|---|---|
| Visual Studio 2022 Build Tools (C++ tools + Windows SDK) | linker, Windows headers, resource compiler | `Microsoft.VisualStudio.2022.BuildTools` |
| LLVM (clang-cl 18 or newer) | the compiler | `LLVM.LLVM` |
| CMake 4.2 or newer | build files (the per-file memory queue needs 4.2) | `Kitware.CMake` |
| Ninja | runs the build | `Ninja-build.Ninja` |
| Python 3.10 or newer | the code generator and decryption | `Python.Python.3.12` |
| Git | downloads RPCS3's source | `Git.Git` |

No Vulkan SDK is needed (only a GPU driver with Vulkan). The checks and installs are done by
`tools\installer\prereqs.ps1`, which you can run yourself:
`powershell -ExecutionPolicy Bypass -File tools\installer\prereqs.ps1 -Check`, or
`... -Install -Ids LLVM.LLVM,Kitware.CMake -DryRun` to see the commands without running them.

## Troubleshooting

### "Windows protected your PC" (SmartScreen)

`Setup.exe` is not code-signed (signing certificates cost money and this is a free project), so Windows
SmartScreen warns about it the first times it is run. Click **More info**, then **Run anyway**. If you'd rather
not, build `Setup.exe` yourself (below) or use the [manual install](#manual-install-advanced).

### Antivirus warnings

Some antivirus programs flag new, unsigned programs that download and run installers, or flag the freshly
compiled game. These are false positives: `Setup.exe` is built from `tools\installer\setup_exe\setup.cpp` in this
repository (one readable file), and it only runs the scripts in `tools\installer\` and `setup.ps1`. If your
antivirus blocks or quarantines `Setup.exe`, `Launcher.exe` or files in `build\`:

- restore the file and add the port folder as an exclusion, or
- build `Setup.exe` yourself, or use the manual install.

A real-time scan of the build folders also slows the build down; an exclusion for the port folder helps.

### Out of memory / the PC becomes very slow

Setup retries automatically with fewer jobs. If it still fails, close other programs and click **Resume**. On
12 GB, also make sure Windows manages the page file automatically (*System → About → Advanced system settings →
Performance → Advanced → Virtual memory*), with at least 16 GB available on some drive.

### Not enough disk space

Setup checks before it starts and says how much it needs. Free up space or move the port folder to another drive
(move the whole folder, then run `Setup.exe` from the new place).

### winget is missing

`winget` comes with the *App Installer* from the Microsoft Store (preinstalled on current Windows 10 and 11).
Without it, Setup downloads the tools from their official sites instead (fixed versions; each download is
checked against a known SHA-256 hash, and the Visual Studio installer against Microsoft's signature). To get
winget, install or update **App Installer** from the Microsoft Store. You can also install the tools in the table
above yourself, then run Setup again.

### A tool install fails

Restart the PC (Visual Studio's installer sometimes needs that) and click **Resume**. Details are in
`logs\prereqs.log`. If it keeps failing, install the tool yourself from its website and run Setup again.

### The game copy is not accepted

See the README, *If your game copy is not accepted*. Only the version listed there is supported.

### Anything else

Click **Open logs**. `logs\setup.log` (Setup), `logs\setup_script.log` (setup.ps1) and `logs\build.log` (the build)
say what went wrong. To report a problem, see [REPORTING.md](REPORTING.md) (it explains how to create a report).

## Manual install (advanced)

1. Install the tools in [Build tools](#build-tools) (with Visual Studio Build Tools: the *Desktop development
   with C++* workload).
2. Run `Setup.bat` (or `powershell -ExecutionPolicy Bypass -File setup.ps1 -Game "<path to ISO or folder>"`).
   See the README for details.

Useful `setup.ps1` options: `-Game <path>`, `-GameCopyMode Copy|Move`, `-AcceptSdkLicenses` / `-NoOptionalSdks`,
`-Unattended` (no questions; the log goes to `logs\setup_script.log`).

## Building Setup.exe yourself

With the Visual Studio C++ build tools installed, run `tools\installer\setup_exe\build_setup.bat`. It compiles
`setup.cpp` (with clang-cl if LLVM is installed, otherwise cl) into `Setup.exe` in the port folder.

Testing without changing anything: `Setup.exe --dry-run` prints the tool check, the install commands it would
run, the game check (`--game <path>`), the disk space, the job counts (`--ram-gb 16 --free-gb 12 --cores 8` to
simulate another PC), the resume state and the uninstall list (`--uninstall --dry-run`). From PowerShell, pipe it
(`.\Setup.exe --dry-run | Out-Host`) so the output is shown.
