# Builds a GameCube game package in RECOMP mode: the game's machine code from your own disc,
# recompiled to C (GcnRecomp), with com.recomp.gcn's SDK replacement as its HLE module.
# Polyphase runs this from Packaging > Target Options > GCN Recomp (Build mode Recomp / Live).
#
#   build_recomp.ps1 -Package <Packages\com.recomp.<game>> [-Disc FILE] [-Decomp DIR] [-Config Release] [-DebugCrt] [-Live]
#
#   -Package   the game package; its Recomp\ folder holds game.json and syms.txt (names only)
#   -Disc      your disc image (default: Native\local.json, else gcn_game.json "disc" in the decomp,
#              else the disc already unpacked into the project)
#   -Decomp    the decomp checkout (the SDK replacement compiles against its headers;
#              default: Native\local.json, else gcn_game.json "decomp")
#   -DebugCrt  link the library against the debug C runtime (/MDd: Debug editor builds)
#   -Live      a Live build: no recompiled C; the library recompiles the game from the disc it runs
#              from when it starts (Runtime/recomp/live, sljit). It holds no game code, only the
#              symbols (names and addresses), and needs no disc to build.
#
# Writes:
#   <project>\Assets\Recomp\<name>\Disc\     your disc unpacked: what the game reads at run time
#                                            (project assets, not the package; git-ignored)
#   <project>\Assets\Recomp\<name>\game.json the disc the build was made from (launcher check)
#   <package>\Native\build\recomp-<cfg>\     intermediates (generated C: never commit)
#   com.recomp.gcn\Lib\Windows\<name>_recomp.lib            the game (git-ignored)
#   com.recomp.gcn\Source\Guest\<name>_recomp\              registers it with GcnPlayer (+ mode.txt)
# and removes com.recomp.gcn\Source\Guest\<name> (the decomp build of the same game: one at a time).
param([Parameter(Mandatory = $true)][string]$Package, [string]$Disc = '', [string]$Decomp = '',
      [string]$Config = 'Release', [switch]$DebugCrt, [switch]$Live)
$ErrorActionPreference = 'Stop'

function Step($text) { Write-Host "[gcn recomp] $text" }

$gcn = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path          # com.recomp.gcn
$tools = Join-Path $gcn 'Runtime\tools'
$pkg = (Resolve-Path $Package).Path
$project = (Resolve-Path (Join-Path $pkg '..\..')).Path
$native = Join-Path $pkg 'Native'
$recompSrc = Join-Path $pkg 'Recomp'
if (-not (Test-Path (Join-Path $recompSrc 'game.json'))) { throw "$pkg has no Recomp\game.json" }
$game = Get-Content (Join-Path $recompSrc 'game.json') -Raw | ConvertFrom-Json
$name = $game.name
$hleName = "${name}recomp"     # no underscore: the wasm2c module name
$syms = Join-Path $recompSrc 'syms.txt'
if (-not (Test-Path $syms)) { throw "$recompSrc\syms.txt is missing (gcn_syms.py makes it from the decomp)" }

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) { $python = Get-Command py -ErrorAction SilentlyContinue }
if (-not $python) { throw 'Python 3 is needed (python or py on PATH)' }
$python = $python.Source

# the decomp build's config (where the decomp and the disc are)
$cfg = $null
if (Test-Path (Join-Path $native 'gcn_game.json')) { $cfg = Get-Content (Join-Path $native 'gcn_game.json') -Raw | ConvertFrom-Json }
$local = $null
if (Test-Path (Join-Path $native 'local.json')) { $local = Get-Content (Join-Path $native 'local.json') -Raw | ConvertFrom-Json }
function FromNative($p) { if ([System.IO.Path]::IsPathRooted($p)) { $p } else { Join-Path $native $p } }
if (-not $Decomp) {
    if ($local -and $local.decomp) { $Decomp = FromNative $local.decomp }
    elseif ($cfg -and $cfg.decomp) { $Decomp = FromNative $cfg.decomp }
}
if (-not $Disc) {
    if ($local -and $local.disc) { $Disc = FromNative $local.disc }
    elseif ($cfg -and $cfg.disc -and $Decomp) { $Disc = Join-Path $Decomp $cfg.disc }
}

# ---- the disc, unpacked into the project (what the game reads at run time) ----------------
$assets = Join-Path $project "Assets\Recomp\$name"
$discOut = Join-Path $assets 'Disc'
$idx = Join-Path $discOut 'disc.idx'
if ($Disc -and (Test-Path $Disc -PathType Leaf)) {
    $have = if (Test-Path $idx) { ((Get-Content $idx -TotalCount 1) -split ' ')[-1] } else { '' }
    $stream = [System.IO.File]::OpenRead($Disc)
    $head = New-Object byte[] 6
    [void]$stream.Read($head, 0, 6)
    if ([System.Text.Encoding]::ASCII.GetString($head, 0, 4) -eq 'CISO') {
        # a CISO image: the disc starts in its first stored block, after the 32 KB block map
        [void]$stream.Seek(0x8000, 'Begin')
        [void]$stream.Read($head, 0, 6)
    }
    $stream.Close()
    $want = [System.Text.Encoding]::ASCII.GetString($head)
    if ($have -ne $want) {
        Step "unpacking the disc ($want) into $discOut"
        New-Item -ItemType Directory -Force $discOut | Out-Null
        $unpackArgs = @((Join-Path $tools 'gcn_disc.py'), 'unpack', $Disc, $discOut)
        if ($have) { $unpackArgs += '--force' }
        & $python @unpackArgs
        if ($LASTEXITCODE -ne 0) { throw 'unpacking the disc failed' }
    }
}
elseif (-not (Test-Path $idx) -and -not $Live) {
    throw "no disc: pass -Disc (your .iso / .gcm / .nkit.iso / .ciso), or set it in Pre Process Rom"
}
$dol = Join-Path $discOut 'sys\main.dol'
if (Test-Path $dol) {
    $sha1 = (Get-FileHash -Algorithm SHA1 $dol).Hash.ToLower()
    if ($game.dol_sha1 -and $sha1 -ne $game.dol_sha1.ToLower()) {
        throw "this disc's main.dol (sha1 $sha1) is not the one $name was set up for ($($game.dol_sha1)): another region or revision"
    }
}
else {
    Step 'no disc unpacked into the project: the Live build asks the player for theirs (launcher)'
}
# what the build was made from, for the launcher's disc check (GcnLauncher): project assets,
# packaged with the game
New-Item -ItemType Directory -Force $assets | Out-Null
Copy-Item (Join-Path $recompSrc 'game.json') (Join-Path $assets 'game.json') -Force
# the unpacked disc never goes into git
$gitignore = Join-Path $project '.gitignore'
$ignoreLine = "Assets/Recomp/$name/Disc/"
if ((Test-Path $gitignore) -and -not (Select-String -Path $gitignore -SimpleMatch $ignoreLine -Quiet)) {
    Add-Content $gitignore "`n# your own game disc, unpacked for the recompiled game (com.recomp.gcn build_recomp.ps1)`n$ignoreLine"
}

# ---- toolchain -------------------------------------------------------------------------------
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
if (-not $vs) { throw 'Visual Studio 2022 (with its C++ clang tools) is needed' }
if (-not $env:VSCMD_VER) {
    $vars = & "$env:SystemRoot\System32\cmd.exe" /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1 && set"
    foreach ($line in $vars) { if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] } }
}
$cmake = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
$clangcl = "$vs\VC\Tools\Llvm\x64\bin\clang-cl.exe"

# ---- recompile (AOT), HLE module, glue -------------------------------------------------------
$build = Join-Path $native ("build\recomp-$Config" + $(if ($DebugCrt) { '-dcrt' } else { '' }))
$out = Join-Path $build 'out'
$hle = Join-Path $build 'hle'
if (-not $Live) {
    # the recompiler (built once)
    $toolBuild = Join-Path $gcn 'Runtime\build\gcnrecomp'
    if (-not (Test-Path (Join-Path $toolBuild 'build.ninja'))) {
        & $cmake -S (Join-Path $gcn 'Runtime\recomp\tool') -B $toolBuild -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" `
            "-DCMAKE_CXX_COMPILER=$clangcl" -DCMAKE_BUILD_TYPE=Release | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'configuring GcnRecomp failed' }
    }
    & $cmake --build $toolBuild --target GcnRecomp | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'building GcnRecomp failed' }
    $gcnrecomp = Join-Path $toolBuild 'GcnRecomp.exe'
    Step "recompiling $dol"
    & $gcnrecomp --dol $dol --syms $syms --out $out
    if ($LASTEXITCODE -ne 0) { throw 'GcnRecomp failed' }
}

Step 'building the HLE module (com.recomp.gcn Runtime\guest)'
if ($Decomp) { $env:GCN_DECOMP = (Resolve-Path $Decomp).Path }
& $python (Join-Path $tools 'recomp\gcn_hle_build.py') $native --syms $syms --out $hle --name $hleName --config $Config
if ($LASTEXITCODE -ne 0) { throw 'gcn_hle_build failed' }
& $python (Join-Path $tools 'recomp\gen_hle_glue.py') $hle $hleName $syms (Join-Path $hle "${hleName}_glue.c")
if ($LASTEXITCODE -ne 0) { throw 'gen_hle_glue failed' }
if ($Live) {
    & $python (Join-Path $tools 'recomp\gcn_live_syms.py') $syms (Join-Path $hle "${hleName}_live_syms.c")
    if ($LASTEXITCODE -ne 0) { throw 'gcn_live_syms failed' }
}

# ---- the library -------------------------------------------------------------------------------
$libBuild = Join-Path $build $(if ($Live) { 'lib-live' } else { 'lib' })
$crt = if ($DebugCrt) { 'MultiThreadedDebugDLL' } else { 'MultiThreadedDLL' }
if (-not (Test-Path (Join-Path $libBuild 'build.ninja'))) {
    & $cmake -S (Join-Path $gcn 'Runtime\recomp\host') -B $libBuild -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" `
        "-DCMAKE_C_COMPILER=$clangcl" $(if ($Live) { "-DCMAKE_CXX_COMPILER=$clangcl" } else { '-DGCN_RECOMP_LIVE=OFF' }) -DCMAKE_BUILD_TYPE=Release `
        "-DCMAKE_MSVC_RUNTIME_LIBRARY=$crt" -DCMAKE_POLICY_DEFAULT_CMP0091=NEW "-DGCN_HLE_DIR=$hle" `
        "-DGCN_RECOMP_DIR=$out" "-DGCN_HLE_NAME=$hleName" "-DGCN_RECOMP_NAME=$name" -DGCN_RECOMP_RUNNER=OFF `
        "-DGCN_RECOMP_LIVE=$(if ($Live) { 'ON' } else { 'OFF' })" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'configuring the game library failed' }
}
Step "compiling the game library ($(if ($Live) { 'Live, ' } else { '' })$Config, $crt)"
& $cmake --build $libBuild
if ($LASTEXITCODE -ne 0) { throw 'compiling the game library failed' }
$lib = Join-Path $libBuild "${name}_recomp.lib"

# ---- publish into com.recomp.gcn -----------------------------------------------------------
$libDir = Join-Path $gcn 'Lib\Windows'
New-Item -ItemType Directory -Force $libDir | Out-Null
$published = Join-Path $libDir "${name}_recomp.lib"
Copy-Item $lib $published -Force
$libSha1 = (Get-FileHash -Algorithm SHA1 $published).Hash.ToLower()
$guest = Join-Path $gcn "Source\Guest\${name}_recomp"
New-Item -ItemType Directory -Force $guest | Out-Null
$libPath = $published -replace '\\', '/'
$register = @"
// Generated by com.recomp.gcn Runtime/tools/recomp/build_recomp.ps1 - do not edit.
// Registers $($game.title) ($(if ($Live) { 'recompiled from your disc when it starts' } else { 'recompiled from your disc' })) with GcnPlayer;
// the game is $libPath (library sha1 $libSha1).
#if defined(_WIN32) && defined(_M_X64)
#pragma comment(lib, "$libPath")
// the addon's own C runtime only (the editor's addon builder can pick up a Debug Lua.lib for a
// release addon, which drags in the other one)
#if defined(_DEBUG)
#pragma comment(linker, "/NODEFAULTLIB:msvcrt.lib")
#else
#pragma comment(linker, "/NODEFAULTLIB:msvcrtd.lib")
#endif
#include "../../Gcn/gcnw_module.h"

extern "C" const GcnwModule gcnw_module_$hleName;

namespace
{
struct Register
{
    Register() { gcnw_register_module(&gcnw_module_$hleName); }
} sRegister;
}
#endif
"@
Set-Content -Path (Join-Path $guest "${name}_recomp_guest_register.cpp") -Value $register -Encoding UTF8
Set-Content -Path (Join-Path $guest 'mode.txt') -Value $(if ($Live) { 'recomp-live' } else { 'recomp' }) -Encoding ASCII
$decompGuest = Join-Path $gcn "Source\Guest\$name"
if (Test-Path $decompGuest) {
    Step "removing the decomp build of $name (Source\Guest\$name): one build of a game at a time"
    Remove-Item -Recurse -Force $decompGuest
}
Step "done: $published. Reload Native Addons so the editor links it."
