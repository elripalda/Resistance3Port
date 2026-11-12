# Build-tool check and install for Setup.exe (shared by the three Resistance ports).
#
#   prereqs.ps1 -Check -Out <file>
#       Checks the tools the build needs and writes one line per tool:
#       key|status|version|path|label|install-id      status: ok, missing, old, nosdk
#   prereqs.ps1 -Install -Ids <id,id,...> -Log <file> -Out <file> [-DryRun]
#       Installs the given tools. Setup.exe runs this once, elevated (one UAC prompt).
#       Uses winget with exact package IDs; without winget it downloads the pinned
#       installers below, checks their SHA-256 / Microsoft signature and runs them.
#       If winget is there but fails for a tool, that tool falls back to the direct download.
#       -DryRun only prints the commands it would run; -NoWinget forces the direct downloads.
#
# The tools: Python 3.10+ (decrypt + code generator), CMake 3.20+ (4.2+ for the
# per-file RAM job pool), Ninja, LLVM clang-cl, Visual Studio 2022 Build Tools with
# the C++ workload and a Windows SDK (linker, libraries, rc.exe), and Git for the
# ports that fetch RPCS3's source (tools\fetch_rpcs3.ps1). No Vulkan SDK is needed.
param(
    [switch]$Check,
    [switch]$Install,
    [string[]]$Ids = @(),
    [switch]$DryRun,
    [switch]$NoWinget,          # use the direct downloads even when winget is there (testing)
    [string]$Out = '',
    [string]$Log = ''
)
$ErrorActionPreference = 'Continue'
$Port = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if ($Ids.Count -eq 1 -and $Ids[0] -match ',') { $Ids = $Ids[0] -split ',' }
$NeedGit = Test-Path (Join-Path $Port 'tools\fetch_rpcs3.ps1')

$VsWorkload = '--wait --quiet --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended'
# id -> winget package, label and the direct download used when winget is missing
$Pkgs = [ordered]@{
    'Python.Python.3.12' = @{ Label = 'Python 3.12'; Url = 'https://www.python.org/ftp/python/3.12.10/python-3.12.10-amd64.exe'
        Sha256 = '67b5635e80ea51072b87941312d00ec8927c4db9ba18938f7ad2d27b328b95fb'; Kind = 'exe'; Args = '/quiet InstallAllUsers=1 PrependPath=1 Include_test=0' }
    'Kitware.CMake'      = @{ Label = 'CMake'; Url = 'https://github.com/Kitware/CMake/releases/download/v4.4.4/cmake-4.4.4-windows-x86_64.msi'
        Sha256 = 'f7aac962e8eea82c0a1c603bb11ee3514806a43a35147d5b1e9d69926ffd7d7e'; Kind = 'msi'; Args = 'ADD_CMAKE_TO_PATH=System' }
    'Ninja-build.Ninja'  = @{ Label = 'Ninja'; Url = 'https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip'
        Sha256 = '07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65'; Kind = 'zip'; Args = '' }
    'LLVM.LLVM'          = @{ Label = 'LLVM (clang-cl)'; Url = 'https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.5/LLVM-23.1.5-win64.msi'
        Sha256 = '288ab326377157a9e02e6fb9724914cc2b55963882d493a405f31dc66e837c12'; Kind = 'msi'; Args = '' }
    'Git.Git'            = @{ Label = 'Git'; Url = 'https://github.com/git-for-windows/git/releases/download/v2.56.0.windows.4/Git-2.56.0.4-64-bit.exe'
        Sha256 = '4961a4a8cc778d8b6e00a5819e5fe4d078b754555aff7aedc2d77a5d74c2de80'; Kind = 'exe'; Args = '/VERYSILENT /NORESTART /NOCANCEL /SP- /SUPPRESSMSGBOXES' }
    'Microsoft.VisualStudio.2022.BuildTools' = @{ Label = 'Visual Studio 2022 Build Tools (C++)'; Url = 'https://aka.ms/vs/17/release/vs_BuildTools.exe'
        Sha256 = ''; Kind = 'exe'; Args = $VsWorkload }
}

function Refresh-Path {
    $m = [Environment]::GetEnvironmentVariable('Path', 'Machine'); $u = [Environment]::GetEnvironmentVariable('Path', 'User')
    $env:Path = "$m;$u;$env:LOCALAPPDATA\Microsoft\WinGet\Links;$env:ProgramFiles\WinGet\Links;C:\Program Files\LLVM\bin;C:\Program Files\CMake\bin"
}
function Find-Exe([string]$name) {
    $c = Get-Command $name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($c) { return $c.Source }; return $null
}
function First-Version([string]$text) {
    if ($text -match '(\d+)\.(\d+)(?:\.(\d+))?') { return $Matches[0] }; return ''
}
function Ver([string]$v) { try { return [version]($v -replace '^(\d+\.\d+)$', '$1.0') } catch { return [version]'0.0' } }
function Find-VsWhere { $p = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"; if (Test-Path $p) { return $p }; return $null }
function Find-WinSdk {
    try { $root = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Microsoft SDKs\Windows\v10.0' -ErrorAction Stop).InstallationFolder } catch { return $null }
    if (-not $root) { return $null }
    $rc = Get-ChildItem (Join-Path $root 'bin\*\x64\rc.exe') -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1
    if ($rc) { return $rc.FullName }; return $null
}

function Get-ToolStatus {
    Refresh-Path
    $r = New-Object System.Collections.ArrayList
    function Add($key, $status, $ver, $path, $label, $id) { [void]$r.Add(('{0}|{1}|{2}|{3}|{4}|{5}' -f $key, $status, $ver, $path, $label, $id)) }

    # Python: same test as setup.ps1 (py or python on PATH, 3.10 or newer; the Store alias stub fails it)
    $py = $null; $pyv = ''
    foreach ($c in @('py', 'python')) {
        $exe = Find-Exe $c
        if (-not $exe) { continue }
        $v = & $exe -c "import sys; print('%d.%d.%d' % sys.version_info[:3])" 2>$null
        if ($LASTEXITCODE -eq 0 -and "$v".Trim() -match '^\d+\.\d+\.\d+$') {
            $pyv = "$v".Trim()
            if ((Ver $pyv) -ge [version]'3.10') { $py = $exe; break }
        }
    }
    if ($py) { Add 'python' 'ok' $pyv $py 'Python 3.10+' '' }
    elseif ($pyv) { Add 'python' 'old' $pyv '' 'Python 3.10+' 'Python.Python.3.12' }
    else { Add 'python' 'missing' '' '' 'Python 3.10+' 'Python.Python.3.12' }

    $exe = Find-Exe 'cmake'
    if ($exe) {
        $v = First-Version "$(& $exe --version 2>$null | Select-Object -First 1)"
        # 4.2+ gives the lifted-PPU files their own RAM-limited job pool; 3.20-4.1 still builds (more slowly)
        $st = if ((Ver $v) -ge [version]'4.2') { 'ok' } else { 'old' }
        Add 'cmake' $st $v $exe 'CMake 4.2+' 'Kitware.CMake'
    } else { Add 'cmake' 'missing' '' '' 'CMake 4.2+' 'Kitware.CMake' }

    $exe = Find-Exe 'ninja'
    if ($exe) {
        $v = First-Version "$(& $exe --version 2>$null | Select-Object -First 1)"
        $st = if ((Ver $v) -ge [version]'1.10') { 'ok' } else { 'old' }
        Add 'ninja' $st $v $exe 'Ninja 1.10+' 'Ninja-build.Ninja'
    } else { Add 'ninja' 'missing' '' '' 'Ninja 1.10+' 'Ninja-build.Ninja' }

    $exe = Find-Exe 'clang-cl'
    if ($exe) {
        $v = First-Version "$(& $exe --version 2>$null | Select-Object -First 1)"
        $st = if ((Ver $v) -ge [version]'18.0') { 'ok' } else { 'old' }
        Add 'clang' $st $v $exe 'LLVM clang-cl 18+' 'LLVM.LLVM'
    } else { Add 'clang' 'missing' '' '' 'LLVM clang-cl 18+' 'LLVM.LLVM' }

    if ($NeedGit) {
        $exe = Find-Exe 'git'
        if ($exe) { Add 'git' 'ok' (First-Version "$(& $exe --version 2>$null)") $exe 'Git' 'Git.Git' }
        else { Add 'git' 'missing' '' '' 'Git' 'Git.Git' }
    }

    $vw = Find-VsWhere; $vs = $null; $vsv = ''
    if ($vw) {
        $vs = & $vw -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null | Select-Object -First 1
        if ($vs) { $vsv = "$(& $vw -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion 2>$null | Select-Object -First 1)".Trim() }
    }
    if (-not $vs) { Add 'vs' 'missing' '' '' 'Visual Studio Build Tools (C++)' 'Microsoft.VisualStudio.2022.BuildTools' }
    elseif (-not (Find-WinSdk)) { Add 'vs' 'nosdk' $vsv $vs 'Visual Studio Build Tools (C++) + Windows SDK' 'VSModify' }
    else { Add 'vs' 'ok' $vsv $vs 'Visual Studio Build Tools (C++) + Windows SDK' '' }

    $exe = Find-Exe 'winget'
    if ($exe) { Add 'winget' 'ok' (First-Version "$(& $exe --version 2>$null)") $exe 'winget (App Installer)' '' }
    else { Add 'winget' 'missing' '' '' 'winget (App Installer)' '' }
    if (Test-Path (Join-Path $env:SystemRoot 'System32\vulkan-1.dll')) { Add 'vulkan' 'ok' '' (Join-Path $env:SystemRoot 'System32\vulkan-1.dll') 'Vulkan runtime (GPU driver)' '' }
    else { Add 'vulkan' 'missing' '' '' 'Vulkan runtime (GPU driver)' '' }
    return $r
}

if ($Check) {
    $lines = Get-ToolStatus
    if ($Out) { [IO.File]::WriteAllLines($Out, [string[]]$lines) } else { $lines }
    exit 0
}

if (-not $Install) { Write-Host 'Use -Check or -Install -Ids <ids>.'; exit 2 }

# ---------------------------------------------------------------- install (elevated)
if ($Log) { try { New-Item -ItemType Directory -Force -Path (Split-Path $Log) | Out-Null } catch {} }
function Say([string]$t) {
    $line = '{0:HH:mm:ss} {1}' -f (Get-Date), $t
    Write-Host $line
    if ($Log) { Add-Content -LiteralPath $Log -Value $line -Encoding UTF8 }
}
$results = New-Object System.Collections.ArrayList
function Result($id, $ok, $code, $note) { [void]$results.Add(('{0}|{1}|{2}|{3}' -f $id, $(if ($ok) { 'ok' } else { 'fail' }), $code, $note)) }
$wingetOk = [bool](Find-Exe 'winget') -and -not $NoWinget
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
Say ("Installing: {0}  (winget: {1}, administrator: {2}{3})" -f ($Ids -join ', '), $wingetOk, $isAdmin, $(if ($DryRun) { ', DRY RUN' } else { '' }))
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
$dl = Join-Path $Port 'logs\prereq_downloads'

function Run([string]$file, [string]$argline) {
    Say "  > $file $argline"
    if ($DryRun) { return 0 }
    $p = Start-Process -FilePath $file -ArgumentList $argline -Wait -PassThru -WindowStyle Hidden
    return $p.ExitCode
}
function Winget-Code-Ok([int]$c) {
    # 0, already installed (0x8A150061), no newer version (0x8A15002B), reboot needed (3010 / 0x8A150109)
    return ($c -eq 0 -or $c -eq 3010 -or $c -eq -1978335135 -or $c -eq -1978335189 -or $c -eq -1978334967)
}
function Install-Winget([string]$id) {
    $base = "install --id $id -e --source winget --accept-package-agreements --accept-source-agreements --silent --disable-interactivity"
    if ($id -eq 'Microsoft.VisualStudio.2022.BuildTools') { $base += " --override `"$VsWorkload`"" }
    $c = Run 'winget' "$base --scope machine"
    if (-not (Winget-Code-Ok $c)) {
        Say "  winget exit code $c with --scope machine; trying again without it"
        $c = Run 'winget' $base
    }
    return $c
}
function Install-Direct([string]$id) {
    $p = $Pkgs[$id]
    New-Item -ItemType Directory -Force -Path $dl | Out-Null
    $name = if ($id -eq 'Microsoft.VisualStudio.2022.BuildTools') { 'vs_BuildTools.exe' } else { Split-Path $p.Url -Leaf }
    $file = Join-Path $dl $name
    Say "  downloading $($p.Url)"
    if (-not $DryRun) {
        Invoke-WebRequest -Uri $p.Url -OutFile $file -UseBasicParsing
        if ($p.Sha256) {
            $h = (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash
            if ($h -ne $p.Sha256.ToUpper()) { Remove-Item -LiteralPath $file -Force; throw "SHA-256 mismatch for $name (got $h)" }
        } else {
            $sig = Get-AuthenticodeSignature -LiteralPath $file
            if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') { Remove-Item -LiteralPath $file -Force; throw "$name is not signed by Microsoft ($($sig.Status))" }
        }
    }
    switch ($p.Kind) {
        'msi' { return (Run "$env:SystemRoot\System32\msiexec.exe" "/i `"$file`" /qn /norestart ALLUSERS=1 $($p.Args)") }
        'exe' { return (Run $file $p.Args) }
        'zip' {
            $dest = Join-Path $env:ProgramFiles 'Ninja'
            Say "  extracting to $dest and adding it to the system PATH"
            if (-not $DryRun) {
                New-Item -ItemType Directory -Force -Path $dest | Out-Null
                Expand-Archive -LiteralPath $file -DestinationPath $dest -Force
                $m = [Environment]::GetEnvironmentVariable('Path', 'Machine')
                if (($m -split ';') -notcontains $dest) { [Environment]::SetEnvironmentVariable('Path', "$m;$dest", 'Machine') }
            }
            return 0
        }
    }
}
function Modify-VS {
    $vw = Find-VsWhere
    $vs = if ($vw) { & $vw -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1 }
    $setup = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\setup.exe"
    if (-not $vs -or -not (Test-Path $setup)) { throw 'Visual Studio Installer not found' }
    return (Run $setup "modify --installPath `"$vs`" $VsWorkload")
}

# VS first: it is the biggest and the others never depend on it
$order = @('Microsoft.VisualStudio.2022.BuildTools', 'VSModify') + @($Pkgs.Keys | Where-Object { $_ -ne 'Microsoft.VisualStudio.2022.BuildTools' })
foreach ($id in $order) {
    if ($Ids -notcontains $id) { continue }
    $label = if ($id -eq 'VSModify') { 'Windows SDK (Visual Studio Installer, modify)' } else { $Pkgs[$id].Label }
    Say "== $label"
    try {
        if ($id -eq 'VSModify') { $c = Modify-VS }
        elseif ($wingetOk) {
            $c = Install-Winget $id
            # winget is there but broken on some PCs (source errors, store policy): fall back to the pinned download
            if (-not (Winget-Code-Ok $c)) {
                Say "  winget failed (exit code $c); downloading the official installer instead"
                $c = Install-Direct $id
            }
        }
        else { $c = Install-Direct $id }
        $ok = Winget-Code-Ok $c
        Say $(if ($ok) { "  done (exit code $c)" } else { "  FAILED (exit code $c)" })
        Result $id $ok $c $(if ($c -eq 3010 -or $c -eq -1978334967) { 'reboot' } else { '' })
    } catch {
        Say "  FAILED: $_"
        Result $id $false -1 "$_"
    }
}
if (-not $DryRun -and (Test-Path $dl)) { Remove-Item -LiteralPath $dl -Recurse -Force -ErrorAction SilentlyContinue }
[void]$results.Add('done')
if ($Out) { [IO.File]::WriteAllLines($Out, [string[]]$results) } else { $results }
Say 'Finished.'
exit 0
