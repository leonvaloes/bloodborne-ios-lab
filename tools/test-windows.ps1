param([ValidateSet('Debug','Release')][string]$Configuration = 'Debug')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$portable = Join-Path (Split-Path $root -Parent) '.tools/w64devkit/bin'
if (Test-Path (Join-Path $portable 'cmake.exe')) {
    $env:PATH = "$portable;$env:PATH"
    $generator = 'MinGW Makefiles'
} else { $generator = 'Ninja' }
$build = Join-Path $root "build-$Configuration"
& cmake -S $root -B $build -G $generator "-DCMAKE_BUILD_TYPE=$Configuration"
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
& cmake --build $build --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
& ctest --test-dir $build --output-on-failure --output-junit test-results.xml
if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
& (Join-Path $build 'bb-smoke.exe')
if ($LASTEXITCODE -ne 0) { throw 'Smoke runner failed' }
