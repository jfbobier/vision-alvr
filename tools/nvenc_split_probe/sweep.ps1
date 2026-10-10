# Sweeps split_probe.exe over the configurations that decide whether NVENC splits a frame (see split_probe.cpp).
# powershell -ExecutionPolicy Bypass -File sweep.ps1 [-Exe split_probe.exe] [-Out sweep.jsonl]   (~5 min, keep the GPU idle)
param([string]$Exe = "$PSScriptRoot\split_probe.exe", [string]$Out = "$PSScriptRoot\sweep.jsonl")
$exe = $Exe; $out = $Out
Remove-Item $out -ErrorAction SilentlyContinue
function Probe($a) { $l = & $exe @a 2>&1 | Select -Last 1; Add-Content $out $l }
# A: our frame, every preset x tuning x split mode
foreach ($p in 1..7) { foreach ($t in 'hq','ll','ull') { foreach ($s in 0,1,2,3,15) { Probe @('--w','4224','--h','1664','--preset',"$p",'--tuning',$t,'--split',"$s",'--frames','60') } } }
# B: auto mode vs resolution (P3 low latency, like the session), plus forced 3 for the time it buys
foreach ($wh in '4224x1664','4224x1920','4224x2112','4224x2400','3840x2160','4096x2160','4800x1952','5376x2176','7104x3200','2560x1440','1920x1080') {
  $w,$h = $wh -split 'x'
  foreach ($s in 0,3) { Probe @('--w',$w,'--h',$h,'--preset','3','--tuning','ll','--split',"$s",'--frames','60') }
}
# C: bit depth, weighted prediction, AV1
foreach ($s in 0,2,3) { Probe @('--w','4224','--h','1664','--preset','3','--tuning','ll','--split',"$s",'--bits','8','--frames','60') }
foreach ($s in 2,3) { Probe @('--w','4224','--h','1664','--preset','3','--tuning','ll','--split',"$s",'--wp','1','--frames','60') }
foreach ($s in 0,2,3,15) { Probe @('--w','4224','--h','1664','--preset','3','--tuning','ll','--split',"$s",'--codec','av1','--frames','60') }
"done: " + (Get-Content $out).Count + " runs"
