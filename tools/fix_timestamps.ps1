# Files dated in the future break the build: Ninja sees CMakeLists.txt newer than build.ninja and
# regenerates forever ("still dirty after 100 tries"). Zip and installer times are stored as local
# time without a time zone, so a release packed a few hours "ahead" of the player's time zone
# arrives with future dates. This sets such files to the current time.
param([string]$Root = (Split-Path -Parent $PSScriptRoot))

$skip = @('gamefiles', 'game', 'gamedata', 'shadercache', '.git')
$limit = (Get-Date).AddMinutes(1)
$now = Get-Date
$fixed = 0
function Walk([string]$dir, [bool]$top) {
    foreach ($f in [IO.Directory]::EnumerateFiles($dir)) {
        try {
            if ([IO.File]::GetLastWriteTime($f) -gt $limit) { [IO.File]::SetLastWriteTime($f, $now); $script:fixed++ }
        } catch { }
    }
    foreach ($d in [IO.Directory]::EnumerateDirectories($dir)) {
        if ($top -and $skip -contains (Split-Path -Leaf $d)) { continue }
        if (([IO.File]::GetAttributes($d) -band [IO.FileAttributes]::ReparsePoint) -ne 0) { continue }
        Walk $d $false
    }
}
Walk $Root $true
if ($fixed) { Write-Host "Fixed $fixed file(s) dated in the future (time zone of the download); the build would loop otherwise." }
