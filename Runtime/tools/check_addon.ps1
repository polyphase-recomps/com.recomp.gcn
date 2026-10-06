# Compiles the com.recomp.gcn addon (Source/, via the package CMakeLists) as a quick check,
# outside the editor. The editor's addon builder remains the real build.
# -Engine defaults to the engine the project records in PolyphaseConfig.cmake (the one the
# editor builds addons against), not the POLYPHASE_PATH environment variable.
param([string]$BuildDir = (Join-Path $PSScriptRoot '..\build\addon-check'), [string]$Engine = '')
$ErrorActionPreference = 'Continue'
if (-not $Engine) {
    $config = Join-Path $PSScriptRoot '..\..\..\..\PolyphaseConfig.cmake'
    if ((Test-Path $config) -and ((Get-Content $config -Raw) -match 'set\(POLYPHASE_PATH "([^"]+)"\)')) {
        $Engine = $Matches[1]
    }
}
# Link against the editor build that is newest (DebugEditor or ReleaseEditor import lib).
$buildType = 'Release'
if ($Engine) {
    $libs = @('DebugEditor', 'ReleaseEditor') | ForEach-Object {
        Get-Item (Join-Path $Engine "Standalone\Build\Windows\x64\$_\Polyphase.lib") -ErrorAction SilentlyContinue
    } | Sort-Object LastWriteTime -Descending
    if ($libs -and $libs[0].FullName -match 'DebugEditor') { $buildType = 'Debug' }
}
$BuildDir = "$BuildDir-$buildType"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -property installationPath
$vars = & "$env:SystemRoot\System32\cmd.exe" /c "`"$vs\VC\Auxiliary\Build\vcvarsall.bat`" x64 >nul 2>&1 && set"
foreach ($line in $vars) {
    if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}
$cmake = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
& $cmake -S (Join-Path $PSScriptRoot '..\..') -B $BuildDir -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" `
    "-DCMAKE_BUILD_TYPE=$buildType" -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl "-DPOLYPHASE_PATH=$Engine" | Out-Null
& $cmake --build $BuildDir
exit $LASTEXITCODE
