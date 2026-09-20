$ErrorActionPreference = 'Stop'
$cd = 'c:\Users\conta\Documents\GitHub\Whitehole-Pro'
Set-Location $cd

$msb = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'

Write-Host "Building whitehole_core_tests..."
$result = & $msb /nologo /verbosity:normal /property:Configuration=Release /property:Platform=x64 "build\WhiteholePro.slnx" /target:whitehole_core_tests 2>&1
Write-Host $result

if (Test-Path 'build\Release\whitehole_core_tests.exe') {
    $info = Get-Item 'build\Release\whitehole_core_tests.exe'
    Write-Host "EXE: $($info.LastWriteTime)"
} else {
    Write-Host "EXE not found!"
}

# Run tests if build succeeded
if (Test-Path 'build\Release\whitehole_core_tests.exe') {
    Write-Host "`nRunning tests..."
    $testResult = & 'build\Release\whitehole_core_tests.exe' 2>&1
    Write-Host $testResult
    
    if ($testResult -match "Test failure") {
        Write-Host "`n*** TESTS FAILED ***" -ForegroundColor Red
        exit 1
    } else {
        Write-Host "`n*** ALL TESTS PASSED ***" -ForegroundColor Green
    }
}
