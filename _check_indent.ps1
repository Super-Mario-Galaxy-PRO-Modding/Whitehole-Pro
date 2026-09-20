$lines = Get-Content 'c:\Users\conta\Documents\GitHub\Whitehole-Pro\cpp\tests\core_tests.cpp'
foreach ($line in $lines[1925..1950]) {
    Write-Host $line
}
