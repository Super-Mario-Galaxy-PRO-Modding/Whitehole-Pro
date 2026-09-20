Get-Content '_ml_head.txt' | Set-Content '_ml_head_utf8.txt' -Encoding utf8
$lines = Get-Content '_ml_head_utf8.txt'
$hit = Select-String -Path '_ml_head_utf8.txt' -Pattern 'findModelEntry' | Select-Object -First 1
"first findModelEntry at line $($hit.LineNumber)" | Set-Content _ml_find.txt -Encoding utf8
$lines[($hit.LineNumber-1)..([Math]::Min($hit.LineNumber+90, $lines.Count-1))] | Add-Content _ml_find.txt -Encoding utf8

