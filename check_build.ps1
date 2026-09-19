$bytes = [System.IO.File]::ReadAllBytes('C:\Users\conta\Documents\GitHub\Whitehole-Pro\build_output.txt')
$content = [System.Text.Encoding]::Unicode.GetString($bytes)
if ($content -match 'Build succeeded') {
    Write-Host 'BUILD_SUCCEEDED'
} elseif ($content -match 'error C[0-9]+') {
    Write-Host 'BUILD_FAILED'
    $lines = $content -replace '[\x00]', '' -split '\r\n'
    foreach ($line in $lines) {
        if ($line -match 'error C[0-9]+') {
            Write-Host $line
        }
    }
} else {
    Write-Host 'BUILD_IN_PROGRESS_OR_UNKNOWN'
}