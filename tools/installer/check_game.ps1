# Command-line front end of gamefiles.ps1, used by the installer.
#   check_game.ps1 -Game <iso or folder> [-Dest <port>\gamefiles] -Out result.txt   -> key=value lines
#   check_game.ps1 -Find -Out list.txt                                              -> status|titleid|path|title lines
param(
    [string]$Game = '',
    [switch]$Find,
    [string]$Dest = '',
    [string]$PortDir = '',
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$ProfilePath = ''
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $here 'gamefiles.ps1')
if (-not $ProfilePath) { $ProfilePath = Join-Path $here 'game_profile.psd1' }
$utf8 = New-Object Text.UTF8Encoding $false
try {
    $Prof = Import-GameProfile $ProfilePath
    if ($Find) {
        $lines = @(Find-GameSources $Prof $PortDir | ForEach-Object { "$($_.Status)|$($_.TitleId)|$($_.Path)|$($_.Title)" })
        [IO.File]::WriteAllLines($Out, [string[]]$lines, $utf8)
        exit 0
    }
    $c = Test-GameSource $Prof $Game
    if ($Dest) { Test-GameSpace $Prof $c $Dest -Move }   # Setup.bat checks again if the player copies instead of moving
    Write-CheckFile $c $Out
    exit 0
} catch {
    [IO.File]::WriteAllLines($Out, [string[]]@('status=skip', 'code=SCRIPT', "problem=The game check could not run: $($_.Exception.Message)"), $utf8)
    exit 2
}
