# Modding: the mods folder

Turn on **Display › PC › Mods folder** in the launcher (`R2_MODS=1`). The game then reads the
`mods` folder next to the game. `R2_MODS=<folder>` uses a different folder. When the setting
is off, or the folder is missing or empty, nothing is hooked and the game reads its own files
exactly as before.

Each sub-folder of `mods` is one mod and is laid out like the game's `PS3_GAME` folder:

```
mods\
  MyMod\
    USRDIR\...                 files that replace the game's own (whole files)
  _Disabled\...                folders starting with _ or - are skipped
  .cache\                      patched archives made by the game (safe to delete)
```

- Names are matched without regard to case.
- Mods are applied in alphabetical order of their folder names. When two mods replace the same
  file, or the same archive member or asset, the later one wins.
- Every override is listed in `logs\run.log` as `[mods]` lines.

## 1. Whole files

`mods\MyMod\USRDIR\<path>` replaces `USRDIR\<path>` of the game. This works for loose files
and also for whole `.psarc` archives.

## 2. Single members of a .psarc archive

Most game data is packed in `.psarc` archives, for example
`USRDIR\packed\levels\<level>\level_cached.psarc`. To replace one file inside an archive,
create a **folder** with the archive's name. Inside it, put the replacement file at its path
within the archive:

```
mods\MyMod\USRDIR\packed\levels\<level>\level_cached.psarc\built\levels\<level>\mobys.dat
```

That replaces the member `/built/levels/<level>/mobys.dat`. Member names are listed in the
first entry (the manifest) of each archive; any PSARC tool shows them.

## 3. Single assets of a level `.dat` file, by tuid

A level's `.dat` files (`shaders.dat`, `textures.dat`, `mobys.dat`, ...) are packs of
assets. The level's `assetlookup.dat` lists every asset by its 64-bit **tuid**, with its
offset and length. To replace one asset, put its new bytes in a `<file>.dat.tuid` folder,
named after the tuid in hex. Any extension, or none, is fine:

```
mods\MyMod\USRDIR\packed\levels\<level>\level_cached.psarc\built\levels\<level>\shaders.dat.tuid\0123456789abcdef.bin
```

The blob is the asset exactly as stored at that offset in the `.dat`.

- If it is no larger than the original, it is written in place: the rest is zero-filled and
  the lookup length is updated.
- If it is larger, it is appended at the end of the `.dat` (128-byte aligned) and the lookup
  entry is repointed.
- The level `.toc` member, which lists the `.dat` sizes, is updated to match.

Supported `.dat` files (assetlookup section):

| File | Section |
|---|---|
| shaders.dat | 1D100 |
| textures.dat | 1D180 |
| cubemaps.dat | 1D200 |
| ties.dat | 1D300 |
| foliages.dat | 1D400 |
| shrubs.dat | 1D500 |
| mobys.dat | 1D600 |
| animsets.dat | 1D700 |
| cinematics.dat | 1D800 |
| zones.dat | 1DA00 |
| lighting.dat | 1DB00 |

A tuid override and a member override can be combined. The member file is used as the base
and the assets are then patched into it.

## How it works, and the cache

The first time the game opens an archive that has member or asset overrides, a patched copy is
written to `mods\.cache\` and the game reads that copy instead. In the copy:

- untouched members keep their original compressed data;
- changed members are appended;
- the archive's table of contents is updated.

The game's own files are never written. Later runs reuse the copy until the archive or any of
its mod files change (size or time). Then the copy is rebuilt and the old one is deleted. A
whole-file override of the same archive (section 1) is used as the base. Building the copy
takes a moment the first time a level loads (`[mods] ... done in N ms` in the log).

## Limitations

- Adding new members to an archive is not supported (they are listed as skipped in the log);
  only existing members can be replaced.
- Asset overrides need the `.dat` and its `assetlookup.dat` in the same archive. The
  `highmips.dat` (and, in Resistance 3, `highlight.dat`) assets live in `level_uncached.psarc`
  while the lookup is in `level_cached.psarc`. Replace those `.dat` files whole (section 2)
  instead.
- An asset blob must be in the game's own format. Assets that refer to other assets by offset
  rather than by tuid only work if those references still hold.
- Larger assets make the level use more memory. Check that the level still loads.
