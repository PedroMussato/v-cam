# Installs VCamSource.dll to a machine-wide location and registers it as a COM server (HKLM).
# MUST be run as Administrator. The Windows Frame Server (LocalService) loads the DLL, so it
# cannot live in your user profile.
$ErrorActionPreference = 'Stop'

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this script from an elevated (Administrator) PowerShell."
}

$root = Split-Path -Parent $PSScriptRoot
$src  = "$root\VCamSource\build\Release\VCamSource.dll"
if (-not (Test-Path $src)) { throw "VCamSource.dll not found. Run scripts\build.ps1 first." }

$dest = "$env:ProgramFiles\WinVCam"
New-Item -ItemType Directory -Force -Path $dest | Out-Null
Copy-Item $src "$dest\VCamSource.dll" -Force

& "$env:SystemRoot\System32\regsvr32.exe" /s "$dest\VCamSource.dll"
if ($LASTEXITCODE -ne 0) { throw "regsvr32 failed with exit code $LASTEXITCODE" }

Write-Host "Registered $dest\VCamSource.dll"
Write-Host "Now start WinVCam.exe (as Administrator) and press 'Start virtual camera'."
