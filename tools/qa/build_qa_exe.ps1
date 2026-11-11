# Builds build_qa\<Exe>.exe (default Resistance3): the live build\ objects relinked with
#   - build_qa_rpcs3\rpcs3_rsx.lib  (rpcs3_shim\r2_qa_capture.cpp: R2_QA_CAPTURE renderer frame capture)
#   - a fresh spu_channels.c object (the RPCS3 tree references symbols defined there)
# Nothing in build\ or build_rpcs3\ is written. Run from tools\qa\build_qa_exe.bat (sets up the VS environment).
param(
    [string]$Root = (Resolve-Path "$PSScriptRoot\..\..").Path,
    [string]$Out = "build_qa",
    [string]$RsxBuild = "build_qa_rpcs3",
    [string]$Exe = "Resistance3",
    [string[]]$Units = @('ps3recomp_sdk\CMakeFiles\ps3recomp_runtime.dir\runtime\spu\spu_channels.c.obj')
)
$ErrorActionPreference = 'Stop'
Set-Location $Root
$build = Join-Path $Root 'build'
$outDir = Join-Path $Root $Out
$objDir = Join-Path $outDir 'obj'
New-Item -ItemType Directory -Force $objDir | Out-Null

if (-not (Test-Path "$Root\$RsxBuild\rpcs3_rsx.lib")) { throw "$RsxBuild\rpcs3_rsx.lib missing - run tools\qa\build_qa_rpcs3.bat first" }

# 1. Recompile the two runtime TUs with the live build's exact flags, into build_qa\obj
$newObjs = @()
Push-Location $build
try {
    foreach ($u in $Units) {
        $cmd = (& ninja -t commands -s $u | Select-Object -Last 1)
        if (-not $cmd) { throw "no compile command for $u" }
        $leaf = [IO.Path]::GetFileName($u)
        $dst = Join-Path $objDir $leaf
        $esc = [regex]::Escape($u)
        $cmd = $cmd -replace "/Fo$esc", "/Fo`"$dst`""
        $cmd = $cmd -replace "-clang:-MT$esc(\.d)?", ''
        $cmd = $cmd -replace "-clang:-MF\S+", ''
        $cmd = $cmd -replace '-clang:-MD', ''
        $cmd = $cmd -replace '/Fd\S+', "/Fd`"$objDir\qa_runtime.pdb`""
        Write-Host "compiling $leaf"
        & $env:ComSpec /c $cmd
        if ($LASTEXITCODE -ne 0) { throw "compile failed: $leaf" }
        $newObjs += $dst
    }
} finally { Pop-Location }

# 2. Link: same objects/libraries as build\build.ninja, with the QA pieces substituted
$lines = Get-Content (Join-Path $build 'build.ninja')
$i = ($lines | Select-String -Pattern "^build $([regex]::Escape($Exe))\.exe:" | Select-Object -First 1).LineNumber - 1
$head = $lines[$i] -replace "^build $([regex]::Escape($Exe))\.exe:\s*\S+\s*", ''
$head = ($head -split '\s\|\s|\s\|\|\s')[0]
$objs = $head -split '\s+' | Where-Object { $_ } | ForEach-Object { $_ -replace '\$:', ':' -replace '\$ ', ' ' }
$libsLine = ($lines[($i + 1)..($i + 20)] | Where-Object { $_ -match '^\s+LINK_LIBRARIES = ' } | Select-Object -First 1) -replace '^\s+LINK_LIBRARIES = ', ''
$libs = $libsLine -split '\s+' | Where-Object { $_ }
$libs = $libs | ForEach-Object {
    $q = $_ -replace '\\build_rpcs3\\', "\$RsxBuild\"
    if ($q -ne $_ -and (Test-Path $q)) { $q } else { $_ }
}

$rsp = New-Object System.Collections.Generic.List[string]
foreach ($o in $objs) {
    if ([IO.Path]::IsPathRooted($o)) { $rsp.Add("`"$o`"") } else { $rsp.Add("`"$(Join-Path $build $o)`"") }
}
foreach ($o in $newObjs) { $rsp.Add("`"$o`"") }   # explicit objects win over the runtime.lib members
foreach ($l in $libs) {
    if ($l -match '[\\/]' -and -not [IO.Path]::IsPathRooted($l)) { $l = Join-Path $build $l }
    $rsp.Add("`"$l`"")
}
$rspPath = Join-Path $outDir "$($Exe)_qa.rsp"
Set-Content -Encoding ascii $rspPath $rsp

Write-Host "linking $Out\$Exe.exe ($($objs.Count) objects)"
$linkLog = Join-Path $outDir 'link.log'
& $env:ComSpec /c "lld-link /nologo `"@$rspPath`" `"/out:$outDir\$Exe.exe`" `"/pdb:$outDir\$Exe.pdb`" /machine:x64 /debug /INCREMENTAL:NO /manifest:embed /subsystem:console /STACK:8388608,1048576 > `"$linkLog`" 2>&1"
$linkRc = $LASTEXITCODE
Get-Content $linkLog | Select-String -Pattern 'error' | Select-Object -First 30
if ($linkRc -ne 0) { throw "link failed - see $Out\link.log" }

# 3. Runtime DLLs next to the exe (same set build.bat copies)
foreach ($d in 'avcodec-61.dll', 'avutil-59.dll', 'swresample-5.dll', 'r2_dlss.dll', 'nvngx_dlss.dll', 'amd_fidelityfx_vk.dll', 'libxess.dll', 'sl.interposer.dll', 'sl.common.dll', 'sl.dlss_g.dll', 'sl.reflex.dll', 'sl.pcl.dll', 'nvngx_dlssg.dll', 'NvLowLatencyVk.dll') {
    if (Test-Path "$build\$d") { Copy-Item -Force "$build\$d" $outDir }
}
Write-Host "OK: $outDir\$Exe.exe"

