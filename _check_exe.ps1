$f = 'c:\Users\conta\Documents\GitHub\Whitehole-Pro\build\Release\whitehole_core_tests.exe'
if (Test-Path $f) {
    $s = Get-Item $f
    Write-Host "EXE exists"
    Write-Host "Size: " $s.Length "bytes"
    Write-Host "Modified: " $s.LastWriteTime
} else {
    Write-Host "EXE not found"
}
