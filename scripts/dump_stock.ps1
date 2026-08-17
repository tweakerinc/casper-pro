# Read-only full 16MB flash dump for Xteink X4 Pro (ESP32-S3).
# Does NOT erase or write firmware. Safe to run on stock devices.
#
# Usage:
#   .\scripts\dump_stock.ps1 -Port COM15
#   .\scripts\dump_stock.ps1 -Port COM15 -Baud 115200

param(
  [Parameter(Mandatory = $true)][string]$Port,
  [int]$Baud = 115200
)

$ErrorActionPreference = "Stop"
# scripts/ → project root
$root = Split-Path $PSScriptRoot -Parent
$outDir = Join-Path $root "stock-firmware"
$parts = Join-Path $outDir "parts"
New-Item -ItemType Directory -Force -Path $parts | Out-Null

$final = Join-Path $outDir "x4pro_stock_full_16MB.bin"
$chunkSize = 0x40000  # 256 KB — USB-Serial/JTAG drops larger continuous reads
$total = 0x1000000

Write-Host "Probing $Port ..."
python -m esptool --chip esp32s3 --port $Port flash-id

$ok = $true
for ($off = 0; $off -lt $total; $off += $chunkSize) {
  $partFile = Join-Path $parts ("part_{0:X8}.bin" -f $off)
  if ((Test-Path $partFile) -and ((Get-Item $partFile).Length -eq $chunkSize)) {
    Write-Host ("SKIP 0x{0:X}" -f $off)
    continue
  }
  $tries = 0
  $done = $false
  while (-not $done -and $tries -lt 6) {
    $tries++
    Write-Host ("READ 0x{0:X} try={1} ({2}/16 MB)" -f $off, $tries, [math]::Round($off / 1MB, 2))
    Remove-Item $partFile -ErrorAction SilentlyContinue
    & python -m esptool --chip esp32s3 --port $Port --baud $Baud --no-stub --after no_reset `
      read-flash $off $chunkSize $partFile 2>&1 | Out-Null
    if ((Test-Path $partFile) -and ((Get-Item $partFile).Length -eq $chunkSize)) {
      $done = $true
    } else {
      Start-Sleep -Seconds 1
    }
  }
  if (-not $done) {
    Write-Error ("Failed at offset 0x{0:X}" -f $off)
    $ok = $false
    break
  }
}

if (-not $ok) { exit 1 }

Write-Host "Concatenating..."
$fs = [System.IO.File]::Open($final, [System.IO.FileMode]::Create)
try {
  for ($off = 0; $off -lt $total; $off += $chunkSize) {
    $b = [System.IO.File]::ReadAllBytes((Join-Path $parts ("part_{0:X8}.bin" -f $off)))
    $fs.Write($b, 0, $b.Length)
  }
} finally { $fs.Close() }

$len = (Get-Item $final).Length
$sha = (Get-FileHash $final -Algorithm SHA256).Hash
Write-Host "SIZE=$len SHA256=$sha"
if ($len -ne 16777216) { Write-Error "Unexpected size"; exit 1 }

$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
Copy-Item $final (Join-Path $outDir "x4pro_stock_full_16MB_$stamp.bin")
Write-Host "OK: $final"
Write-Host "Copy: stock-firmware\x4pro_stock_full_16MB_$stamp.bin"
