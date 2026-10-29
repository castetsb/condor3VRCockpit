#Requires -RunAsAdministrator
# Removes every registration of this layer from the 32-bit implicit layer list.

$key = "HKLM:\SOFTWARE\WOW6432Node\Khronos\OpenXR\1\ApiLayers\Implicit"
if (Test-Path $key) {
    (Get-Item $key).Property | Where-Object { $_ -like "*XR_APILAYER_CONDOR3VRCOCKPIT_hands.json" } | ForEach-Object {
        Remove-ItemProperty -Path $key -Name $_
        Write-Host "Removed: $_"
    }
}
