# Blue Dragon VR: fetch + build ReXGlue SDK (bd833a2, the SDK re:Blue v1.3.1 was released with) and re:Blue from source.
# Runs detached (started by an agent command). Log: build_logs\build_<stamp>.log, status line in build_logs\build_status.txt.
#   -Stage all|fetch|sync|sdk|configure|app      (all = fetch if not done, sync, sdk if not installed, configure if needed, app)
param([string]$Stage = 'all', [int]$Jobs = 0)
$ErrorActionPreference = 'Stop'
$P        = 'D:\vrconversions\BlueDragon'
$Rex      = "$P\rexglue"
$Src      = "$P\src"
$Install  = "$Rex\out\install\win-amd64"
$Vcpkg    = 'D:\vrconversions\tools\vcpkg'
$RexCommit = 'bd833a2ab69d305f99ddf57418fc38ffec8af93d'
$BdCommit  = 'v1.3.1'
$Bdir     = "$Src\out\build\vr"
$Logs     = "$P\build_logs"; New-Item -ItemType Directory -Force $Logs | Out-Null
$stamp    = Get-Date -Format 'yyyyMMdd_HHmmss'
$log      = "$Logs\build_$stamp.log"
$statusFile = "$Logs\build_status.txt"
function Status([string]$s) { "$(Get-Date -Format o) $s" | Set-Content $statusFile -Encoding ASCII; "== $(Get-Date -Format T) $s" | Out-File -FilePath $log -Append -Encoding UTF8 }
function Run([string]$what, [scriptblock]$sb) {
    Status "running: $what"
    $ErrorActionPreference = 'Continue'
    $t0 = Get-Date
    & $sb *>&1 | ForEach-Object { "$_" } | Out-File -FilePath $log -Append -Encoding UTF8
    if ($LASTEXITCODE -ne 0) { Status "FAILED: $what (exit $LASTEXITCODE) log=$log"; exit 1 }
    "   ($what took $([int]((Get-Date)-$t0).TotalSeconds) s)" | Out-File -FilePath $log -Append -Encoding UTF8
    $ErrorActionPreference = 'Stop'
}
. 'D:\vrconversions\tools\lo_env.ps1'     # LLVM 20, CMake, Ninja, git, python on PATH
# MSVC / Windows SDK environment (LIB / INCLUDE: the agility SDK package looks for d3d12.lib), then our tools first.
$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
if (Test-Path $vcvars) {
    foreach ($line in (cmd /c "`"$vcvars`" >nul 2>&1 && set")) {
        if ($line -match '^(LIB|LIBPATH|INCLUDE|WindowsSdkDir|WindowsSDKVersion|VCToolsInstallDir|UCRTVersion|UniversalCRTSdkDir)=(.*)$') { Set-Item "env:$($Matches[1])" $Matches[2] }
    }
}
if ($Jobs -le 0) { $Jobs = [Math]::Max(2, [Environment]::ProcessorCount - 2) }
"log $log stage=$Stage jobs=$Jobs" | Out-File $log -Encoding UTF8
"clang: $((& clang --version | Select-Object -First 1))  cmake: $((& cmake --version | Select-Object -First 1))" | Out-File $log -Append

function Fix-Symlinks([string]$tree) {
    # Windows git checks symlinks out as text files holding the target path: replace them with copies.
    Push-Location $tree; $fixed = 0
    foreach ($line in (git ls-files -s --recurse-submodules)) {
        if ($line -notmatch '^120000 \S+ \d+\t(.+)$') { continue }
        $link = Join-Path $tree ($Matches[1] -replace '/', '\')
        if (-not (Test-Path $link -PathType Leaf)) { continue }
        $target = (Get-Content $link -Raw).Trim()
        if ($target.Length -gt 260 -or $target -match "`n") { continue }
        $tp = Join-Path (Split-Path $link) ($target -replace '/', '\')
        if (Test-Path $tp -PathType Leaf) { Copy-Item $tp $link -Force; $fixed++ }
    }
    Pop-Location
    "symlink stubs replaced in ${tree}: $fixed" | Out-File $log -Append
}

if ($Stage -eq 'fetch' -or ($Stage -eq 'all' -and -not (Test-Path "$P\.fetch.ok"))) {
    if (-not (Test-Path "$Vcpkg\vcpkg.exe")) {
        if (-not (Test-Path "$Vcpkg\.git")) { Run 'clone vcpkg' { git clone https://github.com/microsoft/vcpkg.git $Vcpkg } }
        Run 'bootstrap vcpkg' { & "$Vcpkg\bootstrap-vcpkg.bat" -disableMetrics }
    }
    if (-not (Test-Path "$Rex\.git")) { Run 'clone rexglue' { git -c core.longpaths=true clone --no-checkout https://github.com/rexglue/rexglue-sdk.git $Rex } }
    Push-Location $Rex
    git config core.longpaths true; git config core.autocrlf false
    Run "rexglue checkout $RexCommit" { git fetch --tags origin; git checkout -B bd-vr $RexCommit }
    Run 'rexglue submodules' { git -c core.longpaths=true submodule update --init --recursive --jobs 8 }
    Pop-Location
    Fix-Symlinks $Rex
    if (-not (Test-Path "$Src\.git")) { Run 'clone reblue' { git -c core.longpaths=true -c core.autocrlf=false clone https://github.com/zolaware/reblue.git $Src } }
    Push-Location $Src
    git config core.longpaths true; git config core.autocrlf false
    if ((git rev-parse --abbrev-ref HEAD) -ne 'vr') { Run "reblue checkout $BdCommit" { git checkout -B vr $BdCommit } }
    Run 'reblue submodules' { git -c core.longpaths=true submodule update --init --recursive --jobs 8 }
    Pop-Location
    Fix-Symlinks $Src
    # assets\ = the decrypted-by-codegen default.xex + the game's shaders (extracted from !necessity.ipk by 002)
    New-Item -ItemType Directory -Force "$Src\assets" | Out-Null
    Copy-Item "$P\assets\*" "$Src\assets" -Recurse -Force
    Set-Content "$P\.fetch.ok" (Get-Date -Format o)
}
if ($Stage -in 'all','sync') {
    # VR changes: patches\rexglue-vr.patch (pinned SDK), patches\reblue-vr.patch (re:Blue tree) and
    # patches\xenosrecomp-vr.patch (thirdparty\XenosRecomp submodule). The previously applied
    # copy is kept as <tree>\.vrpatch.applied and reverse-applied first, so only patched files change.
    foreach ($pair in @(@($Rex, 'rexglue-vr.patch'), @($Src, 'reblue-vr.patch'), @("$Src\thirdparty\XenosRecomp", 'xenosrecomp-vr.patch'))) {
        $tree = $pair[0]; $pp = "$P\patches\$($pair[1])"; $applied = "$tree\.vrpatch.applied"
        if (-not (Test-Path $pp)) { continue }
        if ((Test-Path $applied) -and ((Get-FileHash $applied).Hash -eq (Get-FileHash $pp).Hash)) { "$($pair[1]) already applied" | Out-File $log -Append; continue }
        Push-Location $tree
        if (Test-Path $applied) { Run "revert old $($pair[1])" { git apply -R --whitespace=nowarn $applied }; Remove-Item $applied }
        Run "apply $($pair[1])" { git apply --whitespace=nowarn $pp }
        Copy-Item $pp $applied
        Pop-Location
    }
}
if ($Stage -eq 'sdk' -or ($Stage -eq 'all' -and -not (Test-Path "$Install\lib\cmake\rexglue"))) {
    Push-Location $Rex
    if (-not (Test-Path 'out\build\win-amd64\build.ninja')) {
        Run 'rexglue configure' { cmake --preset win-amd64 -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DREXGLUE_ENABLE_TRACY=OFF }
    }
    Run 'rexglue build+install (Release)' { cmake --build out\build\win-amd64 --config Release --target install --parallel $Jobs }
    Pop-Location
}
if ($Stage -eq 'configure' -or ($Stage -in 'all','app' -and -not (Test-Path "$Bdir\build.ninja"))) {
    Push-Location $Src
    $cfg = @('-S', '.', '-B', $Bdir, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
        '-DCMAKE_C_COMPILER=clang', '-DCMAKE_CXX_COMPILER=clang++',
        "-DCMAKE_TOOLCHAIN_FILE=$Vcpkg\scripts\buildsystems\vcpkg.cmake", '-DVCPKG_TARGET_TRIPLET=x64-windows-static',
        "-DCMAKE_PREFIX_PATH=$Install", '-DREBLUE_VULKAN_EXE=OFF', '-DREBLUE_PROFILING=OFF',
        '-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded', '-DCMAKE_C_FLAGS_RELEASE=-O3 -g -DNDEBUG', '-DCMAKE_CXX_FLAGS_RELEASE=-O3 -g -DNDEBUG')
    Run 'reblue configure' { cmake @cfg }
    if (-not (Test-Path "$Src\generated\sources.cmake")) {
        Run 'reblue codegen' { cmake --build $Bdir --target reblue_codegen }
        Run 'reblue reconfigure' { cmake @cfg }
    }
    Pop-Location
}
if ($Stage -in 'all','app') {
    Run 'reblue build' { cmake --build $Bdir --target reblue --parallel $Jobs }
    $exe = "$Bdir\reblue.exe"
    if (Test-Path $exe) { Status "OK: built $exe $([int]((Get-Item $exe).Length/1MB)) MB log=$log" } else { Status "FAILED: exe not found log=$log"; exit 1 }
} else { Status "OK: stage $Stage done log=$log" }
