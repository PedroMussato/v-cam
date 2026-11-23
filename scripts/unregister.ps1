# Unregisters and removes VCamSource.dll. Run as Administrator. Close WinVCam.exe first.
$ErrorActionPreference = 'Stop'

$dest = "$env:ProgramFiles\WinVCam"
$dll  = "$dest\VCamSource.dll"
if (Test-Path $dll) {
    & "$env:SystemRoot\System32\regsvr32.exe" /u /s $dll
    Remove-Item $dll -Force -ErrorAction SilentlyContinue
}
Remove-Item $dest -Force -ErrorAction SilentlyContinue
Write-Host "Unregistered."
