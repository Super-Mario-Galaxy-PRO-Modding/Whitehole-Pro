Write-Host "=== build_core.lib ==="
$lib = Get-Item 'c:\Users\conta\Documents\GitHub\Whitehole-Pro\build\Release\whitehole_core.lib' -ErrorAction SilentlyContinue
if ($lib) { Write-Host "modified: $($lib.LastWriteTime)" } else { Write-Host "not found" }

Write-Host "=== whitehole_core_tests.exe ==="
$exe = Get-Item 'c:\Users\conta\Documents\GitHub\Whitehole-Pro\build\Release\whitehole_core_tests.exe' -ErrorAction SilentlyContinue
if ($exe) { Write-Host "modified: $($exe.LastWriteTime)" } else { Write-Host "not found" }

Write-Host "=== _build_core.txt (full) ==="
Get-Content 'c:\Users\conta\Documents\GitHub\Whitehole-Pro\_build_core.txt'

Write-Host "=== _build_tests.txt (full) ==="
Get-Content 'c:\Users\conta\Documents\GitHub\Whitehole-Pro\_build_tests.txt'
