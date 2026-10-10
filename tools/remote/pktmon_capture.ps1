# Captures the ALVR stream (UDP 9944) on the PC for N seconds with pktmon and converts it to pcapng for
# tools/alvr_pcap_report.py. 160-byte snaplen keeps the shard prefix and the bincode headers (all we parse).
#   powershell -ExecutionPolicy Bypass -File tools\remote\pktmon_capture.ps1 -Seconds 120 -Name steamvr_cyberpunk
param([int]$Seconds = 120, [string]$Name = 'capture', [string]$OutDir = "$PSScriptRoot\..\..\logs\pcap", [string]$Comp = 'nics')
$ErrorActionPreference = 'Continue'
$out = (Resolve-Path (New-Item -ItemType Directory -Force $OutDir)).Path
pktmon stop 2>&1 | Out-Null
pktmon filter remove 2>&1 | Out-Null
pktmon filter add -p 9944 | Out-Null
pktmon start -c --comp $Comp --pkt-size 160 -s 2048 -f "$out\$Name.etl" | Out-Null
Write-Host "capturing $Seconds s -> $out\$Name.etl"
Start-Sleep -Seconds $Seconds
pktmon stop | Out-Null
pktmon etl2pcap "$out\$Name.etl" -o "$out\$Name.pcapng" | Out-Null
Remove-Item "$out\$Name.etl" -ErrorAction SilentlyContinue
Get-Item "$out\$Name.pcapng" | Select-Object FullName, Length, LastWriteTime
Write-Host "CAPTURE_DONE"
