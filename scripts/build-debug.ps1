#Requires -Version 5.1
<#
.SYNOPSIS
  Builds ShowFavicon (Debug, with debug symbols) using the MSYS2 MinGW-w64
  toolchain (CMake + Ninja) into a separate build-debug directory.
#>
$ErrorActionPreference = 'Stop'

$mingw = 'C:\msys64\mingw64\bin'
if (Test-Path $mingw) {
    $env:Path = "$mingw;$env:Path"
}

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build-debug'

cmake -S $root -B $build -G Ninja -DCMAKE_BUILD_TYPE=Debug
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build $build
exit $LASTEXITCODE
