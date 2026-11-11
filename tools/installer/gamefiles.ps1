# Shared game-copy handling for the port installers and setup.ps1 (the same file in every port).
# Dot-source it, then load the port's profile:
#     . "$Port\tools\installer\gamefiles.ps1"
#     $Prof = Import-GameProfile "$Port\tools\installer\game_profile.psd1"
#
#   Find-GameSources $Prof            - looks for copies of the game without a file picker (common folders,
#                                    RPCS3's game list, EmuDeck folders under Proton)
#   Test-GameSource  $Prof <path>     - checks an .iso / game folder / file in a game folder and explains
#                                    any problem (Status ok / warn / error, Problem, Fix)
#   Copy-GameSource  $Prof $check <destination folder> [-Move]
#                                  - copies the disc files (mounts an .iso; reads it directly when
#                                    Windows can't mount it, e.g. under Wine)
# Disc images are read with a small ISO 9660 reader, so checking an .iso needs no mounting.
# PowerShell 5.1 compatible.

Set-StrictMode -Off

function Import-GameProfile([string]$Path) {
    # data only (no commands); not Import-PowerShellDataFile, which is missing when PowerShell 7 module paths leak in
    $sb = [scriptblock]::Create([IO.File]::ReadAllText((Resolve-Path -LiteralPath $Path).ProviderPath))
    $sb.CheckRestrictedLanguage([string[]]@(), [string[]]@(), $false)
    $p = & $sb
    if (-not $p.TitleId) { throw "Bad game profile: $Path" }
    return $p
}

# ------------------------------------------------------------------ PARAM.SFO
function Read-Sfo([byte[]]$b) {
    $r = @{}
    if (-not $b -or $b.Length -lt 20 -or $b[0] -ne 0 -or $b[1] -ne 0x50 -or $b[2] -ne 0x53 -or $b[3] -ne 0x46) { return $null }
    $keyTab = [BitConverter]::ToUInt32($b, 8); $dataTab = [BitConverter]::ToUInt32($b, 12); $n = [BitConverter]::ToUInt32($b, 16)
    for ($i = 0; $i -lt $n; $i++) {
        $e = 20 + 16 * $i
        if ($e + 16 -gt $b.Length) { break }
        $ko = $keyTab + [BitConverter]::ToUInt16($b, $e); $fmt = [BitConverter]::ToUInt16($b, $e + 2)
        $len = [BitConverter]::ToUInt32($b, $e + 4); $do = $dataTab + [BitConverter]::ToUInt32($b, $e + 12)
        if ($ko -ge $b.Length -or $do + $len -gt $b.Length) { continue }
        $ke = $ko; while ($ke -lt $b.Length -and $b[$ke] -ne 0) { $ke++ }
        $key = [Text.Encoding]::ASCII.GetString($b, $ko, $ke - $ko)
        if ($fmt -eq 0x0404) { $r[$key] = [BitConverter]::ToUInt32($b, $do) }
        else { $s = [Text.Encoding]::UTF8.GetString($b, $do, $len); $z = $s.IndexOf([char]0); if ($z -ge 0) { $s = $s.Substring(0, $z) }; $r[$key] = $s }
    }
    return $r
}

# ------------------------------------------------------------------ ISO 9660 reader
function Open-Iso([string]$Path) {
    $fs = [IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
    $iso = @{ Stream = $fs; Path = $Path; Root = $null; Joliet = $false }
    $buf = New-Object byte[] 2048
    for ($s = 16; $s -lt 64; $s++) {
        [void]$fs.Seek([int64]$s * 2048, 'Begin')
        if ($fs.Read($buf, 0, 2048) -ne 2048) { break }
        if ([Text.Encoding]::ASCII.GetString($buf, 1, 5) -ne 'CD001') { break }
        $type = $buf[0]
        if ($type -eq 255) { break }
        $rec = @{ Lba = [BitConverter]::ToUInt32($buf, 156 + 2); Size = [int64][BitConverter]::ToUInt32($buf, 156 + 10); Dir = $true }
        if ($type -eq 1 -and -not $iso.Root) { $iso.Root = $rec }
        if ($type -eq 2 -and $buf[88] -eq 0x25 -and $buf[89] -eq 0x2F -and ($buf[90] -eq 0x40 -or $buf[90] -eq 0x43 -or $buf[90] -eq 0x45)) {
            $iso.Root = $rec; $iso.Joliet = $true
        }
    }
    if (-not $iso.Root) { $fs.Close(); return $null }
    return $iso
}
function Close-Iso($iso) { if ($iso -and $iso.Stream) { $iso.Stream.Close() } }

# Entries of a directory: Name, Dir, Extents (list of @(lba, size)), Size.
function Get-IsoDir($iso, $dir) {
    $out = [ordered]@{}
    $data = New-Object byte[] $dir.Size
    [void]$iso.Stream.Seek([int64]$dir.Lba * 2048, 'Begin')
    $got = 0; while ($got -lt $data.Length) { $n = $iso.Stream.Read($data, $got, $data.Length - $got); if ($n -le 0) { break }; $got += $n }
    $pos = 0; $pending = $null
    while ($pos -lt $got) {
        $len = $data[$pos]
        if ($len -eq 0) { $pos = ([math]::Floor($pos / 2048) + 1) * 2048; continue }
        if ($pos + 33 -gt $got) { break }
        $lba = [BitConverter]::ToUInt32($data, $pos + 2); $size = [int64][BitConverter]::ToUInt32($data, $pos + 10)
        $flags = $data[$pos + 25]; $nl = $data[$pos + 32]
        if ($nl -eq 1 -and ($data[$pos + 33] -eq 0 -or $data[$pos + 33] -eq 1)) { $pos += $len; continue }
        if ($iso.Joliet) { $name = [Text.Encoding]::BigEndianUnicode.GetString($data, $pos + 33, $nl) }
        else { $name = [Text.Encoding]::ASCII.GetString($data, $pos + 33, $nl) }
        $name = ($name -replace ';\d+$', '').TrimEnd('.')
        if ($pending -and $pending.Name -eq $name) { $e = $pending }
        else { $e = @{ Name = $name; Dir = [bool]($flags -band 2); Extents = New-Object System.Collections.ArrayList; Size = [int64]0 } }
        [void]$e.Extents.Add(@($lba, $size)); $e.Size += $size
        if ($flags -band 0x80) { $pending = $e } else { $pending = $null; $out[$name.ToUpperInvariant()] = $e }
        $pos += $len
    }
    return $out
}
function Get-IsoEntry($iso, [string]$RelPath) {
    $cur = @{ Dir = $true; Extents = @(, @($iso.Root.Lba, $iso.Root.Size)) }
    foreach ($part in ($RelPath -split '[\\/]' | Where-Object { $_ })) {
        if (-not $cur.Dir) { return $null }
        $d = Get-IsoDir $iso @{ Lba = $cur.Extents[0][0]; Size = $cur.Extents[0][1] }
        $cur = $d[$part.ToUpperInvariant()]
        if (-not $cur) { return $null }
    }
    return $cur
}
function Read-IsoBytes($iso, $entry, [int]$Max = 0) {
    $want = $entry.Size; if ($Max -gt 0 -and $Max -lt $want) { $want = $Max }
    $buf = New-Object byte[] $want; $at = 0
    foreach ($x in $entry.Extents) {
        if ($at -ge $want) { break }
        [void]$iso.Stream.Seek([int64]$x[0] * 2048, 'Begin')
        $left = [math]::Min([int64]$x[1], [int64]($want - $at))
        while ($left -gt 0) { $n = $iso.Stream.Read($buf, $at, $left); if ($n -le 0) { break }; $at += $n; $left -= $n }
    }
    return $buf
}
function Get-IsoSha1($iso, $entry) {
    $sha = [Security.Cryptography.SHA1]::Create(); $buf = New-Object byte[] (4MB)
    foreach ($x in $entry.Extents) {
        [void]$iso.Stream.Seek([int64]$x[0] * 2048, 'Begin'); $left = [int64]$x[1]
        while ($left -gt 0) { $n = $iso.Stream.Read($buf, 0, [int][math]::Min($buf.Length, $left)); if ($n -le 0) { break }; [void]$sha.TransformBlock($buf, 0, $n, $null, 0); $left -= $n }
    }
    [void]$sha.TransformFinalBlock($buf, 0, 0)
    return ([BitConverter]::ToString($sha.Hash) -replace '-', '').ToLower()
}
# Copies the whole image (or the subfolder $Base, e.g. a dump kept one folder down) to $Dest.
function Export-IsoTree($iso, $dirEntry, [string]$Dest, [ref]$Done, [int64]$Total) {
    New-Item -ItemType Directory -Force -Path $Dest | Out-Null
    $d = Get-IsoDir $iso @{ Lba = $dirEntry.Extents[0][0]; Size = $dirEntry.Extents[0][1] }
    $buf = New-Object byte[] (4MB)
    foreach ($e in $d.Values) {
        $target = Join-Path $Dest $e.Name
        if ($e.Dir) { Export-IsoTree $iso $e $target $Done $Total; continue }
        $out = [IO.File]::Create($target)
        try {
            foreach ($x in $e.Extents) {
                [void]$iso.Stream.Seek([int64]$x[0] * 2048, 'Begin'); $left = [int64]$x[1]
                while ($left -gt 0) {
                    $n = $iso.Stream.Read($buf, 0, [int][math]::Min($buf.Length, $left)); if ($n -le 0) { throw "Read error in the disc image at $($e.Name)." }
                    $out.Write($buf, 0, $n); $left -= $n; $Done.Value += $n
                }
            }
        } finally { $out.Close() }
        if ($Total -gt 0) { Write-Progress -Activity 'Copying the game files from the disc image' -Status $e.Name -PercentComplete ([math]::Min(100, [int](100 * $Done.Value / $Total))) }
    }
}

# ------------------------------------------------------------------ helpers
function Get-RegionName([string]$id) {
    if ($id.Length -lt 4) { return 'another region' }
    switch -regex ($id.Substring(0, 4)) {
        '^(BCUS|BLUS|NPUA|NPUB)$' { return 'North America (US)' }
        '^(BCES|BLES|NPEA|NPEB)$' { return 'Europe' }
        '^(BCJS|BLJS|BLJM|BCJB|NPJA|NPJB)$' { return 'Japan' }
        '^(BCAS|BLAS|NPHA|NPHB)$' { return 'Asia' }
        '^(BCKS|BLKS)$' { return 'Korea' }
        default { return 'another region' }
    }
}
function New-Check([string]$Path) {
    return [ordered]@{ Path = $Path; Status = 'ok'; Code = 'OK'; Problem = ''; Fix = ''; Kind = ''; Root = ''; Sub = '';
                       Title = ''; TitleId = ''; Version = ''; Bytes = [int64]0; Notes = New-Object System.Collections.ArrayList }
}
function Set-Bad($c, [string]$status, [string]$code, [string]$problem, [string]$fix) {
    if ($c.Status -eq 'error') { return }
    if ($status -eq 'warn' -and $c.Status -eq 'warn') { [void]$c.Notes.Add("$problem $fix"); return }
    $c.Status = $status; $c.Code = $code; $c.Problem = $problem.Replace('PS3_GAME/', 'PS3_GAME\').Replace('USRDIR/', 'USRDIR\'); $c.Fix = $fix
}
function Format-GB([int64]$b) { return ('{0:N1} GB' -f ($b / 1GB)) }

# ------------------------------------------------------------------ check one source
# $Path may be an .iso, a game folder (the one with PS3_GAME), PS3_GAME itself, or a file inside it.
function Test-GameSource($Prof, [string]$Path) {
    $Path = "$Path".Trim().Trim('"').Trim()
    $c = New-Check $Path
    $name = $Prof.GameName; $want = "$($Prof.GameName) ($($Prof.TitleId), $($Prof.RegionLabel) disc)"
    if (-not $Path -or -not (Test-Path -LiteralPath $Path)) {
        Set-Bad $c 'error' 'NOT_FOUND' "Nothing was found at this path." "Check the path, or pick the .iso or the game folder again."; return $c
    }
    $item = Get-Item -LiteralPath $Path -Force
    if ($item -is [IO.FileInfo]) {
        $ext = $item.Extension.ToLowerInvariant()
        if ($ext -in '.zip', '.7z', '.rar', '.gz', '.tar') {
            Set-Bad $c 'error' 'ARCHIVE' "This is a compressed archive ($ext), not the game itself." "Extract it first (for example with 7-Zip), then pick the .iso or the game folder inside."; return $c
        }
        if ($ext -eq '.pkg') {
            Set-Bad $c 'error' 'PKG' "This is a PlayStation Store package (.pkg), such as a game update." "Pick the disc copy instead: the .iso, or the folder that contains PS3_GAME. Game updates are not needed."; return $c
        }
        if ($ext -in '.bin', '.cue', '.img', '.mdf', '.nrg') {
            if ($item.Name -notmatch '^EBOOT\.BIN$') {
                Set-Bad $c 'error' 'FORMAT' "This disc image format ($ext) is not supported." "Use an .iso file or a game folder (the same kind RPCS3 uses)."; return $c
            }
        }
        if ($ext -eq '.iso') { return (Test-GameIso $Prof $c $item) }
        $Path = $item.DirectoryName
    }
    return (Test-GameFolder $Prof $c $Path)
}

function Find-DumpRoot([string]$p) {
    # walk up from a file or folder inside a dump to the folder that contains PS3_GAME
    $q = $p
    for ($i = 0; $i -lt 4 -and $q; $i++) {
        if (Test-Path -LiteralPath (Join-Path $q 'PS3_GAME\PARAM.SFO')) { return $q }
        if ((Split-Path -Leaf $q) -eq 'PS3_GAME' -and (Test-Path -LiteralPath (Join-Path $q 'PARAM.SFO'))) { return (Split-Path -Parent $q) }
        $q = Split-Path -Parent $q
    }
    # one folder down (<folder>\<game name>\PS3_GAME)
    $sub = Get-ChildItem -LiteralPath $p -Directory -ErrorAction SilentlyContinue | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'PS3_GAME\PARAM.SFO') } | Select-Object -First 1
    if ($sub) { return $sub.FullName }
    return $null
}

function Test-SfoAndIds($Prof, $c, $sfo, [string]$where) {
    if (-not $sfo) {
        Set-Bad $c 'error' 'ENCRYPTED' "The game files can't be read: PARAM.SFO is not valid. This usually means an encrypted disc image." `
            "Use a decrypted .iso or a game folder. An encrypted image (for example a Redump .iso with a .dkey file) can be decrypted first with PS3Dec and its disc key."
        return $false
    }
    $c.Title = "$($sfo.TITLE)" -replace '[\r\n]+', ' ' -replace '[\u2122\u00AE]', ''; $c.TitleId = "$($sfo.TITLE_ID)"; $c.Version = "$($sfo.APP_VER)"
    $cat = "$($sfo.CATEGORY)"
    if ($cat -eq 'GP' -or $where -match '[\\/]dev_hdd0[\\/]game[\\/]') {
        Set-Bad $c 'error' 'UPDATE_FOLDER' "This is a game update or game data folder ($($c.TitleId), from dev_hdd0\game), not the disc." `
            "Pick the disc copy: the .iso, or the folder that contains PS3_GAME and PS3_DISC.SFB."
        return $false
    }
    if ($c.TitleId -eq $Prof.TitleId) { return $true }
    $sameGame = $c.Title -match $Prof.TitleMatch
    if ($c.TitleId -match '^NP') {
        Set-Bad $c 'error' 'PSN' "This is the PlayStation Store version of $($c.Title) ($($c.TitleId))." "Only the disc version is supported: $want."
    } elseif ($sameGame) {
        Set-Bad $c 'error' 'WRONG_REGION' "This is $($c.Title) from $(Get-RegionName $c.TitleId) ($($c.TitleId))." "The port supports only $want for now. Other regions are not supported yet."
    } else {
        Set-Bad $c 'error' 'WRONG_GAME' "This is $(if ($c.Title) { $c.Title } else { 'another game' }) ($($c.TitleId)), not $($Prof.GameName)." "Pick your copy of $want."
    }
    return $false
}

function Test-ProgramFile($Prof, $c, [string]$rel, [byte[]]$head, [scriptblock]$hash) {
    if ($head.Length -ge 4 -and $head[0] -eq 0x7F -and $head[1] -eq 0x45 -and $head[2] -eq 0x4C -and $head[3] -eq 0x46) {
        Set-Bad $c 'error' 'DECRYPTED' "$rel in this copy has been decrypted or replaced (it is a plain ELF file)." "Use the original, unmodified file from the disc. Setup decrypts it itself."
        return
    }
    if ($head.Length -lt 4 -or [Text.Encoding]::ASCII.GetString($head, 0, 3) -ne 'SCE') {
        Set-Bad $c 'error' 'ENCRYPTED' "$rel can't be read (it does not start like a PS3 program file). This usually means an encrypted disc image." `
            "Use a decrypted .iso or a game folder. An encrypted image (for example a Redump .iso with a .dkey file) can be decrypted first with PS3Dec and its disc key."
        return
    }
    $known = $Prof.Sha1[$rel]
    if ($known -and $hash) {
        $h = & $hash
        if ($h -ne $known) {
            Set-Bad $c 'warn' 'MODIFIED' "$rel is different from the known file of $($Prof.TitleId) version $($Prof.AppVer). The copy may be modified or a different release." `
                "Use an original, unmodified copy if you can. Setup checks the file again after decrypting it and stops if it is not the supported version."
        }
    }
}

function Test-GameFolder($Prof, $c, [string]$Path) {
    $c.Kind = 'folder'
    $root = Find-DumpRoot $Path
    if (-not $root) {
        if (Test-Path -LiteralPath (Join-Path $Path 'USRDIR\EBOOT.BIN')) {
            $sfo = $null; try { $sfo = Read-Sfo ([IO.File]::ReadAllBytes((Join-Path $Path 'PARAM.SFO'))) } catch {}
            if ($sfo -and "$($sfo.TITLE_ID)" -match '^NP' -or $sfo.CATEGORY -eq 'HG') {
                [void](Test-SfoAndIds $Prof $c $sfo $Path)
                if ($c.Status -ne 'error') { Set-Bad $c 'error' 'PSN' "This is a PlayStation Store or installed copy, not a disc copy." "Pick the disc copy: $want." }
                return $c
            }
        }
        if (Get-ChildItem -LiteralPath $Path -File -Filter *.iso -ErrorAction SilentlyContinue | Select-Object -First 1) {
            Set-Bad $c 'error' 'NOT_PS3' "This folder has .iso files but no unpacked game." "Pick the .iso file itself."; return $c
        }
        Set-Bad $c 'error' 'NOT_PS3' "No PS3 game was found here (there is no PS3_GAME\PARAM.SFO)." "Pick the .iso, or the folder that contains PS3_GAME and PS3_DISC.SFB."
        return $c
    }
    $c.Root = $root
    $sfo = $null; try { $sfo = Read-Sfo ([IO.File]::ReadAllBytes((Join-Path $root 'PS3_GAME\PARAM.SFO'))) } catch {}
    if (-not (Test-SfoAndIds $Prof $c $sfo $root)) { return $c }
    $missing = @($Prof.Required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $root $_)) })
    if ($missing.Count) {
        Set-Bad $c 'error' 'MISSING' "Some game files are missing: $($missing -join ', ')." "The copy is incomplete. Copy or dump the game again, then try again."; return $c
    }
    foreach ($rel in $Prof.Program) {
        $full = Join-Path $root $rel; $head = New-Object byte[] 4
        try { $fs = [IO.File]::OpenRead($full); [void]$fs.Read($head, 0, 4); $fs.Close() } catch {}
        Test-ProgramFile $Prof $c $rel $head { $fs = [IO.File]::OpenRead($full); try { ([BitConverter]::ToString([Security.Cryptography.SHA1]::Create().ComputeHash($fs)) -replace '-', '').ToLower() } finally { $fs.Close() } }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $root 'PS3_DISC.SFB'))) { [void]$c.Notes.Add('PS3_DISC.SFB is missing (not needed by the port).') }
    $c.Bytes = [int64](Get-ChildItem -LiteralPath (Join-Path $root 'PS3_GAME') -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
    if ($Prof.MinBytes -and $c.Bytes -lt $Prof.MinBytes) {
        Set-Bad $c 'warn' 'SMALL' "The game files are smaller than expected ($(Format-GB $c.Bytes) of about $($Prof.SizeLabel))." "The copy may be incomplete. Copy or dump the game again if the port fails to start."
    }
    return $c
}

function Test-GameIso($Prof, $c, $item) {
    $c.Kind = 'iso'; $c.Root = $item.FullName; $c.Bytes = $item.Length
    if (Test-Path -LiteralPath ([IO.Path]::ChangeExtension($item.FullName, '.dkey'))) { [void]$c.Notes.Add('A .dkey file is next to the image: it may be an encrypted (Redump) image.') }
    $iso = $null
    try { $iso = Open-Iso $item.FullName } catch {}
    if (-not $iso) {
        Set-Bad $c 'error' 'ISO_UNREADABLE' "This file is not a readable disc image." "Check that the file was copied or downloaded completely, then pick it again."; return $c
    }
    try {
        $base = ''
        if (-not (Get-IsoEntry $iso 'PS3_GAME/PARAM.SFO')) {
            # some images keep the dump one folder down
            $top = Get-IsoDir $iso $iso.Root
            foreach ($e in $top.Values) { if ($e.Dir -and (Get-IsoEntry $iso "$($e.Name)/PS3_GAME/PARAM.SFO")) { $base = $e.Name; break } }
            if (-not $base) {
                Set-Bad $c 'error' 'NOT_PS3' "This disc image has no PS3 game in it (there is no PS3_GAME\PARAM.SFO)." "Pick the .iso of your $name disc."; return $c
            }
        }
        $c.Sub = $base
        $pre = if ($base) { "$base/" } else { '' }
        $sfoE = Get-IsoEntry $iso "${pre}PS3_GAME/PARAM.SFO"
        $sfo = Read-Sfo (Read-IsoBytes $iso $sfoE)
        if (-not (Test-SfoAndIds $Prof $c $sfo $item.FullName)) { return $c }
        $missing = @($Prof.Required | Where-Object { -not (Get-IsoEntry $iso "$pre$_") })
        if ($missing.Count) { Set-Bad $c 'error' 'MISSING' "Some game files are missing from the image: $($missing -join ', ')." "The image is incomplete or damaged. Make a new image of the disc."; return $c }
        foreach ($rel in $Prof.Program) {
            $e = Get-IsoEntry $iso "$pre$rel"
            Test-ProgramFile $Prof $c $rel (Read-IsoBytes $iso $e 4) { Get-IsoSha1 $iso $e }
        }
        if ($Prof.MinBytes -and $item.Length -lt $Prof.MinBytes) {
            Set-Bad $c 'warn' 'SMALL' "The disc image is smaller than expected ($(Format-GB $item.Length) of about $($Prof.SizeLabel))." "The image may be incomplete. Make a new image if the port fails to start."
        }
    } catch {
        Set-Bad $c 'error' 'ISO_UNREADABLE' "The disc image could not be read ($($_.Exception.Message))." "Check that the file was copied or downloaded completely, then pick it again."
    } finally { Close-Iso $iso }
    return $c
}

# Free space for the copy plus the build.
function Test-GameSpace($Prof, $c, [string]$Dest, [switch]$Move) {
    if ($c.Status -eq 'error') { return }
    $root = [IO.Path]::GetPathRoot([IO.Path]::GetFullPath($Dest))
    $sameDrive = $c.Kind -eq 'folder' -and ([IO.Path]::GetPathRoot($c.Root) -eq $root)
    $inPlace = $c.Kind -eq 'folder' -and [IO.Path]::GetFullPath($c.Root).TrimEnd('\') -ieq [IO.Path]::GetFullPath($Dest).TrimEnd('\')
    $need = [int64]$Prof.BuildBytes; if (-not ($Move -and $sameDrive) -and -not $inPlace) { $need += [int64]$c.Bytes }
    try { $free = [int64]([IO.DriveInfo]::new($root)).AvailableFreeSpace } catch { return }
    if ($free -lt $need) {
        Set-Bad $c 'error' 'SPACE' "Not enough free space on $root ($(Format-GB $free) free, about $(Format-GB $need) needed for the game files and the build)." `
            "Free up space on $root, or install the port on another drive."
    }
}

# ------------------------------------------------------------------ copy
$script:GfMountedIso = $null
function Dismount-GameSource {
    if ($script:GfMountedIso) { try { Dismount-DiskImage -ImagePath $script:GfMountedIso | Out-Null } catch {}; $script:GfMountedIso = $null }
}
# Returns $null on success, else an error message.
function Copy-GameSource($Prof, $c, [string]$Dest, [switch]$Move) {
    if ($c.Kind -eq 'folder' -and [IO.Path]::GetFullPath($c.Root).TrimEnd('\') -ieq [IO.Path]::GetFullPath($Dest).TrimEnd('\')) { return $null }   # already in place
    if ($c.Kind -eq 'folder') {
        if ($Move) {
            if (Test-Path -LiteralPath $Dest) { Remove-Item -LiteralPath $Dest -Recurse -Force }
            try { Move-Item -LiteralPath $c.Root -Destination $Dest -ErrorAction Stop } catch { return "Moving the game folder failed: $($_.Exception.Message)" }
            return $null
        }
        & robocopy $c.Root $Dest /E /NFL /NDL /NJH /NJS /NP | Out-Null
        if ($LASTEXITCODE -ge 8) { return "Copying the game files failed (robocopy code $LASTEXITCODE). Check the free space and that the source is readable." }
        return $null
    }
    # .iso: let Windows open it as a drive and copy from there; read it directly if that is not possible
    $letter = $null
    if (Get-Command Mount-DiskImage -ErrorAction SilentlyContinue) {
        try {
            Mount-DiskImage -ImagePath $c.Root -StorageType ISO -ErrorAction Stop | Out-Null
            $script:GfMountedIso = $c.Root
            for ($i = 0; $i -lt 20 -and -not $letter; $i++) {
                $letter = (Get-DiskImage -ImagePath $c.Root | Get-Volume -ErrorAction SilentlyContinue).DriveLetter
                if (-not $letter) { Start-Sleep -Milliseconds 500 }
            }
        } catch { $letter = $null }
    }
    if ($letter) {
        $src = "${letter}:\"; if ($c.Sub) { $src = Join-Path $src $c.Sub }
        & robocopy $src $Dest /E /NFL /NDL /NJH /NJS /NP | Out-Null
        $rc = $LASTEXITCODE
        Dismount-GameSource
        if ($rc -lt 8) { return $null }
        Write-Host "   Copying from the opened image failed (robocopy code $rc); reading the image directly instead..."
    } else {
        Dismount-GameSource
        Write-Host "   Reading the disc image directly..."
    }
    $iso = $null
    try {
        $iso = Open-Iso $c.Root
        $start = if ($c.Sub) { Get-IsoEntry $iso $c.Sub } else { @{ Dir = $true; Extents = @(, @($iso.Root.Lba, $iso.Root.Size)) } }
        $done = [int64]0
        Export-IsoTree $iso $start $Dest ([ref]$done) $c.Bytes
        Write-Progress -Activity 'Copying the game files from the disc image' -Completed
    } catch { return "Copying from the disc image failed: $($_.Exception.Message)" }
    finally { Close-Iso $iso }
    return $null
}

# ------------------------------------------------------------------ search (no file picker)
function Get-SearchFolders {
    $dirs = New-Object System.Collections.ArrayList
    $add = { param($d) if ($d -and (Test-Path -LiteralPath $d -PathType Container)) { [void]$dirs.Add((Get-Item -LiteralPath $d).FullName) } }
    foreach ($d in [IO.DriveInfo]::GetDrives()) {
        if (-not $d.IsReady -or $d.DriveType -notin 'Fixed', 'Removable', 'CDRom') { continue }
        $r = $d.RootDirectory.FullName
        if ($d.DriveType -eq 'CDRom') { & $add $r; continue }
        if ($r -match '^Z:\\$' -and (Test-Path 'Z:\home')) {
            # Wine / Proton: the Linux file system; EmuDeck and common Linux folders
            foreach ($h in Get-ChildItem 'Z:\home' -Directory -ErrorAction SilentlyContinue) {
                foreach ($s in 'Emulation\roms\ps3', 'Games', 'Downloads', 'Desktop') { & $add (Join-Path $h.FullName $s) }
            }
            foreach ($m in Get-ChildItem 'Z:\run\media' -Directory -ErrorAction SilentlyContinue) {
                & $add (Join-Path $m.FullName 'Emulation\roms\ps3')
                foreach ($m2 in Get-ChildItem $m.FullName -Directory -ErrorAction SilentlyContinue) { & $add (Join-Path $m2.FullName 'Emulation\roms\ps3') }
            }
            continue
        }
        & $add $r
        foreach ($s in 'Games', 'ISO', 'ISOs', 'PS3', 'PS3 Games', 'Emulation\roms\ps3', 'Emulators\RPCS3\games', 'RPCS3\games', 'Games\RPCS3\games') { & $add (Join-Path $r $s) }
    }
    foreach ($s in 'Downloads', 'Desktop', 'Documents', 'Videos') { & $add (Join-Path $env:USERPROFILE $s) }
    return $dirs
}
function Get-Rpcs3GamePaths {
    # RPCS3 lists the disc games it knows in config\games.yml ("BCUS98107: D:/Games/x/")
    $ymls = New-Object System.Collections.ArrayList
    foreach ($d in [IO.DriveInfo]::GetDrives()) {
        if (-not $d.IsReady -or $d.DriveType -ne 'Fixed') { continue }
        foreach ($s in 'RPCS3', 'Emulators\RPCS3', 'Games\RPCS3', 'Program Files\RPCS3', 'Games\Emulators\RPCS3') { [void]$ymls.Add((Join-Path $d.RootDirectory.FullName "$s\config\games.yml")) }
    }
    if ($env:APPDATA) { [void]$ymls.Add((Join-Path $env:APPDATA 'rpcs3\config\games.yml')) }
    if (Test-Path 'Z:\home') { foreach ($h in Get-ChildItem 'Z:\home' -Directory -ErrorAction SilentlyContinue) { [void]$ymls.Add((Join-Path $h.FullName '.config\rpcs3\games.yml')) } }
    foreach ($y in $ymls) {
        if (-not (Test-Path -LiteralPath $y)) { continue }
        foreach ($l in Get-Content -LiteralPath $y -ErrorAction SilentlyContinue) {
            if ($l -match '^\s*\w{9}\s*:\s*"?(.+?)"?\s*$') {
                $p = $Matches[1] -replace '/', '\'
                if ($p -match '^\\') { $p = 'Z:' + $p }   # a Linux path, seen from Wine
                $p
            }
        }
    }
}
# Returns checked candidates (only copies that look like this game, in any region), best first.
function Find-GameSources($Prof, [string]$PortDir = '') {
    $seen = @{}; $paths = New-Object System.Collections.ArrayList
    $consider = { param($p) $k = "$p".TrimEnd('\').ToLowerInvariant(); if ($PortDir -and "$p".StartsWith($PortDir, [StringComparison]::OrdinalIgnoreCase)) { return }; if ($p -and -not $seen[$k]) { $seen[$k] = 1; [void]$paths.Add($p) } }
    foreach ($p in Get-Rpcs3GamePaths) { & $consider $p }
    foreach ($d in Get-SearchFolders) {
        if (Test-Path -LiteralPath (Join-Path $d 'PS3_GAME\PARAM.SFO')) { & $consider $d }
        foreach ($f in Get-ChildItem -LiteralPath $d -Filter *.iso -File -ErrorAction SilentlyContinue) { & $consider $f.FullName }
        foreach ($s in Get-ChildItem -LiteralPath $d -Directory -ErrorAction SilentlyContinue) {
            if ($s.Name -match '^(Windows|Program Files.*|ProgramData|\$Recycle\.Bin|System Volume Information|Recovery|PerfLogs|AppData|node_modules|\..*)$') { continue }
            if ($PortDir -and $s.FullName.StartsWith($PortDir, [StringComparison]::OrdinalIgnoreCase)) { continue }
            if (Test-Path -LiteralPath (Join-Path $s.FullName 'PS3_GAME\PARAM.SFO')) { & $consider $s.FullName; continue }
            foreach ($f in Get-ChildItem -LiteralPath $s.FullName -Filter *.iso -File -ErrorAction SilentlyContinue) { & $consider $f.FullName }
            foreach ($s2 in Get-ChildItem -LiteralPath $s.FullName -Directory -ErrorAction SilentlyContinue) {
                if (Test-Path -LiteralPath (Join-Path $s2.FullName 'PS3_GAME\PARAM.SFO')) { & $consider $s2.FullName }
            }
        }
    }
    $found = New-Object System.Collections.ArrayList
    foreach ($p in $paths) {
        if (-not (Test-Path -LiteralPath $p)) { continue }
        $c = Quick-Identify $Prof $p
        if ($c) { [void]$found.Add($c) }
    }
    return @($found | Sort-Object @{ e = { if ($_.TitleId -eq $Prof.TitleId) { 0 } else { 1 } } }, Path)
}
# Cheap identification for the search: reads only PARAM.SFO, so it stays fast with many images.
function Quick-Identify($Prof, [string]$p) {
    $sfo = $null
    try {
        if ($p -match '\.iso$') {
            $iso = Open-Iso $p
            if (-not $iso) { return $null }
            try {
                $e = Get-IsoEntry $iso 'PS3_GAME/PARAM.SFO'
                if (-not $e) { foreach ($t in (Get-IsoDir $iso $iso.Root).Values) { if ($t.Dir) { $e = Get-IsoEntry $iso "$($t.Name)/PS3_GAME/PARAM.SFO"; if ($e) { break } } } }
                if ($e) { $sfo = Read-Sfo (Read-IsoBytes $iso $e) }
            } finally { Close-Iso $iso }
        } else {
            $r = Find-DumpRoot $p
            if ($r) { $p = $r; $sfo = Read-Sfo ([IO.File]::ReadAllBytes((Join-Path $r 'PS3_GAME\PARAM.SFO'))) }
        }
    } catch { return $null }
    if (-not $sfo) { return $null }
    $id = "$($sfo.TITLE_ID)"; $title = "$($sfo.TITLE)" -replace '[\r\n]+', ' ' -replace '[\u2122\u00AE]', ''
    if ($id -ne $Prof.TitleId -and $title -notmatch $Prof.TitleMatch) { return $null }
    $c = New-Check $p; $c.TitleId = $id; $c.Title = $title
    if ($id -ne $Prof.TitleId) { $c.Status = 'error'; $c.Code = 'WRONG_REGION'; $c.Problem = "$(Get-RegionName $id) ($id): not supported" }
    return $c
}

# One line per field, for the installer (Inno Setup reads it with LoadStringsFromFile).
function Write-CheckFile($c, [string]$Out) {
    $notes = (@($c.Notes) -join ' ')
    $lines = @("status=$($c.Status)", "code=$($c.Code)", "title=$($c.Title)", "titleid=$($c.TitleId)", "kind=$($c.Kind)",
               "root=$($c.Root)", "bytes=$($c.Bytes)", "problem=$($c.Problem)", "fix=$($c.Fix)", "notes=$notes")
    [IO.File]::WriteAllLines($Out, $lines, (New-Object Text.UTF8Encoding $false))
}

# ------------------------------------------------------------------ console flow (setup.ps1)
function Show-GameCheck($c) {
    $col = @{ ok = 'Green'; warn = 'Yellow'; error = 'Red' }[$c.Status]; if (-not $col) { $col = 'Gray' }
    if ($c.Status -eq 'ok') { Write-Host "   OK: $($c.Title) ($($c.TitleId)), $(Format-GB $c.Bytes)" -ForegroundColor $col; return }
    Write-Host "   Problem: $($c.Problem)" -ForegroundColor $col
    if ($c.Fix) { Write-Host "   What to do: $($c.Fix)" }
    foreach ($n in $c.Notes) { Write-Host "   Also: $n" -ForegroundColor $col }
}
function Test-IsWine {
    try { return [bool](Get-ItemProperty 'HKLM:\Software\Wine' -ErrorAction Stop) } catch {}
    return [bool]$env:WINEPREFIX -or [bool]$env:STEAM_COMPAT_DATA_PATH
}
# Asks for the game copy without needing a file picker: lists the copies found on this PC, and accepts a
# typed, pasted or dragged-in path. The Windows file picker is offered as an option only.
# Returns a checked source ($c, Status ok or an accepted warn), or $null if the player quits.
function Select-GameSource($Prof, [string]$Game, [string]$Dest, [string]$PortDir = '') {
    $want = "$($Prof.GameName) ($($Prof.TitleId), $($Prof.RegionLabel) disc)"
    $check = {
        param($path)
        $c = Test-GameSource $Prof $path
        if ($Dest) { Test-GameSpace $Prof $c $Dest -Move }   # checked again below if the player copies
        Show-GameCheck $c
        return $c
    }
    if ($Game) {
        Write-Host "   Checking $Game ..."
        $c = & $check $Game
        if ($c.Status -eq 'error') { return $null }
        return $c   # the installer already asked about warnings
    }
    Write-Host "   Looking for $want on this PC..."
    $found = @(Find-GameSources $Prof $PortDir)
    $picker = -not (Test-IsWine)
    while ($true) {
        Write-Host ""
        if ($found.Count) {
            Write-Host "   Copies found:"
            for ($i = 0; $i -lt $found.Count; $i++) {
                $x = $found[$i]; $note = if ($x.Status -eq 'error') { "  - $($x.Problem)" } else { '' }
                Write-Host ("    {0}) {1}   [{2} {3}]{4}" -f ($i + 1), $x.Path, $x.Title, $x.TitleId, $note)
            }
        } else {
            Write-Host "   No copy was found automatically."
        }
        Write-Host "   You can use a decrypted disc image (.iso) or a disc dump folder (the one with PS3_GAME in it)."
        $opts = @(); if ($found.Count) { $opts += "a number" }; $opts += "the full path (you can drag the file or folder into this window)"
        if ($picker) { $opts += "B to browse" }; $opts += "Q to quit"
        $ans = "$(Read-Host "   Type $($opts -join ', ')")".Trim().Trim('"').Trim("'").Trim()
        if (-not $ans) { continue }
        if ($ans -match '^[Qq]$') { return $null }
        $path = $null
        if ($ans -match '^\d+$' -and [int]$ans -ge 1 -and [int]$ans -le $found.Count) { $path = $found[[int]$ans - 1].Path }
        elseif ($picker -and $ans -match '^[Bb]$') { $path = Show-GamePicker $Prof }
        else { $path = $ans }
        if (-not $path) { continue }
        $c = & $check $path
        if ($c.Status -eq 'ok') { return $c }
        if ($c.Status -eq 'warn') {
            $y = Read-Host "   Use this copy anyway? [Y/N]"
            if ($y -match '^[Yy]') { return $c }
        }
    }
}
function Show-GamePicker($Prof) {
    try {
        Add-Type -AssemblyName System.Windows.Forms
        $dlg = New-Object System.Windows.Forms.OpenFileDialog
        $dlg.Title = "Select your $($Prof.GameName) disc image (.iso), or the PS3_DISC.SFB of a disc dump (Cancel: pick a folder)"
        $dlg.Filter = 'PS3 disc image or dump|*.iso;PS3_DISC.SFB;PARAM.SFO;EBOOT.BIN|All files|*.*'
        if ($dlg.ShowDialog() -eq 'OK') { return $dlg.FileName }
        $fdlg = New-Object System.Windows.Forms.FolderBrowserDialog
        $fdlg.Description = "Select your $($Prof.GameName) disc dump (the folder that contains PS3_GAME)"
        if ($fdlg.ShowDialog() -eq 'OK') { return $fdlg.SelectedPath }
    } catch { Write-Host "   The file picker is not available here. Type or paste the path instead." }
    return $null
}
# The whole "game copy" step: pick, check, then copy or move into $Dest. Returns $null when done,
# or a message for the caller to stop with. -Mode Copy|Move answers the move-or-copy question (Setup.exe).
# While copying, <Dest>.copying exists, so an interrupted copy is redone (robocopy skips finished files).
function Install-GameFiles($Prof, [string]$Game, [string]$Dest, [string]$PortDir = '', [string]$Mode = '') {
    $c = Select-GameSource $Prof $Game $Dest $PortDir
    if (-not $c) {
        if ($Game) { return "The game copy at $Game can't be used (see the problem above)." }
        return "No game copy picked."
    }
    $move = $false
    if ($c.Kind -eq 'folder' -and ([IO.Path]::GetPathRoot($c.Root) -eq [IO.Path]::GetPathRoot([IO.Path]::GetFullPath($Dest)))) {
        $ans = if ($Mode) { $Mode } else { Read-Host "   Move the game folder into the port folder (instant, but RPCS3 will no longer find it there) or Copy it (keeps the original, needs about $($Prof.SizeLabel) free)? [M/C]" }
        $move = ($ans -match '^[Mm]')
    }
    if (-not $move) {
        Test-GameSpace $Prof $c $Dest
        if ($c.Status -eq 'error') { return "$($c.Problem) $($c.Fix)" }
    }
    if ($move) { Write-Host "   Moving..." } else { Write-Host "   Copying the game files (about $($Prof.SizeLabel), this takes a while)..." }
    $marker = "$($Dest.TrimEnd('\')).copying"
    try { Set-Content -LiteralPath $marker -Value "Copying from $($c.Root)" -Encoding ASCII } catch {}
    $err = Copy-GameSource $Prof $c $Dest -Move:$move
    Dismount-GameSource
    if ($err) { return $err }
    $after = Test-GameSource $Prof $Dest
    if ($after.Status -eq 'error') { return "The copied game files are not complete: $($after.Problem) $($after.Fix)" }
    Remove-Item -LiteralPath $marker -Force -ErrorAction SilentlyContinue
    return $null
}