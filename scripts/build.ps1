#Requires -Version 5.1
<#
.SYNOPSIS
  Builds ShowFavicon (Release) with the MSYS2 MinGW-w64 toolchain (CMake + Ninja).
#>
$ErrorActionPreference = 'Stop'

$mingw = 'C:\msys64\mingw64\bin'
if (Test-Path $mingw) {
    $env:Path = "$mingw;$env:Path"
}

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build'

cmake -S $root -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build $build
exit $LASTEXITCODE
