$exe = 'c:\Users\conta\Documents\GitHub\Whitehole-Pro\build\Release\whitehole_core_tests.exe'
if (Test-Path $exe) {
    $info = Get-Item $exe
    Write-Host "EXE: exists"
    Write-Host "Size: $("{0:N0}" -f $info.Length) bytes"
    Write-Host "Modified: $($info.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss.fff'))"
} else {
    Write-Host "EXE: NOT FOUND"
}

# Also check build output
if (Test-Path 'c:\Users\conta\Documents\GitHub\Whitehole-Pro\_build_output.txt') {
    $lines = Get-Content 'c:\Users\conta\Documents\GitHub\Whitehole-Pro\_build_output.txt'
    Write-Host "Build output lines: $($lines.Count)"
    Write-Host "Last 5 lines:"
    $lines | Select-Object -Last 5 | ForEach-Object { Write-Host "  $_" }
}
