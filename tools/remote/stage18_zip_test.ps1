# "Unzip and run": extracts out\dist\VisionALVR-<version>.zip into a fresh folder and checks the portable layout from the zip
# itself: nothing but the shipped files, logs\ created on first run, the general log appended by every run, one debug session
# folder per run with debug on, config seeded, the quality benchmark finding its scene. Never touches the registry.
$ErrorActionPreference = 'Continue'
$root = "$env:USERPROFILE\openxr"
$zip = Get-ChildItem "$root\out\dist\VisionALVR-*.zip" | Select-Object -First 1
$t = "$root\runs\ziptest\$((Get-Date).ToString('yyyyMMdd-HHmmss'))"
New-Item -ItemType Directory -Force $t | Out-Null
Expand-Archive -Path $zip.FullName -DestinationPath $t
$d = "$t\VisionALVR"
$fails = 0
function Check($name, $ok, $detail) { if (-not $ok) { $script:fails++ }; Write-Host ("CHECK {0} {1}: {2}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $name, $detail) }
$top = (Get-ChildItem $t | ForEach-Object Name) -join ','
Check 'single_top_folder' ($top -eq 'VisionALVR') $top
Check 'no_logs_before_first_run' (-not (Test-Path "$d\logs")) ''
Check 'no_user_config_shipped' (-not (Test-Path "$d\config\session.json")) ''
$runs = @('--debug', '', '--debug')
foreach ($r in $runs) {
  & cmd /c "`"$d\alvr_host.exe`" --install-dir `"$d`" --benchmark-quality --bq-plan quick --bq-frames 10 --bq-warmup 5 $r > `"$t\run.txt`" 2>&1"
  Start-Sleep -Seconds 1
}
$log = Get-Content "$d\logs\VisionALVR.log" -ErrorAction SilentlyContinue
$starts = @($log | Select-String 'host start').Count
Check 'general_log_appended' ($starts -eq 3) "$starts host start lines"
$bq = @($log | Select-String 'quality benchmark:').Count
Check 'benchmark_in_general_log' ($bq -ge 3) "$bq lines"
$dbg = @(Get-ChildItem "$d\logs\debug" -Directory -ErrorAction SilentlyContinue)
Check 'one_debug_folder_per_debug_run' ($dbg.Count -eq 2) (($dbg | ForEach-Object Name) -join ',')
$ev = @($dbg | Where-Object { Test-Path "$($_.FullName)\host_events.jsonl" })
Check 'debug_folder_has_events' ($ev.Count -eq 2) "$($ev.Count) with host_events.jsonl"
Check 'session_seeded' (Test-Path "$d\config\session.json") ''
Check 'benchmark_result_saved' (Test-Path "$d\config\benchmark_quality.json") ''
Write-Host "folder: $d"
if ($fails -eq 0) { Write-Host 'RESULT: PASS' } else { Write-Host "RESULT: FAIL ($fails)" }
