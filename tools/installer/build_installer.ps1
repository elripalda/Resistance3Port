# Builds the one-click installer for this port into dist\ (gitignored).
#
#   powershell -ExecutionPolicy Bypass -File tools\installer\build_installer.ps1 [-Launcher <path>]
#
# What goes in: the port's own files (everything tracked by git, plus new,
# not-ignored files under project\, tools\ and ps3recomp\) and a prebuilt
# Launcher.exe. Game files are never included: the staged list is checked
# against a block list and the build stops if anything matches.
# Needs Inno Setup 6 (free): winget install JRSoftware.InnoSetup
param(
    [string]$Launcher = '',
    [switch]$KeepStage
)
$ErrorActionPreference = 'Stop'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Repo = (Resolve-Path (Join-Path $Here '..\..')).Path
$Dist = Join-Path $Repo 'dist'
$Stage = Join-Path $Dist 'stage'

$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
          "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
          "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw "Inno Setup 6 was not found. Install it with: winget install JRSoftware.InnoSetup" }

$version = '0.0.0'
if (Test-Path "$Repo\VERSION") { $version = (Get-Content "$Repo\VERSION" -TotalCount 1).Trim() }

# Paths (forward slashes, relative to the repo) that must never be shipped.
$block = @(
    '^(game|gamefiles|gamedata|recompiled[^/]*|build[^/]*|dist|logs|shadercache|rsxcap|scratch|cache|lle|fpval)/',
    '^tools/(key_vault\.cpp|ffmpeg_bin/|dlss/|xess/|shaderscan\.exe(\.download)?$)',
    '^project/spu/[^/]+/',
    '(^|/)(EBOOT\.BIN|PARAM\.SFO|PS3_DISC\.SFB)$',
    '\.(elf|self|sprx|iso|pkg|edat|sdat|pup|rap)$',
    '(^|/)settings\.(bat|ini)$', '^gamefiles\.copying$'
)
function Is-Blocked([string]$rel) { foreach ($b in $block) { if ($rel -match $b) { return $true } }; return $false }

Push-Location $Repo
try {
    $tracked = @(git -c core.quotepath=off ls-files)
    if ($LASTEXITCODE -ne 0) { throw "git ls-files failed (run this from a git checkout)." }
    $extra = @(git -c core.quotepath=off ls-files --others --exclude-standard) |
        Where-Object { $_ -match '^(project|tools|ps3recomp|docs)/' -or $_ -notmatch '/' } |
        # skip tool output and pictures (captures can show game content)
        Where-Object { $_ -notmatch '(^|/)(out|output|captures?|screenshots?)/' -and $_ -notmatch '\.(png|jpe?g|bmp|dds|tga|gif|webp|mp4|wav|log)$' }
} finally { Pop-Location }

$files = @($tracked + $extra | Sort-Object -Unique | Where-Object { Test-Path -LiteralPath (Join-Path $Repo $_) -PathType Leaf })
$skipped = @($files | Where-Object { Is-Blocked $_ })
$files = @($files | Where-Object { -not (Is-Blocked $_) })
if ($skipped.Count) { Write-Host "Left out $($skipped.Count) blocked path(s):"; $skipped | Select-Object -First 20 | ForEach-Object { Write-Host "   $_" } }

if (Test-Path $Stage) { Remove-Item $Stage -Recurse -Force }
New-Item -ItemType Directory $Stage -Force | Out-Null
foreach ($f in $files) {
    $dst = Join-Path $Stage $f
    $dir = Split-Path -Parent $dst
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory $dir -Force | Out-Null }
    Copy-Item -LiteralPath (Join-Path $Repo $f) -Destination $dst -Force
}

# Prebuilt launcher (and R1's level-switch helper), so the shortcuts work
# before the first build. build.bat rebuilds them later.
if (-not $Launcher) {
    $Launcher = @("$Repo\build_dist\Launcher.exe", "$Repo\Launcher.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $Launcher -or -not (Test-Path $Launcher)) { throw "No Launcher.exe found. Build it first (build.bat), or pass -Launcher <path>." }
Copy-Item $Launcher "$Stage\Launcher.exe" -Force
foreach ($h in 'RFoMSwitch.exe') {
    $src = @("$Repo\build_dist\$h", "$Repo\build\$h") | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($src) { Copy-Item $src "$Stage\$h" -Force }
}

# Final guard over everything staged, including the copied exes.
$bad = @(Get-ChildItem $Stage -Recurse -File | ForEach-Object { $_.FullName.Substring($Stage.Length + 1).Replace('\', '/') } | Where-Object { Is-Blocked $_ })
if ($bad.Count) { throw "Blocked files ended up in the stage: $($bad -join ', ')" }
$size = (Get-ChildItem $Stage -Recurse -File | Measure-Object Length -Sum).Sum
Write-Host ("Staged {0} files, {1:N1} MB, version {2}" -f $files.Count, ($size / 1MB), $version)

& $iscc /Q "/DStageDir=$Stage" "/DOutDir=$Dist" "/DVersion=$version" (Join-Path $Here 'port.iss')
if ($LASTEXITCODE -ne 0) { throw "ISCC failed with exit code $LASTEXITCODE" }
if (-not $KeepStage) { Remove-Item $Stage -Recurse -Force }
Get-ChildItem $Dist -Filter '*-Setup-*.exe' | ForEach-Object { Write-Host ("Built {0} ({1:N1} MB)" -f $_.FullName, ($_.Length / 1MB)) }
