$f = "$env:USERPROFILE\openxr\logs\smoke3_out.txt"
Write-Host "lines total:" (Get-Content $f).Count
Get-Content $f | Where-Object { $_ -notmatch 'Verbose|ApiLayers' } | Select-Object -Skip 6
