param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot 'build')
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$deps = Join-Path $root '.deps'
$minHook = Join-Path $deps 'minhook'
$openXr = Join-Path $deps 'OpenXR-SDK'
$vsDevCmd = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat'

if (-not (Test-Path -LiteralPath $vsDevCmd)) {
    throw 'Visual Studio 2022 with Desktop C++ was not found.'
}
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    throw 'Git was not found in PATH.'
}

New-Item -ItemType Directory -Force -Path $deps, $BuildDirectory, (Join-Path $root 'bin') | Out-Null
if (-not (Test-Path -LiteralPath (Join-Path $minHook '.git'))) {
    git -c http.sslBackend=openssl clone --depth 1 --branch v1.3.4 https://github.com/TsudaKageyu/minhook.git $minHook
}
if (-not (Test-Path -LiteralPath (Join-Path $openXr '.git'))) {
    git -c http.sslBackend=openssl clone --depth 1 --branch release-1.1.60 https://github.com/KhronosGroup/OpenXR-SDK.git $openXr
}

$ninja = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
if (-not (Test-Path -LiteralPath $ninja)) { throw 'Ninja from Visual Studio was not found.' }

$cmakeCommand = @(
    'call "' + $vsDevCmd + '" -no_logo -arch=x64 -host_arch=x64',
    'cmake -S "' + $root + '" -B "' + $BuildDirectory + '" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM="' + $ninja + '" -DNFSHEAT_VR_MINHOOK_DIR="' + $minHook + '" -DNFSHEAT_VR_OPENXR_DIR="' + $openXr + '"',
    'cmake --build "' + $BuildDirectory + '" --target NFSHeatVRRuntime NFSHeatVRLauncher NFSHeatVRControl --parallel 4'
) -join ' && '

& cmd.exe /d /c $cmakeCommand
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE." }
Copy-Item -LiteralPath (Join-Path $BuildDirectory 'NFSHeatVRRuntime.dll') -Destination (Join-Path $root 'bin\NFSHeatVRRuntime.dll') -Force
Copy-Item -LiteralPath (Join-Path $BuildDirectory 'NFSHeatVRLauncher.exe') -Destination (Join-Path $root 'bin\NFSHeatVRLauncher.exe') -Force
Copy-Item -LiteralPath (Join-Path $BuildDirectory 'NFSHeatVRControl.exe') -Destination (Join-Path $root 'bin\NFSHeatVRControl.exe') -Force
Write-Host 'Release build is ready in bin.'
