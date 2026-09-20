$log = 'c:\Users\conta\Documents\GitHub\Whitehole-Pro\_build_app.txt'
$f = Get-Item $log -ErrorAction SilentlyContinue
if ($f) {
    Write-Host "log size: $($f.Length) bytes"
    Write-Host "last write: $($f.LastWriteTime)"
}

# Check the exe
$exe = 'c:\Users\conta\Documents\GitHub\Whitehole-Pro\build\Release\whitehole-pro.exe'
$e = Get-Item $exe -ErrorAction SilentlyContinue
if ($e) {
    Write-Host "exe: $($e.LastWriteTime)"
    Write-Host "exe size: $($e.Length) bytes"
}

# Check MSBuild processes
$procs = Get-Process -Name MSBuild -ErrorAction SilentlyContinue
Write-Host "MSBuild processes: $($procs.Count)"
