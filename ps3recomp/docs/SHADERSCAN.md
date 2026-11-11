# shaderscan

shaderscan, the offline RSX shader-cache seeding tool the launcher runs before Play, now lives in its own
repository: **https://github.com/miroslavdanutalexandru-alt/shaderscan** (GPL-2.0-only). Its README there covers
what it does, the supported games and back ends, the command line, exit codes, the launcher contract and building.

In this port:

- Setup / `build.bat` run `tools\get_shaderscan.ps1`, which downloads the release pinned in
  `tools\shaderscan.version` into `tools\shaderscan.exe` and checks its SHA-256. It does nothing when the file
  already matches, and skips with a message when offline; the launcher works without it.
- `set SHADERSCAN_EXE=<path to shaderscan.exe>` before `build.bat` uses your own build instead (for example one built
  from the shaderscan repository with `build.bat`).
- To move to a newer release, update `version`, `url` and `sha256` in `tools\shaderscan.version`.
- The exe is never part of the port's source or Setup (`scripts\package_beta.ps1` blocks it).
