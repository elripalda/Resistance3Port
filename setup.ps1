# Resistance 3 - native PC port: one-time setup.
#
# 1. installs the free build tools (Visual Studio Build Tools, LLVM, CMake,
#    Ninja, Python) with winget, if they are missing
# 2. asks for YOUR copy of the game (a disc image or disc dump folder), checks it and copies it
#    into this folder (gamefiles\)
# 3. decrypts the game's program file (EBOOT.BIN; game.self is the same file)
#    and regenerates the port's game code from it (tools\r3_regen.py)
# 4. builds the game (build.bat)
#
# Nothing from the game is downloaded: everything game-specific comes from the
# copy you point it at. Run it again any time; finished steps are skipped.
#
# Options (Setup.bat passes them on):
#   -CheckOnly            only checks this PC and lists what Setup would install or do; changes nothing
#   -Game "<path>"        the game copy (.iso or folder); skips the question
#   -FFmpegZip "<zip>"    an FFmpeg 7.1 LGPL shared win64 zip you downloaded yourself (if the pinned link is gone)
#   -NoOptionalSdks       skips the optional NVIDIA DLSS / Streamline / Intel XeSS downloads (AMD FSR 3.1 still works)
#   -AcceptSdkLicenses    accepts the DLSS / XeSS licences without asking (unattended setup)
#   -Unattended           never waits for keyboard input (Setup.exe runs it so); needs -Game when the game copy
#                         is still missing, skips the optional SDKs unless -AcceptSdkLicenses, makes no shortcut
#   -GameCopyMode Copy|Move   with -Unattended: copy or move a game folder on the same drive (default Copy)

param(
    [string]$Game = '',         # Setup.bat -Game "<.iso or folder>" skips the question (the installer uses it)
    [switch]$CheckOnly,         # check this PC and report what Setup would do; nothing is installed or downloaded
    [string]$FFmpegZip = '',    # a local FFmpeg 7.1 LGPL shared win64 zip, if the pinned download is gone
    [switch]$NoOptionalSdks,    # don't download the optional NVIDIA DLSS / Intel XeSS SDKs
    [switch]$AcceptSdkLicenses, # accept the DLSS / XeSS licences without asking
    [switch]$Unattended,        # Setup.exe: no prompts (see above)
    [ValidateSet('', 'Copy', 'Move')][string]$GameCopyMode = ''
)

# 'Continue': tools like python/winget print warnings on stderr, which
# PowerShell 5 would otherwise turn into a fatal error. Every step checks
# its own result instead.
$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'
# Started from a PowerShell 7 window, Windows PowerShell inherits pwsh's module paths and then fails to load
# its own built-in modules (Get-FileHash, Expand-Archive ... 'not recognized'). Keep only its own paths.
if ($PSVersionTable.PSVersion.Major -lt 6 -and $env:PSModulePath) { $env:PSModulePath = (($env:PSModulePath -split ';') | Where-Object { $_ -and $_ -notmatch '\\PowerShell\\(7[^\\]*\\)?Modules\\?$' }) -join ';' }
$Port = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $Port
$env:PYTHONUTF8 = '1'
New-Item -ItemType Directory -Force -Path (Join-Path $Port 'logs') | Out-Null
$LogName = if ($CheckOnly) { 'setup_check.log' } elseif ($Unattended) { 'setup_script.log' } else { 'setup.log' }   # Setup.exe keeps logs\setup.log
try { Start-Transcript -Path (Join-Path $Port "logs\$LogName") -Force | Out-Null } catch {}
trap { Write-Host ""; Write-Host "SETUP STOPPED with an unexpected error: $_" -ForegroundColor Red; Write-Host "The details are in logs\$LogName."; try { Stop-Transcript | Out-Null } catch {}; if (-not $CheckOnly -and -not $Unattended) { Read-Host "Press Enter to close" }; exit 1 }

# The supported version: Resistance 3, BCUS98176 (US disc), v1.00.
$TitleId = 'BCUS98176'
$Sha1 = @{ 'EBOOT.elf' = '733dc453543f8db28495cf1339f6e68533ad40a8' }

function Step($t) { Write-Host ""; Write-Host "== $t" -ForegroundColor Cyan }
function Info($t) { Write-Host "   $t" }
function Fail($t) {
    Write-Host ""; Write-Host "SETUP STOPPED: $t" -ForegroundColor Red
    Write-Host "Nothing is lost - fix the problem above and run Setup.bat again. (Full log: logs\$LogName)"
    Dismount-GameSource
    try { Stop-Transcript | Out-Null } catch {}
    if (-not $CheckOnly -and -not $Unattended) { Read-Host "Press Enter to close" }; exit 1
}
# Finding, checking and copying the player's game copy (shared with the installer and the other ports).
. (Join-Path $Port 'tools\installer\gamefiles.ps1')
$GameProfile = Import-GameProfile (Join-Path $Port 'tools\installer\game_profile.psd1')
function Refresh-Path {
    $m = [Environment]::GetEnvironmentVariable('Path', 'Machine'); $u = [Environment]::GetEnvironmentVariable('Path', 'User')
    $env:Path = "$m;$u;$env:LOCALAPPDATA\Microsoft\WinGet\Links;C:\Program Files\LLVM\bin;C:\Program Files\CMake\bin"
}
function Have($cmd) { return [bool](Get-Command $cmd -ErrorAction SilentlyContinue) }
function Winget-Install($id, $label, $extra) {
    Info "Installing $label (this can take several minutes)..."
    if (Have 'winget') {
        $a = @('install', '--id', $id, '-e', '--accept-package-agreements', '--accept-source-agreements', '--silent')
        if ($extra) { $a += $extra }
        & winget @a | Out-Host
        $c = $LASTEXITCODE
        Refresh-Path
        # 0, already installed, no newer version, reboot needed
        if ($c -eq 0 -or $c -eq 3010 -or $c -eq -1978335135 -or $c -eq -1978335189 -or $c -eq -1978334967) { return }
        Info "winget failed (exit code $c). Downloading the official installer instead (Windows asks for permission)..."
    } else {
        Info "winget is not available. Downloading the official installer instead (Windows asks for permission)..."
    }
    # the same pinned, SHA-256 / signature checked downloads Setup.exe uses (tools\installer\prereqs.ps1)
    $pr = Join-Path $Port 'tools\installer\prereqs.ps1'
    $lg = Join-Path $Port 'logs\prereqs_install.log'
    try {
        Start-Process -FilePath "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -Verb RunAs -Wait -ArgumentList `
            "-NoProfile -ExecutionPolicy Bypass -File `"$pr`" -Install -NoWinget -Ids $id -Log `"$lg`""
    } catch { Fail "Could not install $label (the permission prompt was declined?). Install it yourself and run Setup again." }
    if (Test-Path $lg) { Get-Content -LiteralPath $lg -Tail 5 | ForEach-Object { Info $_ } }
    Refresh-Path
}
function Sha1Of($p) { (Get-FileHash -Algorithm SHA1 -LiteralPath $p).Hash.ToLower() }

# ---------------------------------------------------------------- pinned downloads
# FFmpeg: an LGPL v3 shared build (configured without --enable-gpl). The port only loads avcodec/avutil at run
# time to decode the game's H.264 and MPEG-2 video and AC3 audio; all three decoders are in the LGPL build.
$FFmpegUrl    = 'https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-07-31-14-10/ffmpeg-n7.1.5-12-g1fdbca85aa-win64-lgpl-shared-7.1.zip'
$FFmpegSha256 = '0F376F96FB38554CCEFB1B2AE9C7C6A7B351F0E60A372B38262C320E8392C5D0'
# Optional upscaler SDKs: downloaded from the vendor to this PC only after the player accepts the vendor's licence.
$DlssLicense  = 'https://github.com/NVIDIA/DLSS/blob/v310.9.16/LICENSE.txt'
$XessUrl      = 'https://github.com/intel/xess/releases/download/v2.1.1/XeSS_SDK_2.1.1.zip'
$XessSha256   = 'AFBCD939160CBD7C5C6DB4C4C6F79CAB482492BE23582B938F280D94561C9D40'
$XessLicense  = 'https://github.com/intel/xess/blob/v2.1.1/LICENSE.txt'
# NVIDIA Streamline 2.14.16 (headers MIT; sl.*.dll / nvngx_dlssg.dll under the NVIDIA RTX SDK licence) for DLSS Frame Generation.
$SlUrl        = 'https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.14.16/streamline-sdk-v2.14.16.zip'
$SlSha256     = '32041E51DB0CAB709A271B72A352281B690C2ABBD669A827809401A5DD23956B'
$SlLicense    = 'https://github.com/NVIDIA-RTX/Streamline/blob/v2.14.16/license.txt'
# What this port needs from the PC.
$NeedGit   = $true
$DiskGB    = 50          # free space for a first setup (game copy + tools + build), as in README.md
$GameGB    = 36          # of which the game copy
$GpuApi    = 'Vulkan'
$GenMarker = 'recompiled\ppu_recomp.h'
$UsrFiles  = @('EBOOT.BIN')

function Find-Python {
    foreach ($c in @('py', 'python')) {
        if (Have $c) {
            $v = & $c -c "import sys; print(sys.version_info[0]*100+sys.version_info[1])" 2>$null
            if ($LASTEXITCODE -eq 0 -and ("$v".Trim() -match '^\d+$') -and [int]"$v".Trim() -ge 310) { return $c }
        }
    }
    return $null
}
function Find-VS {
    $vw = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vw)) { return $null }
    return (& $vw -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
}
function Find-WinSdk {
    try { $root = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Microsoft SDKs\Windows\v10.0' -ErrorAction Stop).InstallationFolder } catch { return $null }
    if (-not $root) { return $null }
    $rc = Get-ChildItem (Join-Path $root 'bin\*\x64\rc.exe') -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($rc) { return $rc.FullName }; return $null
}
function Test-Avx2 {
    # PF_AVX2_INSTRUCTIONS_AVAILABLE (40). Older Windows 10 builds don't know it and return false.
    try {
        if (-not ('PortSetup.Cpu' -as [type])) {
            Add-Type -Namespace PortSetup -Name Cpu -MemberDefinition '[DllImport("kernel32.dll")] public static extern bool IsProcessorFeaturePresent(uint f);'
        }
        return [PortSetup.Cpu]::IsProcessorFeaturePresent(40)
    } catch { return $false }
}
function Test-FFmpegLgpl([string]$dir) {
    # 'lgpl' = the LGPL build Setup installs; 'other' = DLLs from an older setup (a GPL build); 'missing'
    if (-not (Test-Path (Join-Path $dir 'avcodec-61.dll'))) { return 'missing' }
    $lic = Join-Path $dir 'LICENSE.txt'
    if ((Test-Path $lic) -and ((Get-Content -LiteralPath $lic -TotalCount 1) -cmatch 'GNU LESSER GENERAL PUBLIC LICENSE')) { return 'lgpl' }
    return 'other'
}
function Test-Url([string]$u) {
    for ($i = 0; $i -lt 2; $i++) {
        try { Invoke-WebRequest -UseBasicParsing -Method Head -Uri $u -TimeoutSec 30 | Out-Null; return $true } catch { Start-Sleep -Seconds 2 }
    }
    return $false
}
function Confirm-SdkLicense([string]$name, [string]$url) {
    if ($AcceptSdkLicenses) { Info "$name licence accepted with -AcceptSdkLicenses: $url"; return $true }
    if ($Unattended) { Info "Skipping $name (not accepted in Setup). That option will use AMD FSR 3.1 instead."; return $false }
    Write-Host ""
    Write-Host "   Optional download: $name" -ForegroundColor Yellow
    Info "It comes straight from the vendor's GitHub to this PC. It is not part of the port, and using it means"
    Info "accepting the vendor's licence (read it here): $url"
    $a = Read-Host "   Accept that licence and download $name? [Y/n]"
    if ($a -match '^\s*n') { Info "Skipped. That option will use AMD FSR 3.1 instead. Run Setup.bat again to change your mind."; return $false }
    return $true
}

# ---------------------------------------------------------------- PC check (also: Setup.bat -CheckOnly)
$script:Chk = $null
function Chk-Line([string]$tag, [string]$t, [string]$col) { Write-Host ('   [{0,-7}] {1}' -f $tag, $t) -ForegroundColor $col }
function Chk-Ok($t)   { Chk-Line 'OK' $t 'Green' }
function Chk-Note($t) { Chk-Line 'INFO' $t 'Gray' }
function Chk-Warn($t) { $script:Chk.Warnings++; Chk-Line 'WARN' $t 'Yellow' }
function Chk-Err($t)  { $script:Chk.Errors++; if (-not $script:Chk.FirstError) { $script:Chk.FirstError = $t }; Chk-Line 'ERROR' $t 'Red' }
function Chk-Todo($t) { $script:Chk.Todo++; Chk-Line 'WILL DO' $t 'Cyan' }
function Test-ThisPC([switch]$Full) {
    $script:Chk = @{ Errors = 0; Warnings = 0; Todo = 0; FirstError = '' }
    $os = [Environment]::OSVersion.Version
    if (-not [Environment]::Is64BitOperatingSystem) { Chk-Err "Windows is 32-bit. The port needs 64-bit Windows 10 or 11." }
    elseif ($os.Major -lt 10) { Chk-Err "Windows $os is too old. The port needs 64-bit Windows 10 or 11." }
    elseif ($os.Build -lt 17763) { Chk-Warn "Windows 10 build $($os.Build) is old: winget (used to install the build tools) needs build 17763 or newer. Run Windows Update first." }
    else { Chk-Ok ("Windows {0} 64-bit (build {1})" -f $(if ($os.Build -ge 22000) { '11' } else { '10' }), $os.Build) }
    if (Test-IsWine) { Chk-Warn "Wine/Proton detected: Setup and the build are only tested on Windows." }

    $cpu = "$((Get-CimInstance Win32_Processor -ErrorAction SilentlyContinue | Select-Object -First 1).Name)".Trim()
    if (Test-Avx2) { Chk-Ok "CPU with AVX2: $cpu" }
    else { Chk-Warn "Could not confirm AVX2 on this CPU ($cpu). The port needs AVX2 (Intel 4th gen or newer, any AMD Ryzen). Older Windows 10 versions can't report it, so this may be fine." }
    $ram = [math]::Round(((Get-CimInstance Win32_ComputerSystem -ErrorAction SilentlyContinue).TotalPhysicalMemory) / 1GB)
    if ($ram -ge 16) { Chk-Ok "$ram GB RAM" } elseif ($ram -gt 0) { Chk-Warn "$ram GB RAM. The build needs a lot of memory: 16 GB or more is recommended; close other programs while it builds." }

    $gpus = @(Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue | ForEach-Object { "$($_.Name)".Trim() } | Where-Object { $_ -and $_ -notmatch 'Basic Display|Basic Render|Remote|Virtual|Parsec|Meta Virtual' })
    if ($gpus.Count) { Chk-Ok "GPU: $($gpus -join ', ')" } else { Chk-Warn "No graphics driver found (only Windows' basic display driver). Install the latest driver for your GPU." }
    if ($GpuApi -eq 'Vulkan') {
        if (Test-Path (Join-Path $env:SystemRoot 'System32\vulkan-1.dll')) { Chk-Ok "Vulkan runtime (vulkan-1.dll)" }
        else { Chk-Warn "vulkan-1.dll is missing, so the GPU driver has no Vulkan support. The game needs a Vulkan 1.3 driver: install the latest one from NVIDIA, AMD or Intel." }
    }
    if (-not ($gpus -match 'RTX')) { Chk-Note "NVIDIA DLSS needs a GeForce RTX card; on this PC use AMD FSR 3.1 or Intel XeSS." }

    $haveGame = $true
    foreach ($n in $UsrFiles) { if (-not (Test-Path (Join-Path $Port "gamefiles\PS3_GAME\USRDIR\$n"))) { $haveGame = $false } }
    try {
        $drv = New-Object IO.DriveInfo ([IO.Path]::GetPathRoot($Port))
        $free = [math]::Floor($drv.AvailableFreeSpace / 1GB)
        $need = if ($haveGame) { $DiskGB - $GameGB } else { $DiskGB }
        if ($free -ge $need) { Chk-Ok "$free GB free on $($drv.Name) (about $need GB needed)" }
        else { Chk-Warn "Only $free GB free on $($drv.Name); about $need GB is needed$(if (-not $haveGame) { " ($GameGB GB of it for the game copy)" }). Free some space or put the port on another drive." }
    } catch { Chk-Warn "Could not read the free space of this drive." }
    if ($Port -match '[^\x20-\x7E]') { Chk-Warn "The folder path has non-English characters. If the build fails, move the port to a plain path like C:\Games\$(Split-Path $Port -Leaf)." }
    if ($Port -match '\\OneDrive') { Chk-Warn "The port is inside OneDrive. Syncing the large build and game folders is slow and can break the build; a folder like C:\Games is better." }
    if ($env:ProgramFiles -and $Port.StartsWith($env:ProgramFiles, [StringComparison]::OrdinalIgnoreCase)) { Chk-Warn "The port is under Program Files, where Windows may block writing. A folder like C:\Games is better." }

    $missing = 0
    $script:py = Find-Python
    if ($script:py) {
        Chk-Ok "Python: $("$(& $script:py --version 2>&1)".Trim())"
        & $script:py -c "import Crypto" 2>$null | Out-Null
        if ($LASTEXITCODE -eq 0) { Chk-Ok "Python package pycryptodome" } else { Chk-Todo "Install the Python package pycryptodome (pip)." }
    } else { $missing++; Chk-Todo "Install Python 3.12 (winget)." }
    $tools = @(@('cmake', 'CMake'), @('ninja', 'Ninja'), @('clang-cl', 'LLVM (clang-cl)'))
    if ($NeedGit) { $tools += ,@('git', 'Git') }
    foreach ($t in $tools) {
        $cmd = Get-Command $t[0] -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($cmd) { Chk-Ok "$($t[1]): $($cmd.Source)" } else { $missing++; Chk-Todo "Install $($t[1]) (winget)." }
    }
    $vsPath = Find-VS
    if ($vsPath) {
        Chk-Ok "Visual Studio C++ build tools: $vsPath"
        if (Find-WinSdk) { Chk-Ok "Windows SDK" }
        else { Chk-Warn "The Windows 10/11 SDK was not found. The build needs it: open Visual Studio Installer, Modify, and tick a Windows 11 SDK." }
    } else { $missing++; Chk-Todo "Install Visual Studio 2022 Build Tools with the C++ workload (winget; several GB, can take 15+ minutes)." }
    if (Have 'winget') { Chk-Ok "winget" }
    elseif ($missing) { Chk-Note "winget is not available: Setup downloads the official installers for the tools above directly." }

    if (-not $Full) { return }
    if (Test-Url 'https://github.com') { Chk-Ok "Internet (github.com)" } else { Chk-Err "github.com can't be reached. Setup downloads tools and libraries from GitHub: check the internet connection, proxy or firewall." }
    $ff = Test-FFmpegLgpl (Join-Path $Port 'tools\ffmpeg_bin')
    if ($ff -eq 'lgpl') { Chk-Ok "FFmpeg (LGPL build)" }
    else {
        Chk-Todo $(if ($ff -eq 'other') { "Replace the FFmpeg DLLs from an older setup with the LGPL build (~60 MB download)." } else { "Download FFmpeg 7.1, LGPL build (~60 MB, for the game's videos)." })
        if (-not $FFmpegZip -and -not (Test-Url $FFmpegUrl)) { Chk-Err "The pinned FFmpeg download is not available any more: $FFmpegUrl. Download a 'win64-lgpl-shared-7.1' zip from https://github.com/BtbN/FFmpeg-Builds/releases and run Setup.bat -FFmpegZip <zip>." }
    }
    if ((Test-Path (Join-Path $Port 'tools\dlss\lib\nvsdk_ngx_s.lib')) -and (Test-Path (Join-Path $Port 'tools\dlss\nvngx_dlss.dll'))) { Chk-Ok "NVIDIA DLSS SDK (optional)" }
    elseif ($NoOptionalSdks) { Chk-Note "NVIDIA DLSS SDK: skipped (-NoOptionalSdks)." }
    else { Chk-Todo "Optional: download the NVIDIA DLSS SDK after you accept NVIDIA's licence ($DlssLicense)." }
    if (Test-Path (Join-Path $Port 'tools\xess\bin\libxess.dll')) { Chk-Ok "Intel XeSS SDK (optional)" }
    elseif ($NoOptionalSdks) { Chk-Note "Intel XeSS SDK: skipped (-NoOptionalSdks)." }
    else { Chk-Todo "Optional: download the Intel XeSS SDK 2.1.1 (~90 MB) after you accept Intel's licence ($XessLicense)." }
    if (Test-Path (Join-Path $Port 'tools\streamline\bin\sl.interposer.dll')) { Chk-Ok "NVIDIA Streamline SDK (optional, DLSS Frame Generation)" }
    elseif ($NoOptionalSdks) { Chk-Note "NVIDIA Streamline SDK: skipped (-NoOptionalSdks)." }
    else { Chk-Todo "Optional: download NVIDIA Streamline 2.14.16 (~200 MB, DLSS Frame Generation) after you accept NVIDIA's licence ($SlLicense)." }

    if ($haveGame) { Chk-Ok "Game files: gamefiles\" } else { Chk-Todo "Ask for your copy of the game ($TitleId) and copy it into gamefiles\ ($GameGB GB)." }
    $dec = $true
    foreach ($n in $Sha1.Keys) { $p = Join-Path $Port "game\$n"; if (-not (Test-Path $p) -or (Sha1Of $p) -ne $Sha1[$n]) { $dec = $false } }
    if ($dec) { Chk-Ok "Decrypted program file(s): game\ (version checked)" } else { Chk-Todo "Decrypt the game's program file(s) into game\ and check the version." }
    if (Test-Path (Join-Path $Port $GenMarker)) { Chk-Ok "Game code generated" } else { Chk-Todo "Generate the game code from your copy." }
    if (Test-Path (Join-Path $Port 'Launcher.exe')) { Chk-Todo "Rebuild (only what changed)." } else { Chk-Todo "Build the port and the launcher (the first build takes 20-60 minutes)." }
}

Write-Host "Resistance 3 - PC port setup" -ForegroundColor Yellow
Write-Host "Folder: $Port"
if ($Port -match ' ') { Info "Note: this folder's path has spaces in it. That works, but a path without spaces (like C:\Games\Resistance3Port) is safer." }

if ($CheckOnly) {
    Step "Checking this PC (nothing will be installed, downloaded or changed)"
    Refresh-Path
    Test-ThisPC -Full
    Write-Host ""
    if ($script:Chk.Errors) {
        Write-Host "CHECK: $($script:Chk.Errors) problem(s) marked ERROR must be fixed before Setup can finish." -ForegroundColor Red
    } else {
        Write-Host "CHECK PASSED ($($script:Chk.Warnings) warning(s)). Setup.bat (without -CheckOnly) would do the WILL DO items above." -ForegroundColor Green
    }
    try { Stop-Transcript | Out-Null } catch {}
    exit $(if ($script:Chk.Errors) { 1 } else { 0 })
}
Step "Checking this PC"
Refresh-Path
Test-ThisPC
if ($script:Chk.Errors) { Fail $script:Chk.FirstError }

# ---------------------------------------------------------------- 1. tools
Step "1/5  Build tools"
Refresh-Path
$py = $null
foreach ($c in @('py', 'python')) {
    if (Have $c) {
        $v = & $c -c "import sys; print(sys.version_info[0]*100+sys.version_info[1])" 2>$null
        if ($LASTEXITCODE -eq 0 -and ("$v".Trim() -match '^\d+$') -and [int]"$v".Trim() -ge 310) { $py = $c; break }
    }
}
if (-not $py) { Winget-Install 'Python.Python.3.12' 'Python 3.12'; if (Have 'py') { $py = 'py' } elseif (Have 'python') { $py = 'python' } else { Fail "Python did not install." } }
# Run scripts with the real interpreter: "py script.py" follows the scripts' "#!/usr/bin/env python3" line,
# which on many PCs finds the Microsoft Store's python3.exe stub ("Python was not found").
$pyExe = "$(& $py -c "import sys; print(sys.executable)" 2>$null)".Trim()
if ($LASTEXITCODE -eq 0 -and $pyExe -and (Test-Path -LiteralPath $pyExe)) { $py = $pyExe }
Info "Python: OK"
& $py -c "import Crypto" 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) {
    Info "Installing pycryptodome (for decrypting the game files)..."
    & $py -m pip install --user --quiet --disable-pip-version-check pycryptodome 2>&1 | Out-Host
    & $py -c "import Crypto" 2>$null | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail "Could not install the Python package pycryptodome." }
}
if (-not (Have 'cmake')) { Winget-Install 'Kitware.CMake' 'CMake' }
if (-not (Have 'ninja')) { Winget-Install 'Ninja-build.Ninja' 'Ninja' }
if (-not (Have 'clang-cl')) { Winget-Install 'LLVM.LLVM' 'LLVM (clang)' }
if (-not (Have 'git')) { Winget-Install 'Git.Git' 'Git' }
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = $null
if (Test-Path $vswhere) { $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath }
if (-not $vs) {
    Winget-Install 'Microsoft.VisualStudio.2022.BuildTools' 'Visual Studio 2022 Build Tools (C++)' @('--override', '--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended')
    if (Test-Path $vswhere) { $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath }
    if (-not $vs) { Fail "Visual Studio Build Tools did not install (C++ workload)." }
}
foreach ($c in @('cmake', 'ninja', 'clang-cl', 'git')) { if (-not (Have $c)) { Fail "$c is still missing after installing it. Restart the PC and run Setup.bat again." } }
Info "Visual Studio C++ tools, LLVM, CMake, Ninja, Git: OK"

# ---------------------------------------------------------------- 2. game copy
Step "2/5  Your copy of the game"
$gf = Join-Path $Port 'gamefiles'
$usr = Join-Path $gf 'PS3_GAME\USRDIR'
if ((Test-Path -LiteralPath "$gf.copying") -or -not (Test-Path (Join-Path $usr 'EBOOT.BIN'))) {
    if ($Unattended -and -not $Game) { Fail "The game copy is missing or was not copied completely, and no -Game path was given. Pick your game in Setup.exe again." }
    $err = Install-GameFiles $GameProfile $Game $gf $Port -Mode $GameCopyMode
    if ($err) { Fail $err }
}
# the PS3's hard-drive cache folder the game expects to exist. The game-data folder (game\<id>) is
# left to the game's own first-run install: an empty one made here reads as installed-but-corrupted.
New-Item -ItemType Directory -Force -Path (Join-Path $gf "cache\$TitleId") | Out-Null
$gd = Join-Path $gf "game\$TitleId"
if ((Test-Path $gd) -and -not (Get-ChildItem $gd -Recurse -File -ErrorAction SilentlyContinue | Select-Object -First 1)) { Remove-Item $gd -Recurse -Force }
Info "Game files: $gf"

# ---------------------------------------------------------------- 3. decrypt
Step "3/5  Decrypting the game program file"
New-Item -ItemType Directory -Force -Path (Join-Path $Port 'game') | Out-Null
$need = $false
foreach ($n in $Sha1.Keys) { $p = Join-Path $Port "game\$n"; if (-not (Test-Path $p) -or (Sha1Of $p) -ne $Sha1[$n]) { $need = $true } }
if ($need) {
    $kv = Join-Path $Port 'tools\key_vault.cpp'
    if (-not (Test-Path $kv)) {
        Info "Fetching the PS3 application keys from RPCS3's public source code..."
        try { Invoke-WebRequest -UseBasicParsing -Uri 'https://raw.githubusercontent.com/RPCS3/rpcs3/master/rpcs3/Crypto/key_vault.cpp' -OutFile $kv }
        catch { Fail "Could not download key_vault.cpp from github.com (no internet?)." }
    }
    & $py tools\unself.py --keys $kv --out game "$usr\EBOOT.BIN" | Out-Host
    if ($LASTEXITCODE -ne 0) { Fail "Decrypting EBOOT.BIN failed." }
    foreach ($n in $Sha1.Keys) {
        if ((Sha1Of (Join-Path $Port "game\$n")) -ne $Sha1[$n]) {
            Fail "game\$n is not the expected version. The port supports the unpatched US disc (BCUS98176 v1.00); a game update or another release won't match."
        }
    }
}
Info "EBOOT.elf: OK (BCUS98176 v1.00)"

# ---------------------------------------------------------------- 4. game code
Step "4/5  Generating the game code from your copy"
if (-not (Test-Path (Join-Path $Port 'recompiled\ppu_recomp.h')) -or -not (Test-Path (Join-Path $Port 'project\spu\jobmods_register.c')) -or $env:R3_REGEN) {
    & $py tools\r3_regen.py --eboot game\EBOOT.elf | Out-Host
    if ($LASTEXITCODE -ne 0) { Fail "Generating the game code failed (see the messages above)." }
} else { Info "Already generated (set R3_REGEN=1 to force)." }

# ---------------------------------------------------------------- 5. build
Step "5/5  Building (the first build takes 20-60 minutes)"
$ffd = Join-Path $Port 'tools\ffmpeg_bin'
if ($FFmpegZip -or (Test-FFmpegLgpl $ffd) -ne 'lgpl') {
    if ((Test-FFmpegLgpl $ffd) -eq 'other') { Info "Replacing the FFmpeg DLLs from an older setup with the LGPL build..." }
    $zip = Join-Path $env:TEMP "$($TitleId)_ffmpeg_lgpl.zip"
    if ($FFmpegZip) {
        if (-not (Test-Path -LiteralPath $FFmpegZip)) { Fail "-FFmpegZip: there is no file at $FFmpegZip" }
        $zip = (Resolve-Path -LiteralPath $FFmpegZip).Path
        Info "Using your FFmpeg zip: $zip"
    } else {
        Info "Fetching FFmpeg 7.1, LGPL build (~60 MB, for the game's videos)..."
        try { Invoke-WebRequest -UseBasicParsing -Uri $FFmpegUrl -OutFile $zip }
        catch { Fail "Could not download FFmpeg from github.com ($FFmpegUrl).
If the internet works and that link is gone, download an FFmpeg 7.1 'win64-lgpl-shared-7.1' zip from https://github.com/BtbN/FFmpeg-Builds/releases and run:  Setup.bat -FFmpegZip <path of the zip>" }
        if ((Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash -ne $FFmpegSha256) { Remove-Item $zip -Force; Fail "The FFmpeg download is incomplete or not the expected file (SHA-256 mismatch). Run Setup.bat again." }
    }
    $tmp = Join-Path $env:TEMP "$($TitleId)_ffmpeg"; if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
    Expand-Archive -LiteralPath $zip -DestinationPath $tmp
    $bin = Get-ChildItem $tmp -Recurse -Filter 'avcodec-61.dll' | Select-Object -First 1
    if (-not $bin) { Fail "This FFmpeg zip has no avcodec-61.dll: it must be an FFmpeg 7.1 shared build (avcodec-61, avutil-59, swresample-5)." }
    $lic = Get-ChildItem $tmp -Recurse -Filter 'LICENSE.txt' | Sort-Object { $_.FullName.Length } | Select-Object -First 1
    if (-not $lic -or (Get-Content -LiteralPath $lic.FullName -TotalCount 1) -cnotmatch 'GNU LESSER GENERAL PUBLIC LICENSE') { Fail "This FFmpeg zip is not an LGPL build (its LICENSE.txt is not the GNU LGPL). Use a 'lgpl-shared' build." }
    if (Test-Path $ffd) { Remove-Item (Join-Path $ffd '*') -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $ffd | Out-Null
    foreach ($n in 'avcodec-61.dll', 'avutil-59.dll', 'swresample-5.dll') {
        $f = Join-Path $bin.DirectoryName $n
        if (-not (Test-Path $f)) { Fail "This FFmpeg zip has no $n." }
        Copy-Item -LiteralPath $f -Destination $ffd
    }
    Copy-Item -LiteralPath $lic.FullName -Destination (Join-Path $ffd 'LICENSE.txt')
    $from = if ($FFmpegZip) { $zip } else { $FFmpegUrl }
    Set-Content -LiteralPath (Join-Path $ffd 'SOURCE.txt') -Encoding ASCII -Value @(
        'FFmpeg shared libraries, LGPL v3 (see LICENSE.txt), unmodified, from:', $from,
        'FFmpeg source code: https://github.com/FFmpeg/FFmpeg (release/7.1). Build scripts: https://github.com/BtbN/FFmpeg-Builds')
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
    if (-not $FFmpegZip) { Remove-Item $zip -Force -ErrorAction SilentlyContinue }
}
Info "FFmpeg (LGPL build): OK"
$dl = Join-Path $Port 'tools\dlss'
$needDlss = -not (Test-Path (Join-Path $dl 'lib\nvsdk_ngx_s.lib')) -or -not (Test-Path (Join-Path $dl 'nvngx_dlss.dll'))
if ($needDlss -and $NoOptionalSdks) { Info "Skipping the NVIDIA DLSS SDK (-NoOptionalSdks): the DLSS option will use AMD FSR 3.1 instead." }
elseif ($needDlss -and (Confirm-SdkLicense 'the NVIDIA DLSS SDK v310.9.16' $DlssLicense)) {
    Info "Fetching the NVIDIA DLSS SDK v310.9.16 (optional: the NVIDIA DLSS / DLAA option)..."
    $base = 'https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16'
    $hdrs = 'nvsdk_ngx.h','nvsdk_ngx_defs.h','nvsdk_ngx_defs_dlssd.h','nvsdk_ngx_defs_dlssg.h','nvsdk_ngx_defs_vk.h',
            'nvsdk_ngx_helpers.h','nvsdk_ngx_helpers_cuda.h','nvsdk_ngx_helpers_d3d.h','nvsdk_ngx_helpers_dlssd.h',
            'nvsdk_ngx_helpers_dlssd_cuda.h','nvsdk_ngx_helpers_dlssd_d3d.h','nvsdk_ngx_helpers_dlssd_vk.h',
            'nvsdk_ngx_helpers_dlssg.h','nvsdk_ngx_helpers_dlssg_d3d.h','nvsdk_ngx_helpers_dlssg_vk.h','nvsdk_ngx_helpers_vk.h',
            'nvsdk_ngx_loader.h','nvsdk_ngx_params.h','nvsdk_ngx_params_dlssd.h','nvsdk_ngx_params_dlssg.h',
            'nvsdk_ngx_standalone_common.h','nvsdk_ngx_standalone_cuda.h','nvsdk_ngx_vk.h'
    $files = @(@('LICENSE.txt', 'LICENSE.txt'),
               @('lib/Windows_x86_64/x64/nvsdk_ngx_s.lib', 'lib\nvsdk_ngx_s.lib'),
               @('lib/Windows_x86_64/rel/nvngx_dlss.dll', 'nvngx_dlss.dll')) + ($hdrs | ForEach-Object { ,@("include/$_", "include\$_") })
    try {
        foreach ($f in $files) {
            $out = Join-Path $dl $f[1]
            New-Item -ItemType Directory -Force -Path (Split-Path $out) | Out-Null
            Invoke-WebRequest -UseBasicParsing -Uri "$base/$($f[0])" -OutFile $out
        }
    } catch {
        Info "Could not download the DLSS SDK - building without the NVIDIA DLSS option (FSR still works)."
        if (Test-Path (Join-Path $dl 'lib')) { Remove-Item (Join-Path $dl 'lib') -Recurse -Force }
    }
}
$xd = Join-Path $Port 'tools\xess'
if (-not (Test-Path (Join-Path $xd 'bin\libxess.dll'))) {
    if ($NoOptionalSdks) { Info "Skipping the Intel XeSS SDK (-NoOptionalSdks): the XeSS option will use AMD FSR 3.1 instead." }
    elseif (Confirm-SdkLicense 'the Intel XeSS SDK 2.1.1' $XessLicense) {
        Info "Fetching the Intel XeSS SDK 2.1.1 (~90 MB, optional: the Intel XeSS option)..."
        $zip = Join-Path $env:TEMP "$($TitleId)_xess.zip"; $tmp = Join-Path $env:TEMP "$($TitleId)_xess"
        try {
            Invoke-WebRequest -UseBasicParsing -Uri $XessUrl -OutFile $zip
            if ((Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash -ne $XessSha256) { throw "the download is incomplete or not the expected file (SHA-256 mismatch)" }
            if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
            Expand-Archive -LiteralPath $zip -DestinationPath $tmp
            $dll = Get-ChildItem $tmp -Recurse -Filter 'libxess.dll' | Where-Object { $_.Directory.Name -eq 'bin' } | Sort-Object { $_.FullName.Length } | Select-Object -First 1
            if (-not $dll) { throw "libxess.dll is not in the zip" }
            $src = $dll.Directory.Parent.FullName
            New-Item -ItemType Directory -Force -Path (Join-Path $xd 'bin'), (Join-Path $xd 'inc') | Out-Null
            Copy-Item -LiteralPath (Join-Path $src 'inc\xess') -Destination (Join-Path $xd 'inc') -Recurse -Force
            foreach ($n in 'LICENSE.txt', 'third-party-programs.txt') { Copy-Item -LiteralPath (Join-Path $src $n) -Destination $xd -Force }
            Copy-Item -LiteralPath $dll.FullName -Destination (Join-Path $xd 'bin') -Force   # last: marks a complete copy
            Info "Intel XeSS SDK: OK"
        } catch { Info "Could not get the Intel XeSS SDK ($_) - building without the Intel XeSS option (FSR 3.1 is used instead)." }
        Remove-Item $zip, $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
}
$sd = Join-Path $Port 'tools\streamline'
if (-not (Test-Path (Join-Path $sd 'bin\sl.interposer.dll'))) {
    if ($NoOptionalSdks) { Info "Skipping NVIDIA Streamline (-NoOptionalSdks): no DLSS Frame Generation option." }
    elseif (Confirm-SdkLicense 'NVIDIA Streamline 2.14.16 (DLSS Frame Generation)' $SlLicense) {
        Info "Fetching NVIDIA Streamline 2.14.16 (~200 MB, optional: DLSS Frame Generation on GeForce RTX 40/50)..."
        $zip = Join-Path $env:TEMP "$($TitleId)_streamline.zip"; $tmp = Join-Path $env:TEMP "$($TitleId)_streamline"
        try {
            Invoke-WebRequest -UseBasicParsing -Uri $SlUrl -OutFile $zip
            if ((Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash -ne $SlSha256) { throw "the download is incomplete or not the expected file (SHA-256 mismatch)" }
            if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
            Expand-Archive -LiteralPath $zip -DestinationPath $tmp
            $bin = Join-Path $tmp 'bin\x64'
            if (-not (Test-Path (Join-Path $bin 'sl.interposer.dll'))) { throw "sl.interposer.dll is not in the zip" }
            New-Item -ItemType Directory -Force -Path (Join-Path $sd 'bin'), (Join-Path $sd 'include') | Out-Null
            Copy-Item -Path (Join-Path $tmp 'include\*') -Destination (Join-Path $sd 'include') -Recurse -Force
            foreach ($n in 'license.txt', '3rd-party-licenses.md') { Copy-Item -LiteralPath (Join-Path $tmp $n) -Destination $sd -Force }
            foreach ($n in 'nvngx_dlss.license.txt', 'reflex.license.txt') { Copy-Item -LiteralPath (Join-Path $bin $n) -Destination $sd -Force }
            foreach ($n in 'sl.common.dll', 'sl.dlss_g.dll', 'sl.reflex.dll', 'sl.pcl.dll', 'nvngx_dlssg.dll', 'NvLowLatencyVk.dll', 'sl.interposer.dll') {
                Copy-Item -LiteralPath (Join-Path $bin $n) -Destination (Join-Path $sd 'bin') -Force   # sl.interposer last: marks a complete copy
            }
            Info "NVIDIA Streamline: OK"
        } catch { Info "Could not get NVIDIA Streamline ($_) - building without DLSS Frame Generation." }
        Remove-Item $zip, $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
}
Info "Fetching RPCS3's renderer source and applying the port's patch..."
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Port 'tools\fetch_rpcs3.ps1') | Out-Host
if ($LASTEXITCODE -ne 0) { Fail "Could not download or patch the RPCS3 source (no internet?)." }
& cmd /c "`"$Port\build.bat`" nopause"
if ($LASTEXITCODE -ne 0) { Fail "The build failed - see logs\build.log and logs\build_game.log." }

$desktop = [Environment]::GetFolderPath('Desktop')
$lnk = Join-Path $desktop 'Resistance 3.lnk'
if ($desktop -and -not $Unattended -and -not (Test-Path -LiteralPath $lnk)) {   # Setup.exe makes its own shortcuts
    $ans = Read-Host "Create a desktop shortcut to the launcher? [Y/n]"
    if ($ans -notmatch '^\s*n') {
        try {
            $sc = (New-Object -ComObject WScript.Shell).CreateShortcut($lnk)
            $sc.TargetPath = Join-Path $Port 'Launcher.exe'
            $sc.WorkingDirectory = $Port
            $sc.IconLocation = (Join-Path $Port 'Launcher.exe') + ',0'
            $sc.Description = 'Resistance 3 - PC port launcher'
            $sc.Save()
            Info "Desktop shortcut created: $lnk"
        } catch { Info "Could not create the desktop shortcut ($_)." }
    }
}

Write-Host ""
Write-Host "SETUP COMPLETE. Start the game with Launcher.exe." -ForegroundColor Green
try { Stop-Transcript | Out-Null } catch {}
if (-not $Unattended) {
    Start-Process (Join-Path $Port 'Launcher.exe')
    Read-Host "Press Enter to close"
}
