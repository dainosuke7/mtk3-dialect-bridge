# COM3 のUART出力を logs/uart_<日時>.log に落としつつ画面にも出す。
# 使い方: powershell -ExecutionPolicy Bypass -File scripts/log.ps1 [-Port COM3] [-Seconds 0] [-Timestamp]
# Seconds=0 で Ctrl+C まで動き続ける。
# -Timestamp を付けると各行の先頭に PC の時計 (HH:mm:ss.fff) を入れる。
# scripts/aed_play_test.py のログと同じ形式なので、対照試験の突き合わせに使える。
# 既定はオフで、オフのときの出力は今までと同じ (受信したものをそのまま書く)。
param(
    [string]$Port    = "COM3",
    [int]   $Baud    = 115200,
    [int]   $Seconds = 0,
    [string]$Out     = "",
    [switch]$Timestamp
)

# 既定の出力先は logs\uart_<日時>.log。固定名に追記すると、対照試験を何回も
# 走らせたときにどの試験の行か分からなくなるので、走るたびに別ファイルにする
# (scripts/aed_play_test.py の play_<日時>.txt と同じ日時の書き方)。
# 追記したいときや名前を決めたいときは -Out で渡す
if ([string]::IsNullOrEmpty($Out)) {
    $name = "uart_" + (Get-Date -Format "yyyyMMdd_HHmmss") + ".log"
    $Out  = Join-Path (Split-Path -Parent $PSScriptRoot) (Join-Path "logs" $name)
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

Write-Host "[log.ps1] $Port @ $Baud -> $Out$(if ($Timestamp) { ' (timestamped)' })"
$sw = New-Object System.IO.StreamWriter($Out, $true)   # 追記
$sw.AutoFlush = $true
$sw.NewLine   = "`n"                                   # -Timestamp のときも改行は LF
$start = Get-Date

# -Timestamp のときの行の組み立て。ReadExisting は行の途中で返るので、
# 改行が来るまで溜めてから 1行として書く
$buf      = ""
$lineTime = ""

try {
    while ($true) {
        try {
            $s = $sp.ReadExisting()
            if ($s.Length -gt 0) {
                if ($Timestamp) {
                    # 時刻は「その行の先頭が届いた読み取り」のもの。1回の読み取りで
                    # 複数行が届いたときは、その行たちは同じ時刻とみなす
                    $now = (Get-Date).ToString("HH:mm:ss.fff")
                    if ($buf.Length -eq 0) { $lineTime = $now }
                    $buf = $buf + $s
                    $i = $buf.IndexOf("`n")
                    while ($i -ge 0) {
                        $line = $buf.Substring(0, $i).TrimEnd("`r")
                        $buf  = $buf.Substring($i + 1)
                        $text = "$lineTime $line"
                        [Console]::WriteLine($text)
                        $sw.WriteLine($text)
                        $lineTime = $now
                        $i = $buf.IndexOf("`n")
                    }
                } else {
                    [Console]::Write($s)
                    $sw.Write($s)
                }
            } else {
                Start-Sleep -Milliseconds 50
            }
        } catch [TimeoutException] { }

        if ($Seconds -gt 0 -and ((Get-Date) - $start).TotalSeconds -ge $Seconds) { break }
    }
} finally {
    # 改行がまだ来ていない書きかけの行も捨てずに残す
    if ($Timestamp -and $buf.Length -gt 0) {
        $text = "$lineTime $buf"
        [Console]::WriteLine($text)
        $sw.WriteLine($text)
    }
    $sw.Close()
    $sp.Close()
    Write-Host "`n[log.ps1] closed"
}
