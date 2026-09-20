$exe = 'c:\Users\conta\Documents\GitHub\Whitehole-Pro\build\Release\whitehole-pro.exe'
$f = Get-Item $exe -ErrorAction SilentlyContinue
if ($f) {
    Write-Host "whitehole-pro.exe: $($f.LastWriteTime)"
    Write-Host "Size: $($f.Length) bytes"
} else {
    Write-Host "whitehole-pro.exe NOT FOUND"
}

# Check build app log
if (Test-Path 'c:\Users\conta\Documents\GitHub\Whitehole-Pro\_build_app.txt') {
    $lines = Get-Content 'c:\Users\conta\Documents\GitHub\Whitehole-Pro\_build_app.txt'
    Write-Host "Build log lines: $($lines.Count)"
    Write-Host "Last 5:"
    $lines | Select-Object -Last 5 | ForEach-Object { Write-Host "  $_" }
}
