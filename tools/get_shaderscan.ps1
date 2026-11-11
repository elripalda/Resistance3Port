# Gets tools\shaderscan.exe, the launcher's optional offline shader-cache seeding tool, from its own repository:
#   https://github.com/miroslavdanutalexandru-alt/shaderscan (GPL-2.0-only; source, licence and notices there)
# The release and its SHA-256 are pinned in tools\shaderscan.version. Called by build.bat (and so by Setup).
#
#   get_shaderscan.ps1 [-Url <url>] [-Sha256 <hex>] [-Dest <exe>]
#     -Url / -Sha256   override the pin (tests, mirrors); file:// URLs and plain local paths work too
#   env SHADERSCAN_EXE=<path>   use that exe instead (copied to tools\shaderscan.exe without a hash check,
#                               e.g. your own build of the shaderscan repo)
#
# Exit codes: 0 tools\shaderscan.exe is in place (already current, downloaded or copied), 1 skipped (offline or
# download failed; the launcher works without it), 2 the download did not match the pinned SHA-256 (rejected).
param([string]$Url = '', [string]$Sha256 = '', [string]$Dest = '')
$ErrorActionPreference = 'Stop'
$tools = $PSScriptRoot
$pinFile = Join-Path $tools 'shaderscan.version'
if (-not $Dest) { $Dest = Join-Path $tools 'shaderscan.exe' }

$pin = @{}
if (Test-Path -LiteralPath $pinFile) {
    foreach ($l in Get-Content -LiteralPath $pinFile) {
        if ($l -match '^\s*([A-Za-z0-9_]+)\s*=\s*(.*?)\s*$') { $pin[$Matches[1]] = $Matches[2] }
    }
}
if (-not $Url) { $Url = $pin['url'] }
if (-not $Sha256) { $Sha256 = $pin['sha256'] }
$ver = if ($pin['version']) { $pin['version'] } else { '(unpinned)' }
function Hash($p) { (Get-FileHash -Algorithm SHA256 -LiteralPath $p).Hash.ToUpperInvariant() }

if ($env:SHADERSCAN_EXE) {
    if (-not (Test-Path -LiteralPath $env:SHADERSCAN_EXE -PathType Leaf)) {
        Write-Host "shaderscan: SHADERSCAN_EXE=$($env:SHADERSCAN_EXE) does not exist - skipped. The launcher works without it."
        exit 1
    }
    $src = (Resolve-Path -LiteralPath $env:SHADERSCAN_EXE).Path
    if ($src -ne [IO.Path]::GetFullPath($Dest)) { Copy-Item -LiteralPath $src -Destination $Dest -Force }
    Write-Host "shaderscan: using SHADERSCAN_EXE=$src (not checked against the pinned $ver)"
    exit 0
}
if (-not $Url -or $Sha256 -notmatch '^[0-9A-Fa-f]{64}$') {
    Write-Host "shaderscan: no valid pin in $pinFile - skipped. The launcher works without it."
    exit 1
}
$Sha256 = $Sha256.ToUpperInvariant()
if ((Test-Path -LiteralPath $Dest) -and (Hash $Dest) -eq $Sha256) {
    Write-Host "shaderscan: tools\shaderscan.exe is $ver (SHA-256 OK)"
    exit 0
}

$tmp = "$Dest.download"
Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
Write-Host "shaderscan: fetching $ver from $Url ..."
try {
    $u = $null
    if ([Uri]::TryCreate($Url, [UriKind]::Absolute, [ref]$u) -and $u.Scheme -in 'http', 'https') {
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        $pp = $ProgressPreference; $ProgressPreference = 'SilentlyContinue'
        try { Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $tmp -TimeoutSec 60 } finally { $ProgressPreference = $pp }
    } else {
        $local = if ($u -and $u.IsFile) { $u.LocalPath } else { $Url }
        Copy-Item -LiteralPath $local -Destination $tmp -Force
    }
} catch {
    Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
    $keep = if (Test-Path -LiteralPath $Dest) { ' Keeping the tools\shaderscan.exe that is already there.' } else { '' }
    Write-Host "shaderscan: could not download it ($($_.Exception.Message)) - skipped (offline?).$keep The launcher works without it."
    exit 1
}
$got = Hash $tmp
if ($got -ne $Sha256) {
    Remove-Item -LiteralPath $tmp -Force
    Write-Host "shaderscan: REJECTED - the download's SHA-256 is $got, expected $Sha256 (tools\shaderscan.version). Nothing was installed."
    exit 2
}
try { Move-Item -LiteralPath $tmp -Destination $Dest -Force }
catch {
    Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
    Write-Host "shaderscan: could not replace tools\shaderscan.exe ($($_.Exception.Message)); is it running? Skipped."
    exit 1
}
Write-Host "shaderscan: tools\shaderscan.exe is $ver (downloaded, SHA-256 OK)"
exit 0
