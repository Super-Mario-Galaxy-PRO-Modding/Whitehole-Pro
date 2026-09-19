$g = 'C:\Program Files\Git\cmd\git.exe'
$out = @()
foreach ($rev in @('HEAD', 'HEAD~1', 'HEAD~2', 'HEAD~3', 'HEAD~4', 'HEAD~5')) {
    foreach ($fn in @('bcsvSetCommand', 'zoneListCommand', 'mapParamsCommand', 'objectdbQueryCommand')) {
        $hits = & $g --no-pager grep -n "int $fn(int" $rev -- cpp 2>$null
        if ($hits) { $out += "$rev $fn => $hits" } else { $out += "$rev $fn => NONE" }
    }
}
$out | Out-File -Encoding utf8 _probe.txt
