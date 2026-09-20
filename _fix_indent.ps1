$path = 'c:\Users\conta\Documents\GitHub\Whitehole-Pro\cpp\src\smg\bmd.cpp'
$lines = Get-Content $path
# Fix line 764 (0-indexed: 763) - should have 8 spaces indent
$lines[763] = '        const auto tag = reader.u32(position);'
Set-Content $path $lines
Write-Host "Fixed line 764"
