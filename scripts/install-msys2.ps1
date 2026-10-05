#Requires -Version 5.1
<#
.SYNOPSIS
  Installs MSYS2 plus the MinGW-w64 (x86_64) toolchain on Windows.

.DESCRIPTION
  - Installs MSYS2 if it is not already present (direct download of the
    official installer, silent mode, into C:\msys64 by default).
  - Updates the base system (pacman -Syu).
  - Installs: mingw-w64-x86_64-toolchain (gcc/g++/gdb/make), cmake, ninja.
  - Adds <root>\mingw64\bin to the user PATH.
  - Idempotent: safe to run again.

.PARAMETER InstallRoot
  Where MSYS2 should be installed. Default: C:\msys64

.PARAMETER SkipPath
  Do not modify the user PATH.
#>
[CmdletBinding()]
param(
    [string]$InstallRoot = 'C:\msys64',
    [switch]$SkipPath
)

$ErrorActionPreference = 'Stop'

# PowerShell 5.1 on Windows 11 defaults to TLS 1.0 for Invoke-WebRequest/RestMethod.
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

function Write-Step {
    param([string]$Message)
    Write-Host "`n==> $Message" -ForegroundColor Cyan
}

function Find-MsysRoot {
    $candidates = @(
        $InstallRoot,
        'C:\msys64',
        'C:\Program Files\msys64',
        (Join-Path $env:LOCALAPPDATA 'Programs\msys64'),
        (Join-Path $env:LOCALAPPDATA 'msys64')
    )
    foreach ($c in $candidates) {
        if ($c -and (Test-Path (Join-Path $c 'usr\bin\bash.exe'))) {
            return $c
        }
    }
    return $null
}

# ---- 1. Locate or install MSYS2 -------------------------------------------
$msysRoot = Find-MsysRoot

if ($msysRoot) {
    Write-Step "MSYS2 already installed: $msysRoot"
}
else {
    Write-Step 'Installing MSYS2 (silent, this downloads ~100 MB)'

    $installed = $false

    # Preferred: download the official installer and run it silently with a
    # known root. Deterministic and avoids winget silent-install quirks.
    try {
        $release = Invoke-RestMethod -Uri 'https://api.github.com/repos/msys2/msys2-installer/releases/latest'
        $asset = $release.assets |
            Where-Object { $_.name -match '^msys2-x86_64-.*\.exe$' } |
            Select-Object -First 1
        if ($asset) {
            $installer = Join-Path $env:TEMP $asset.name
            Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $installer
            Write-Step "Running installer into $InstallRoot"
            Start-Process -FilePath $installer `
                -ArgumentList @('in', '--confirm-command', '--root', $InstallRoot,
                               '--accept-messages', '--accept-licenses') `
                -Wait
            Remove-Item $installer -ErrorAction SilentlyContinue
            $installed = $true
        }
    }
    catch {
        Write-Host "Installer download failed: $_" -ForegroundColor Yellow
    }

    $msysRoot = Find-MsysRoot

    # Fallback: winget.
    if (-not $msysRoot) {
        Write-Step 'Falling back to winget'
        $winget = Get-Command winget -ErrorAction SilentlyContinue
        if (-not $winget) { throw 'MSYS2 not found and winget is unavailable.' }
        winget install --id MSYS2.MSYS2 -e --silent `
            --accept-source-agreements --accept-package-agreements
        if ($LASTEXITCODE -ne 0) { throw "winget install failed (exit $LASTEXITCODE)." }
        $msysRoot = Find-MsysRoot
    }

    if (-not $msysRoot) { throw 'MSYS2 finished but the install root was not found.' }
}

$bash = Join-Path $msysRoot 'usr\bin\bash.exe'
$mingwBin = Join-Path $msysRoot 'mingw64\bin'
$gpp = Join-Path $mingwBin 'g++.exe'

# ---- 2. Update the base system --------------------------------------------
# Separate invocation: pacman may update bash/pacman itself; a fresh shell
# then picks the new binaries up.
Write-Step 'Updating MSYS2 (pacman -Syu)'
& $bash -lc 'pacman -Syu --noconfirm'
if ($LASTEXITCODE -ne 0) { throw "pacman -Syu failed (exit $LASTEXITCODE)." }

# ---- 3. Install the MinGW-w64 toolchain -----------------------------------
Write-Step 'Installing MinGW-w64 x86_64 toolchain (gcc/g++/make), cmake, ninja'
& $bash -lc 'pacman -S --needed --noconfirm mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja'
if ($LASTEXITCODE -ne 0) { throw "Toolchain install failed (exit $LASTEXITCODE)." }

# ---- 4. Verify ------------------------------------------------------------
Write-Step 'Verifying compiler'
& $gpp '--version'
if ($LASTEXITCODE -ne 0) { throw "g++ not found at $gpp" }

# ---- 5. PATH --------------------------------------------------------------
if (-not $SkipPath) {
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    if (-not $userPath) { $userPath = '' }
    if ($userPath -notlike "*$mingwBin*") {
        $newPath = $userPath.TrimEnd(';') + ';' + $mingwBin
        [Environment]::SetEnvironmentVariable('Path', $newPath, 'User')
        Write-Host "Added to user PATH: $mingwBin" -ForegroundColor Green
        Write-Host 'Restart the terminal (or VS Code) so the new PATH takes effect.' -ForegroundColor Yellow
    }
    else {
        Write-Host "Already on user PATH: $mingwBin" -ForegroundColor Green
    }
    $env:Path = "$mingwBin;$env:Path"
}

Write-Host "`nDone. MSYS2 root: $msysRoot" -ForegroundColor Green
Write-Host "Compiler: $gpp"
