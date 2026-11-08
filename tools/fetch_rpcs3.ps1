# Downloads RPCS3's source at the pinned commit (plus the third-party code the
# RSX/Vulkan build needs) and applies this port's patch
# (ps3recomp\patches\r3-rpcs3.patch). RPCS3 is GPL-2.0; see its LICENSE.
param([string]$Dest = (Join-Path $PSScriptRoot '..\ps3recomp\third_party\rpcs3'))
$ErrorActionPreference = 'Stop'

$Commit = 'eee3f0f7c285f2b8aa18fec278a2146c5cd874eb'
$Patch  = (Resolve-Path (Join-Path $PSScriptRoot '..\ps3recomp\patches\r3-rpcs3.patch')).Path
$Submodules = '3rdparty/asmjit/asmjit', '3rdparty/glslang/glslang', '3rdparty/GPUOpen/VulkanMemoryAllocator',
              '3rdparty/stblib/stb', '3rdparty/yaml-cpp/yaml-cpp'

function Invoke-Git { $ErrorActionPreference = 'Continue'; & git -c core.autocrlf=false -c advice.detachedHead=false @args; if ($LASTEXITCODE -ne 0) { throw "git $args failed" } }

if (Test-Path (Join-Path $Dest 'rpcs3\Emu\RSX\VK\VKR2Upscaler.cpp')) { Write-Host "   RPCS3 source already present."; exit 0 }
if (Test-Path $Dest) { Remove-Item $Dest -Recurse -Force }

Invoke-Git init -q $Dest
Push-Location $Dest
try {
    Invoke-Git config core.autocrlf false
    Invoke-Git remote add origin https://github.com/RPCS3/rpcs3.git
    Invoke-Git fetch -q --depth 1 origin $Commit
    Invoke-Git checkout -q FETCH_HEAD
    Invoke-Git submodule update -q --init --depth 1 --jobs 5 -- @Submodules
    # Not from the RPCS3 submodule set: pugixml 1.15 and Vulkan-Headers 1.4.344.
    Remove-Item 3rdparty\pugixml -Recurse -Force -ErrorAction SilentlyContinue
    Invoke-Git clone -q --depth 1 --branch v1.15 https://github.com/zeux/pugixml.git 3rdparty/pugixml
    Invoke-Git clone -q --depth 1 --branch v1.4.344 https://github.com/KhronosGroup/Vulkan-Headers.git 3rdparty/Vulkan-Headers
    Invoke-Git apply --whitespace=nowarn $Patch
} finally { Pop-Location }
Write-Host "   RPCS3 $($Commit.Substring(0,7)) downloaded and patched."
