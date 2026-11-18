param(
    [string]$Zig = "zig"
)

$ErrorActionPreference = "Stop"
$projectRoot = $PSScriptRoot
$source = Join-Path $projectRoot "src\mouse_battery.c"
$outputDir = Join-Path $projectRoot "dist"
$output = Join-Path $outputDir "MouseBattery.exe"

New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

& $Zig cc `
    -target x86_64-windows-gnu `
    -O2 `
    -s `
    -municode `
    $source `
    -o $output `
    -lsetupapi `
    -lhid `
    -luser32 `
    -lgdi32 `
    -lshell32 `
    -ladvapi32 `
    '-Wl,--subsystem,windows'

if ($LASTEXITCODE -ne 0) {
    throw "Zig build failed with exit code $LASTEXITCODE"
}

$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash
$size = (Get-Item -LiteralPath $output).Length
Write-Host "Built $output ($size bytes)"
Write-Host "SHA-256: $hash"
