# Runs the quality benchmark (benchmark phase 2, headless) on the builder: alvr_host --benchmark-quality, events to a log file.
param([string]$Label = 'bq', [string]$Plan = 'full', [int]$Frames = 90, [int]$Warmup = 20, [int]$Every = 10, [int]$Stride = 1, [int]$Mbps = 0, [int]$Debug = 0, [int]$RefScale = 1)
$ErrorActionPreference = 'Continue'
$root = if ($env:VISIONALVR_ROOT) { $env:VISIONALVR_ROOT } else { "$env:USERPROFILE\openxr" }
$run = "$root\runs\$Label"
New-Item -ItemType Directory -Force "$run\cfg" | Out-Null
$exe = "$root\src\ALVR-v20.14.1\target\release\alvr_host.exe"
$env:VISIONALVR_ALLOW_ANY_GPU = '1'
if ($Debug -gt 0) { $env:VISIONALVR_BENCH_DEBUG = "$Debug" }
$args = "--config-dir $run\cfg --session $run\session.json --force-session --benchmark-quality --bench-scene $root\bench\littlest_tokyo.vab --bq-out $run\bq.json --bq-preview $run\preview.png --bq-plan $Plan --bq-frames $Frames --bq-warmup $Warmup --bq-metric-every $Every --bq-stride $Stride --bq-ref-scale $RefScale"
if ($Mbps -gt 0) { $args += " --bench-mbps $Mbps" }
$t = Get-Date
& cmd /c "`"$exe`" $args > `"$run\stdout.jsonl`" 2> `"$run\stderr.txt`""
Write-Host "exit $LASTEXITCODE after $([int]((Get-Date) - $t).TotalSeconds) s"
Write-Host "RESULT: DONE"
