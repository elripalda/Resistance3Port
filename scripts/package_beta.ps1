<#
.SYNOPSIS
  Checks and builds the release assets of this port: the source at the release tag plus the one-click
  installer (Setup). No prebuilt game is ever packaged. Run -DryRun to check only.

.DESCRIPTION
  Distribution model (0.2.0-beta on): players get the port's source and Setup, supply their own copy of
  the game, and Setup builds the game on their PC. The release assets are therefore:
    * <PortShort>-Setup-<VERSION>.exe  built by tools\installer\build_installer.ps1 (Inno Setup): the
      port's files (git-tracked + new not-ignored files) plus Launcher.exe (R1 also RFoMSwitch.exe).
    * <PortShort>-<VERSION>.zip, the release zip: git archive of HEAD plus Setup.exe (the one-click
      setup, gitignored) at its root. docs\INSTALL.md and the README tell players to download this.
    * the source archive, which GitHub generates from the tag (or -SourceZip: git archive of HEAD).
    * SHA256SUMS.txt for the assets.

  Works in all three Resistance ports; the port is picked by the title ID in setup.ps1:
  BCUS98107 = Resistance: Fall of Man, BCES00226 = Resistance 2, BCUS98176 = Resistance 3.

  Checks (errors stop a real run; -DryRun reports them and exits 1):
    * VERSION is semver (x.y.z or x.y.z-pre); warns when it is not a pre-release or still 0.1.0.
    * The release docs and the setup / installer files exist (README, LICENSE, THIRD_PARTY_NOTICES,
      docs\RELEASE_NOTES_BETA.md, docs\REPORTING.md, Setup.bat, setup.ps1, tools\installer\...).
    * The installer file set (computed the way build_installer.ps1 does) contains nothing from the
      strict block list below: game data or anything generated from it (recompiled code, SPU code,
      EBOOT, caches, logs, saves, settings), build output, downloaded SDKs and their binaries
      (tools\dlss, tools\xess, tools\streamline, tools\ffmpeg_bin), RPCS3's key_vault.cpp, debug symbols, and any
      .exe / .dll other than the allow-listed port tools and the AMD FidelityFX DLLs.
    * Launcher.exe is not older than its sources; uncommitted changes are reported, because the
      installer is built from the working tree while the published source is the tag.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\package_beta.ps1 -DryRun
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\package_beta.ps1 -SourceZip
#>
[CmdletBinding()]
param(
    [switch]$DryRun,          # check and list only; write nothing
    [switch]$ListFiles,       # with -DryRun: print every file that the installer would contain
    [string]$Launcher = '',   # passed to build_installer.ps1 (default: build_dist\Launcher.exe, else .\Launcher.exe)
    [switch]$SourceZip,       # also write dist\<PortShort>-<VERSION>-source.zip from git HEAD (git archive)
    [switch]$AllowDirty       # build even when tracked files have uncommitted changes (not for public releases)
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Dist = Join-Path $Repo 'dist'

$warnings = New-Object System.Collections.Generic.List[string]
$errors = New-Object System.Collections.Generic.List[string]
function Warn([string]$m) { $warnings.Add($m); Write-Host "WARNING: $m" -ForegroundColor Yellow }
function Fail([string]$m) { $errors.Add($m); Write-Host "ERROR:   $m" -ForegroundColor Red }

# ---------------------------------------------------------------- port profile
$setupText = Get-Content -Raw -LiteralPath (Join-Path $Repo 'setup.ps1')
if ($setupText -notmatch "\`$TitleId\s*=\s*'([A-Z]{4}\d{5})'") { throw "Can't find `$TitleId in setup.ps1" }
$TitleId = $Matches[1]
switch ($TitleId) {
    'BCUS98107' { $Prof = @{ Game = 'Resistance: Fall of Man'; Short = 'ResistancePort';  Helpers = @('RFoMSwitch.exe');
                             FfxDlls = '^tools/ffx_bin/amd_fidelityfx_(loader|upscaler)_dx12\.dll$' } }
    'BCES00226' { $Prof = @{ Game = 'Resistance 2';            Short = 'Resistance2Port'; Helpers = @();
                             FfxDlls = '^tools/ffx_vk/amd_fidelityfx_vk\.dll$' } }
    'BCUS98176' { $Prof = @{ Game = 'Resistance 3';            Short = 'Resistance3Port'; Helpers = @();
                             FfxDlls = '^tools/ffx_vk/amd_fidelityfx_vk\.dll$' } }
    default { throw "Unknown title ID $TitleId in setup.ps1" }
}
$iss = Join-Path $Repo 'tools\installer\port.iss'
if ((Test-Path -LiteralPath $iss) -and ((Get-Content -Raw -LiteralPath $iss) -match '#define\s+PortShort\s+"([^"]+)"')) { $Prof.Short = $Matches[1] }

Write-Host "== $($Prof.Game) ($TitleId)  repo: $Repo"
Write-Host "   mode: $(if ($DryRun) { 'DRY RUN (nothing is written)' } else { 'BUILD INSTALLER + CHECKSUMS' })"

# ---------------------------------------------------------------- git state
$head = (& git -C $Repo rev-parse --short HEAD 2>$null); $branch = (& git -C $Repo rev-parse --abbrev-ref HEAD 2>$null)
if (-not $head) { throw "Not a git checkout: $Repo" }
$dirtyList = @(& git -C $Repo -c core.quotepath=off status --porcelain --untracked-files=no 2>$null)
$untrackedList = @(& git -C $Repo -c core.quotepath=off ls-files --others --exclude-standard 2>$null)
Write-Host "   git: $branch @ $head, $($dirtyList.Count) modified tracked file(s), $($untrackedList.Count) untracked not-ignored file(s)"
if ($dirtyList.Count) {
    $m = "$($dirtyList.Count) tracked file(s) have uncommitted changes; the installer is built from the working tree but the published source is the tag. Commit first."
    if ($AllowDirty -or $DryRun) { Warn $m } else { Fail "$m (or -AllowDirty for a private test)" }
}

# ---------------------------------------------------------------- version
$ver = $null
$versionFile = Join-Path $Repo 'VERSION'
if (Test-Path -LiteralPath $versionFile) {
    $ver = (Get-Content -LiteralPath $versionFile -TotalCount 1).Trim()
    Write-Host "   VERSION: $ver   -> tag v$ver, asset $($Prof.Short)-Setup-$ver.exe"
    if ($ver -notmatch '^\d+\.\d+\.\d+(-[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?$') { Fail "VERSION '$ver' is not semver (x.y.z or x.y.z-beta[.n]); the launcher's update check compares semver" }
    elseif ($ver -eq '0.1.0') { Fail "VERSION is still 0.1.0" }
    elseif ($ver -notmatch '-') { Warn "VERSION '$ver' has no pre-release suffix; publish betas as GitHub pre-releases with a -beta version" }
} else { Fail "VERSION file missing" }

# ---------------------------------------------------------------- required files
$required = @('README.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'VERSION', 'Setup.bat', 'setup.ps1', 'run.bat',
              'docs\RELEASE_NOTES_BETA.md', 'docs\REPORTING.md', 'docs\INSTALL.md', 'tools\installer\prereqs.ps1', 'tools\installer\gamefiles.ps1',
              'tools\installer\build_installer.ps1', 'tools\installer\port.iss', 'tools\installer\port_common.iss')
foreach ($f in $required) { if (-not (Test-Path -LiteralPath (Join-Path $Repo $f))) { Fail "$f missing" } }
foreach ($f in @('docs\INSTALL.md')) { if (-not (Test-Path -LiteralPath (Join-Path $Repo $f))) { Warn "$f missing (the setup work adds it)" } }
$setupExeSrc = Join-Path $Repo 'tools\installer\setup_exe\setup.cpp'
$setupExe = Join-Path $Repo 'Setup.exe'
if (Test-Path -LiteralPath $setupExeSrc) {
    if (-not (Test-Path -LiteralPath $setupExe)) { Fail "Setup.exe missing (build it with tools\installer\setup_exe\build_setup.bat)" }
    else {
        $src = Get-ChildItem -LiteralPath (Split-Path $setupExeSrc) -File | Where-Object { $_.Extension -in '.cpp', '.h', '.rc', '.manifest' } |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($src.LastWriteTime -gt (Get-Item -LiteralPath $setupExe).LastWriteTime) { Warn "Setup.exe is older than tools\installer\setup_exe\$($src.Name); rebuild it" }
    }
} else { Warn "no one-click Setup.exe source (tools\installer\setup_exe\); the installer will rely on Setup.bat" }
foreach ($doc in 'README.md', 'docs\RELEASE_NOTES_BETA.md', 'THIRD_PARTY_NOTICES.md') {
    $p = Join-Path $Repo $doc
    if ((Test-Path -LiteralPath $p) -and ((Get-Content -Raw -LiteralPath $p) -match 'gyan\.dev|full_build-shared')) { Fail "$doc still describes the GPL FFmpeg build" }
}

# ---------------------------------------------------------------- launcher
$launcherExe = if ($Launcher) { $Launcher } else { @("$Repo\build_dist\Launcher.exe", "$Repo\Launcher.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1 }
if (-not $launcherExe -or -not (Test-Path -LiteralPath $launcherExe)) { Fail "Launcher.exe missing (build.bat builds it)" }
else {
    $lt = (Get-Item -LiteralPath $launcherExe).LastWriteTime
    $newest = Get-ChildItem -LiteralPath (Join-Path $Repo 'project') -File | Where-Object { $_.Name -like 'launcher*' } |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    Write-Host "   Launcher: $launcherExe ($lt)"
    if ($newest -and $newest.LastWriteTime -gt $lt) { Warn "Launcher.exe is older than project\$($newest.Name) ($($newest.LastWriteTime)); rebuild it before building the installer" }
}
foreach ($h in $Prof.Helpers) {
    if (-not (@("$Repo\build_dist\$h", "$Repo\build\$h") | Where-Object { Test-Path $_ })) { Warn "$h not built (build_dist\ or build\); the installer will go without it" }
}

# ---------------------------------------------------------------- installer file set
# Same selection as tools\installer\build_installer.ps1 (tracked + new not-ignored files under
# project/, tools/, ps3recomp/, docs/ and the root, minus its own block list) ...
$instBlock = @(
    '^(game|gamefiles|gamedata|recompiled[^/]*|build[^/]*|dist|logs|shadercache|rsxcap|scratch|cache|lle|fpval)/',
    '^tools/(key_vault\.cpp|ffmpeg_bin/|dlss/|xess/|streamline(_dl)?/|shaderscan\.exe(\.download)?$)', '^project/spu/[^/]+/',
    '(^|/)(EBOOT\.BIN|PARAM\.SFO|PS3_DISC\.SFB)$', '\.(elf|self|sprx|iso|pkg|edat|sdat|pup|rap)$', '(^|/)settings\.(bat|ini)$', '^gamefiles\.copying$'
)
$tracked = @(& git -C $Repo -c core.quotepath=off ls-files)
$extra = @($untrackedList | Where-Object { $_ -match '^(project|tools|ps3recomp|docs)/' -or $_ -notmatch '/' } |
    Where-Object { $_ -notmatch '(^|/)(out|output|captures?|screenshots?)/' -and $_ -notmatch '\.(png|jpe?g|bmp|dds|tga|gif|webp|mp4|wav|log)$' })
$set = @($tracked + $extra | Sort-Object -Unique | Where-Object { Test-Path -LiteralPath (Join-Path $Repo $_) -PathType Leaf } |
    Where-Object { $rel = $_; -not ($instBlock | Where-Object { $rel -match $_ }) })

# ... checked against a stricter list. Anything hit here would ship in the installer: fix the
# installer's block list or .gitignore (or untrack the file) before releasing.
$strict = [ordered]@{
    'game data / ISO-derived'   = '(^|/)(PS3_GAME|USRDIR|TROPDIR|gamefiles|gamedata|savedata)(/|$)|(^|/)EBOOT\.(BIN|elf)$|\.(elf|self|sprx|sdat|edat|iso|pkg|psarc|pup|rap)$|TROPHY\.TRP$'
    'generated game code'       = '(^|/)recompiled|^project/spu/[^/]+/|(^|/)lle/'
    'build output'              = '(^|/)build[^/]*/|(^|/)CMakeFiles/|\.(obj|exp|ilk|pdb|ninja)$|\.pdb\.syms$|(^|/)obj/'
    'caches / logs / dumps'     = '\.foz$|(^|/)shadercache/|(^|/)logs/|\.log$|\.dmp$|(^|/)(crash|dumps?)/|(^|/)(rsxcap|fpval|cache)/'
    'user settings / saves'     = '(^|/)settings\.(ini|bat)$|portable\.txt$|trophies\.json$|mouselook_r2\.cfg$'
    'downloaded SDKs'           = '^tools/(dlss|xess|streamline|ffmpeg_bin)/|(^|/)(nvngx_dlssg?|sl\.\w+|NvLowLatencyVk|libxess|avcodec-\d+|avutil-\d+|swresample-\d+|r2_dlss|rfom_dlss)\.dll$|nvsdk_ngx'
    'RPCS3 key_vault'           = 'key_vault'
}
$allowBin = @('^Launcher\.exe$', '^Setup\.exe$', '^RFoMSwitch\.exe$', $Prof.FfxDlls)
$hits = @()
foreach ($f in $set) {
    foreach ($k in $strict.Keys) { if ($f -match $strict[$k]) { $hits += "$f  [$k]"; break } }
    if ($f -match '\.(exe|dll)$' -and -not ($allowBin | Where-Object { $f -match $_ })) { $hits += "$f  [binary not on the allow list]" }
}
$hits = @($hits | Sort-Object -Unique)
foreach ($h in $hits) { Fail "the installer would ship $h" }
foreach ($f in 'README.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'VERSION', 'docs/RELEASE_NOTES_BETA.md', 'docs/REPORTING.md', 'docs/INSTALL.md') {
    if ((Test-Path -LiteralPath (Join-Path $Repo $f)) -and ($set -notcontains $f)) { Warn "$f is not in the installer yet: build_installer.ps1 only picks up new files under project/, tools/, ps3recomp/, docs/ and the root, so commit it" }
}
# Setup.exe is gitignored: it is added to the release zip (below) and to the Inno installer, which opens it after installing.

$sizes = @($set | ForEach-Object { [pscustomobject]@{ Rel = $_; Len = (Get-Item -LiteralPath (Join-Path $Repo $_)).Length } })
$total = ($sizes | Measure-Object Len -Sum).Sum
Write-Host ""
Write-Host ("Installer content: {0} files, {1:N1} MB before compression ({2} tracked, {3} new not-ignored), plus Launcher.exe{4}" -f `
    $set.Count, ($total / 1MB), @($set | Where-Object { $tracked -contains $_ }).Count, @($set | Where-Object { $tracked -notcontains $_ }).Count,
    $(if ($Prof.Helpers.Count) { ' and ' + ($Prof.Helpers -join ', ') } else { '' }))
$top = @($set | ForEach-Object { ($_ -split '/')[0] + $(if ($_ -match '/') { '/' } else { '' }) } | Group-Object | Sort-Object Count -Descending)
Write-Host ("   by top folder: " + (($top | ForEach-Object { "$($_.Name) $($_.Count)" }) -join ', '))
Write-Host "   largest files:"
$sizes | Sort-Object Len -Descending | Select-Object -First 8 | ForEach-Object { Write-Host ('   {0,10:N0} KB  {1}' -f ($_.Len / 1KB), $_.Rel) }
$bins = @($set | Where-Object { $_ -match '\.(exe|dll|lib)$' })
Write-Host ("   binaries: " + $(if ($bins.Count) { $bins -join ', ' } else { '(none)' }))
$newFiles = @($set | Where-Object { $tracked -notcontains $_ })
if ($newFiles.Count) {
    Write-Host "   new (untracked) files that would ship; commit them so the tag's source matches the installer:"
    $newFiles | Select-Object -First 25 | ForEach-Object { Write-Host "      $_" }
    if ($newFiles.Count -gt 25) { Write-Host "      ... and $($newFiles.Count - 25) more" }
}
if ($ListFiles) { Write-Host ""; $set | ForEach-Object { Write-Host "   $_" } }

# Downloaded runtime pieces on this PC (informational: they are never packaged).
$ffLic = Join-Path $Repo 'tools\ffmpeg_bin\LICENSE.txt'
if (-not (Test-Path -LiteralPath $ffLic)) { Warn "tools\ffmpeg_bin has no LICENSE.txt: this PC still has the old GPL FFmpeg; re-run Setup to get the LGPL build (not packaged either way)" }
elseif ((Get-Content -Raw -LiteralPath $ffLic) -notmatch 'LESSER GENERAL PUBLIC') { Warn "tools\ffmpeg_bin\LICENSE.txt is not the LGPL; re-run Setup" }

# ---------------------------------------------------------------- result
Write-Host ""
if ($errors.Count) {
    Write-Host "$($errors.Count) error(s), $($warnings.Count) warning(s)." -ForegroundColor Red
    if (-not $DryRun) { throw "Release packaging stopped; fix the errors above." }
    exit 1
}
if ($DryRun) {
    Write-Host "Dry run OK: $($set.Count) files would go into $($Prof.Short)-Setup-$ver.exe; $($warnings.Count) warning(s). Nothing was written." -ForegroundColor Green
    exit 0
}

# ---------------------------------------------------------------- build
$bi = Join-Path $Repo 'tools\installer\build_installer.ps1'
$biArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $bi)
if ($Launcher) { $biArgs += @('-Launcher', $Launcher) }
& powershell @biArgs
if ($LASTEXITCODE -ne 0) { throw "build_installer.ps1 failed ($LASTEXITCODE)" }
$assets = @()
$setupAsset = Join-Path $Dist "$($Prof.Short)-Setup-$ver.exe"
if (-not (Test-Path -LiteralPath $setupAsset)) { throw "Expected $setupAsset was not built" }
$assets += $setupAsset
# Zip times are local time without a time zone: a player behind our time zone would get files dated
# in the future, which makes Ninja regenerate forever. Dating every entry a day back avoids that.
function Set-ZipPastTimes($zip) {
    $t = [DateTimeOffset]::Now.AddDays(-1)
    foreach ($e in @($zip.Entries)) { $e.LastWriteTime = $t }
}
if (Test-Path -LiteralPath $setupExe) {
    $relZip = Join-Path $Dist "$($Prof.Short)-$ver.zip"
    if (Test-Path -LiteralPath $relZip) { Remove-Item -LiteralPath $relZip -Force }
    & git -C $Repo archive --format=zip "--prefix=$($Prof.Short)-$ver/" -o $relZip HEAD
    if ($LASTEXITCODE -ne 0) { throw "git archive failed" }
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $z = [System.IO.Compression.ZipFile]::Open($relZip, 'Update')
    try {
        [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($z, $setupExe, "$($Prof.Short)-$ver/Setup.exe")
        Set-ZipPastTimes $z
    } finally { $z.Dispose() }
    $assets += $relZip
}
if ($SourceZip) {
    $srcZip = Join-Path $Dist "$($Prof.Short)-$ver-source.zip"
    & git -C $Repo archive --format=zip "--prefix=$($Prof.Short)-$ver/" -o $srcZip HEAD
    if ($LASTEXITCODE -ne 0) { throw "git archive failed" }
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $z = [System.IO.Compression.ZipFile]::Open($srcZip, 'Update')
    try { Set-ZipPastTimes $z } finally { $z.Dispose() }
    $assets += $srcZip
}
$sums = @($assets | ForEach-Object { '{0}  {1}' -f (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash.ToLowerInvariant(), (Split-Path -Leaf $_) })
[System.IO.File]::WriteAllLines((Join-Path $Dist 'SHA256SUMS.txt'), $sums, (New-Object System.Text.UTF8Encoding $false))
Write-Host ""
$sums | ForEach-Object { Write-Host $_ }
Write-Host ""
Write-Host "Done ($($warnings.Count) warning(s)). Publish: tag v$ver, GitHub release marked 'pre-release', attach $(($assets | ForEach-Object { Split-Path -Leaf $_ }) -join ', ') and SHA256SUMS.txt." -ForegroundColor Green