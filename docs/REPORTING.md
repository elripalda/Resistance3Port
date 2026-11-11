# Reporting a problem (Resistance 3 beta)

Thanks for testing! A good report has two parts: **what happened** (in your words) and **the report zip** that the launcher creates for you.

## How to report

1. Open the **Launcher**.
2. Click **Report a problem** (on the *About* page, under *Folders*).
3. Write a few lines in **What happened?**: what you were doing, what you saw (crash, freeze, graphics glitch, sound problem...), and whether it happens again if you repeat it.
4. Tick **Include my save files** only if the problem depends on where you are in the game (for example, a crash when loading one save). It is off by default.
5. Click **Create report**. The launcher writes
   `Desktop\Resistance3_report_<date>_<time>.zip`
   and opens Explorer with the file selected.
6. Attach that zip to your bug report (Discord / GitHub issue / wherever the beta asks for reports), together with a short description. Screenshots or a short video help a lot for graphics bugs.

**After a crash or freeze:** the game saves crash information by itself. The next time you start the launcher, it asks *"The game crashed last time. Create a report?"*. Click **Create report** to get the same zip. **Not now** asks again at the next start, for up to a week.

If the launcher will not start, zip the `logs` folder of the game by hand (right-click, *Send to*, *Compressed (zipped) folder*) and attach that instead.

## What's in the zip

Everything is a plain file you can open and read before sending. The zip is capped at 50 MB.

| In the zip | What it is |
|---|---|
| `report.txt` | Windows version, CPU, RAM, monitors, GPU(s) and driver version (DXGI and Vulkan), game exe version/build date/git commit, which files were included or left out |
| `note.txt` | What you typed in "What happened?" |
| `crash\*.txt` | Latest crash/freeze report: error code, where it happened (module + offset, and the game function if known), short stack traces of all threads, last lines of the log, relevant settings |
| `crash\*.dmp` | Latest minidump (small, usually under a few MB, not a full memory dump). It lets developers see the state of the crash |
| `crash\crashes.txt` | One line per crash/freeze so far |
| `logs\` | `run.log`, `out.log` and other port logs (only the last 8 MB of each) |
| `settings\` | Launcher settings (`settings.ini` / `settings.bat`, `rpcs3_config.yml`, `controls.txt`, `run.bat`) |
| `saves\` | **Only if you ticked the box.** Your save files from `gamedata\dev_hdd0\home\00000001\savedata` in the game folder (or under `PS3_GAMEDATA_ROOT` if you set it) |

Never included: game data (the disc files and the recompiled game), shader caches, screenshots, executables, and other large files.

## Privacy

- **Nothing is uploaded.** The zip is only created on your Desktop; you decide whether and where to send it.
- In the text files, your Windows user folder is replaced with `%USERPROFILE%` and your Windows user name with `<user>`.
- The **minidump** (`.dmp`) is a snapshot of part of the game's memory at the moment of the crash. It can contain file paths (including your user name) and the names of programs or overlays loaded into the game (for example Discord or Steam overlays). It does not contain your other programs' data. If you don't want to share it, delete the `.dmp` from the zip before sending; the `.txt` report is still useful.
- `report.txt` lists your hardware (CPU, GPU, RAM, monitors) and Windows version, but not your PC name, serial numbers or network details.
- Save files are only included when you tick the box.

## Advanced: switches

Set these as environment variables (for example in `settings.bat` or before running `run.bat`). Testers normally do not need them.

| Variable | Effect |
|---|---|
| `PORT_HANG_DUMP=1` | Freeze detection: if the game loop or the renderer makes no progress for **20 s** (while not paused), write a freeze report and dump. The game keeps running. `PORT_HANG_S=<seconds>` changes the limit (minimum 5). |
| `PORT_HANG_DUMP=0` | No freeze detection at all. |
| (not set) | Freeze report only after the game loop has stopped for 90 s (`R2_HANG_S` changes it). |
| `PORT_CRASH_DUMPS=0` | Turn the crash reporter off (also `R2_CRASH_DUMPS=0`). |
| `PORT_CRASH_DUMPS=full` | Full-memory dumps (very large, several GB; only when a developer asks for one). |
| `PORT_CRASH_KEEP=<n>` | How many crash dumps to keep in `logs\crash` (default 12). |

Crash files are written to `logs\crash\` in the game folder (`<TAG>_<date>_<time>_crash.dmp` + `.txt`; `_hang` for freezes).