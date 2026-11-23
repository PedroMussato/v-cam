# Builds the native DLLs (VCamSource.dll, VCamReg.dll) and the WPF app.
# Run from a "x64 Native Tools Command Prompt for VS 2022" PowerShell, or any shell where
# cmake and dotnet are on PATH and the Windows 11 SDK (10.0.22000+) is installed.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

cmake -S "$root\VCamSource" -B "$root\VCamSource\build" -A x64
cmake --build "$root\VCamSource\build" --config Release

dotnet build "$root\VCamApp\VCamApp.csproj" -c Release -p:Platform=x64

Write-Host ""
Write-Host "Built. App: $root\VCamApp\bin\x64\Release\net8.0-windows10.0.19041.0\win-x64\WinVCam.exe"
Write-Host "Next (once, as Administrator): scripts\register.ps1"
