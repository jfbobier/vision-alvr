# Runs the host encoder bench while sampling the NVIDIA GPU; prints clocks/pstate during the busy window.
param([string]$Extra = '', [int]$Frames = 150, [int]$SleepMs = 0, [string]$Label = 'bench', [switch]$Decode)
$ErrorActionPreference = 'Continue'
$root = "$env:USERPROFILE\openxr"
$csv = "$root\logs\gpu_$Label.csv"
$smi = Start-Process -FilePath nvidia-smi -ArgumentList "--query-gpu=pstate,clocks.sm,clocks.video,utilization.gpu,utilization.encoder,utilization.decoder,power.draw --format=csv,noheader -lms 250" -RedirectStandardOutput $csv -PassThru -WindowStyle Hidden
$dec = $null
if ($Decode) {
  # concurrent NVDEC load: decode the NVENC test stream in a loop on the same GPU
  $ff = (Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Gyan.FFmpeg_*\ffmpeg-*\bin\ffmpeg.exe" | Select-Object -First 1).FullName
  $file = "$root\out\nvenc\hevc10_30_fix10.hevc"
  $cmdline = "1..2000 | ForEach-Object { & '$ff' -nostdin -v error -hwaccel cuda -i '$file' -f null - }"
  $dec = Start-Process -FilePath powershell -ArgumentList "-NoProfile","-Command",$cmdline -PassThru -WindowStyle Hidden
  Start-Sleep -Seconds 4
}
Start-Sleep -Seconds 1
$exe = "$root\src\ALVR-v20.14.1\target\release\alvr_host.exe"
$sess = (Get-ChildItem "$root\runs\*_diag_live_gpu\session.json" | Select-Object -Last 1).FullName
$args = "--config-dir $root\runs\bench\cfg --session $sess --live --width 3552 --height 1600 --bench $Frames --bench-sleep-ms $SleepMs $Extra"
$out = & cmd /c "`"$exe`" $args 2>&1" | Select-String '"bench"'
Start-Sleep -Seconds 1
Stop-Process -Id $smi.Id -Force
if ($dec) { Stop-Process -Id $dec.Id -Force -ErrorAction SilentlyContinue; taskkill /f /im ffmpeg.exe 2>&1 | Out-Null; Write-Host "[$Label] decoder stderr: $((Get-Content "$root\logs\ffdec_$Label.txt" -ErrorAction SilentlyContinue | Select-Object -First 2) -join ' | ')" }
Write-Host "[$Label] $out"
$rows = Get-Content $csv | Where-Object { $_ -match '^P\d' }
$busy = $rows | Where-Object { ($_ -split ',')[3] -notmatch '^ 0 %' -or ($_ -split ',')[4] -notmatch '^ 0 %' }
Write-Host "[$Label] busy samples: $($busy.Count) of $($rows.Count)"
$both = $rows | Where-Object { ($_ -split ',')[4] -notmatch '^ 0 %' -and ($_ -split ',')[5] -notmatch '^ 0 %' }
Write-Host "[$Label] samples with BOTH encoder and decoder active: $($both.Count)"
$both | Select-Object -First 3 | ForEach-Object { Write-Host "[$Label]  both: $_" }
$busy | Select-Object -Skip 2 -First 3 | ForEach-Object { Write-Host "[$Label]  $_" }
