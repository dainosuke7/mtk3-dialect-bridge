# COM3 のUART出力を logs/uart.log に落としつつ画面にも出す。
# 使い方: powershell -ExecutionPolicy Bypass -File scripts/log.ps1 [-Port COM3] [-Seconds 0]
# Seconds=0 で Ctrl+C まで動き続ける。
param(
    [string]$Port    = "COM3",
    [int]   $Baud    = 115200,
    [int]   $Seconds = 0,
    [string]$Out     = ""
)

if ([string]::IsNullOrEmpty($Out)) {
    $Out = Join-Path (Split-Path -Parent $PSScriptRoot) "logs\uart.log"
}
$dir = Split-Path -Parent $Out
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
$sp.Handshake   = 'None'     # フロー制御なし
$sp.ReadTimeout = 250
$sp.NewLine     = "`n"

try {
    $sp.Open()
} catch {
    Write-Error "$Port を開けません: $($_.Exception.Message)"
    exit 1
}

Write-Host "[log.ps1] $Port @ $Baud -> $Out"
$sw = New-Object System.IO.StreamWriter($Out, $true)   # 追記
$sw.AutoFlush = $true
$start = Get-Date

try {
    while ($true) {
        try {
            $s = $sp.ReadExisting()
            if ($s.Length -gt 0) {
                [Console]::Write($s)
                $sw.Write($s)
            } else {
                Start-Sleep -Milliseconds 50
            }
        } catch [TimeoutException] { }

        if ($Seconds -gt 0 -and ((Get-Date) - $start).TotalSeconds -ge $Seconds) { break }
    }
} finally {
    $sw.Close()
    $sp.Close()
    Write-Host "`n[log.ps1] closed"
}
