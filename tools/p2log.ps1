# Tail the racket's backend log into logs\padel0.log (read by the firmware Claude through the linked folder).
#   powershell -ExecutionPolicy Bypass -File tools\p2log.ps1            # follows forever, Ctrl-C to stop
#   powershell -ExecutionPolicy Bypass -File tools\p2log.ps1 -Lines 500 # one snapshot of the last 500 lines
# Token: tools\p2_token.txt (git-ignored, GET-only backend token).
param([string]$Device = "d40592485f78", [int]$Lines = 0, [string]$Level = "I")
$root  = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$token = (Get-Content (Join-Path $root "tools\p2_token.txt") -Raw).Trim()
$api   = "https://admin.padelsense.net/p2/api"
$out   = Join-Path $root "logs\padel0.log"
New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
$h = @{ Authorization = "Bearer $token" }
if ($Lines -gt 0) {
    $r = Invoke-RestMethod -Headers $h "$api/devices/$Device/log?lines=$Lines&level=$Level"
    $r.lines | Set-Content -Encoding UTF8 $out
    Write-Host "wrote $($r.lines.Count) lines to $out"
    exit 0
}
Write-Host "streaming to $out (Ctrl-C to stop)"
while ($true) {
    try {
        & curl.exe -sN -H "Authorization: Bearer $token" "$api/devices/$Device/log/stream?tail=200&level=$Level" |
            ForEach-Object { $_ | Out-File -Append -Encoding UTF8 $out }
    } catch { Start-Sleep 2 }
    Start-Sleep 1
}
