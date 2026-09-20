$obj = Get-Item 'c:\Users\conta\Documents\GitHub\Whitehole-Pro\build\whitehole_core.dir\Release\model_library.obj' -ErrorAction SilentlyContinue
if ($obj) {
    Write-Host "obj modified: $($obj.LastWriteTime)"
} else {
    Write-Host "obj not found"
}
