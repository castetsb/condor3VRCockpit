# Builds condorhands even while it is running: Windows lets a running exe be renamed, so the
# running copy is moved aside and the new one linked in its place.
param([string]$Config = "RelWithDebInfo")

$root = Split-Path $PSScriptRoot -Parent
$bin = Join-Path $root "build\app\$Config"
$cmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if (-not (Test-Path $cmake)) { $cmake = "cmake" }

Remove-Item (Join-Path $bin "condorhands.old.exe") -ErrorAction SilentlyContinue
if (Test-Path (Join-Path $bin "condorhands.exe")) {
    Rename-Item (Join-Path $bin "condorhands.exe") "condorhands.old.exe" -ErrorAction SilentlyContinue
}
& $cmake --build (Join-Path $root "build") --config $Config --target condorhands
