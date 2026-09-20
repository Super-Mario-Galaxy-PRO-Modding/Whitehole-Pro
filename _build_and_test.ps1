$ErrorActionPreference = 'Stop'
$cd = 'c:\Users\conta\Documents\GitHub\Whitehole-Pro'
Set-Location $cd

$msb = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'

Write-Host "Building whitehole-pro (main app)..."
& $msb /nologo /verbosity:minimal /maxcpucount /property:Configuration=Release /property:Platform=x64 "build\WhiteholePro.slnx" /target:whitehole-pro /t:Rebuild 2>&1 | Out-File _build_app.txt -Encoding utf8

Get-Content _build_app.txt | ForEach-Object { Write-Host "  $_" }

if (Test-Path 'build\Release\whitehole-pro.exe') {
    $info = Get-Item 'build\Release\whitehole-pro.exe'
    Write-Host "EXE: $($info.LastWriteTime)"
} else {
    Write-Host "EXE not found!"
}
