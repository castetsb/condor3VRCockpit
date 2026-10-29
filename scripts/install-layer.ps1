#Requires -RunAsAdministrator
# Registers the layer as an implicit OpenXR API layer for 32-bit apps (Condor.exe is 32-bit).
param([string]$Config = "RelWithDebInfo")

$manifest = Join-Path $PSScriptRoot "..\build\layer\$Config\XR_APILAYER_CONDOR3VRCOCKPIT_hands.json"
if (-not (Test-Path $manifest)) {
    Write-Error "Manifest not found: $manifest. Build first: cmake --preset win32; cmake --build --preset release"
    exit 1
}
$manifest = (Resolve-Path $manifest).Path

reg add "HKLM\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit" /v "$manifest" /t REG_DWORD /d 0 /f /reg:32
Write-Host "Installed: $manifest"
