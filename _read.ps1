param([string]$File, [int]$Start = 1, [int]$End = 0)
$lines = Get-Content $File
if ($End -le 0) { $End = $lines.Count }
for ($i = $Start - 1; $i -lt $End; $i++) {
    if ($i -ge $lines.Count) { break }
    "{0,5}: {1}" -f ($i + 1), $lines[$i]
}
