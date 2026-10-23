# Check that a change keeps the output exact: runs the reference race script at 1x and 4x
# and compares every screenshot with build/n43 and build/s4, the 16:9 4x frame with
# build/hd_ws.bmp and the 1x audio with build/n43.pcm.
#   .\tools\verify.ps1 [-Build] [-Name v] [-SaveAudio]
#   -SaveAudio writes build/n43.pcm (the reference) instead of comparing with it.
param([switch]$Build, [string]$Name = "v", [switch]$SaveAudio, [string[]]$Extra = @())
Set-Location (Split-Path $PSScriptRoot)
Get-Process ctr -ErrorAction SilentlyContinue | Stop-Process -Force
if ($Build) {
    .\build.ps1 2>&1 | Select-String -Pattern "error|Built|Traceback" | Select-Object -First 10 | ForEach-Object { $_.Line }
}
$keys = "2700:down,2740:down," + ((0..13 | ForEach-Object { "$(2780 + $_ * 60):cross" }) -join ",") + ",3700:cross:3000"
function Run($argList, $timeoutSec) {
    $p = Start-Process .\ctr.exe -ArgumentList $argList -PassThru
    if (-not $p.WaitForExit($timeoutSec * 1000)) { Stop-Process $p -Force; "TIMEOUT" }
}
foreach ($cfg in @(@{n = "${Name}1"; s = 1; ref = "n43"; pg = "--no-pgxp" }, @{n = "${Name}4"; s = 4; ref = "s4"; pg = "--pgxp" })) {
    $d = "build\$($cfg.n)"
    New-Item -ItemType Directory -Force $d | Out-Null
    Get-ChildItem $d | ForEach-Object { [IO.File]::Delete($_.FullName) }
    $argList = @("--turbo", "--aspect", "4:3", "--overclock", "100", "--scale", "$($cfg.s)", $cfg.pg, "--frames", "6300",
        "--shot-every", "300", "build/$($cfg.n)", "--keys", $keys) + $Extra
    $pcm = "build/$($cfg.n).pcm"
    if ($cfg.s -eq 1) { $argList += @("--audio-dump", $pcm) }
    $t = Measure-Command { Run $argList 600 }
    $diff = 0; $tot = 0; $first = $null
    Get-ChildItem "build\$($cfg.ref)" -Filter *.bmp | ForEach-Object {
        $tot++
        $o = Join-Path $d $_.Name
        if (-not (Test-Path $o) -or (Get-FileHash $o).Hash -ne (Get-FileHash $_.FullName).Hash) { $diff++; if (-not $first) { $first = $_.Name } }
    }
    "$($cfg.n): $([int]$t.TotalSeconds)s, differing frames: $diff of $tot $(if ($first) { "(first $first)" })"
    if ($cfg.s -eq 1) {
        if ($SaveAudio) { Copy-Item $pcm build/n43.pcm -Force; "audio reference saved ($((Get-Item $pcm).Length) bytes)" }
        elseif (Test-Path build/n43.pcm) { "audio identical: $((Get-FileHash $pcm).Hash -eq (Get-FileHash build/n43.pcm).Hash) ($((Get-Item $pcm).Length) bytes)" }
        else { "no audio reference (run with -SaveAudio first)" }
    }
}
Run (@("--turbo", "--aspect", "16:9", "--scale", "4", "--pgxp", "--overclock", "100", "--frames", "4800", "--shot", "4800",
    "build/${Name}169.bmp", "--keys", $keys) + $Extra) 300
"16:9 identical: $((Get-FileHash "build/${Name}169.bmp").Hash -eq (Get-FileHash build/hd_ws.bmp).Hash)"
