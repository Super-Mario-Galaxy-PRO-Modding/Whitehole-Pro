$exePath = 'c:\Users\conta\Documents\GitHub\Whitehole-Pro\build\Release\whitehole_core_tests.exe'
if (Test-Path $exePath) {
    $f = Get-Item $exePath
    Write-Host "exe modified: $($f.LastWriteTime)"
} else {
    Write-Host "exe not found"
}

Write-Host "=== _build_core.txt ==="
Get-Content 'c:\Users\conta\Documents\GitHub\Whitehole-Pro\_build_core.txt' | Select-Object -Last 3

Write-Host "=== _build_tests.txt ==="
Get-Content 'c:\Users\conta\Documents\GitHub\Whitehole-Pro\_build_tests.txt' | Select-Object -Last 5
