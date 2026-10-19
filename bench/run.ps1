# The benchmarks of M6.5 part 4: a numeric loop, calls, field access and a JadeGate-style
# simulation slice. Each times its own work (not astra.exe starting) and prints a check value;
# this runs each a few times and takes the best. With -Save, the times are kept as the
# baseline; afterwards each run is shown against it.
#
#   powershell -ExecutionPolicy Bypass -File bench\run.ps1 [-Save] [-Runs 5]

param([string]$Astra = (Join-Path $PSScriptRoot '..\build\astra.exe'), [int]$Runs = 5, [switch]$Save)

$baselineFile = Join-Path $PSScriptRoot 'baseline.txt'
$baseline = @{}
if (Test-Path $baselineFile) {
    foreach ($line in Get-Content $baselineFile) {
        $parts = $line -split ' '
        if ($parts.Count -ge 3) { $baseline[$parts[0]] = @{ Seconds = [double]$parts[1]; Check = ($parts[2..($parts.Count - 1)] -join ' ') } }
    }
}

$results = @()
foreach ($bench in Get-ChildItem (Join-Path $PSScriptRoot '*.astra') | Sort-Object Name) {
    $best = $null
    $check = $null
    for ($r = 0; $r -lt $Runs; $r++) {
        $out = (& $Astra $bench.FullName 2>&1 | Select-Object -Last 1) -split ' '
        $seconds = [double]$out[-1]
        $check = $out[1..($out.Count - 2)] -join ' '
        if ($null -eq $best -or $seconds -lt $best) { $best = $seconds }
    }
    $name = $bench.BaseName
    $line = "{0,-15} {1,8:N1} ms   {2}" -f $name, ($best * 1000), $check
    if ($baseline.ContainsKey($name)) {
        $was = $baseline[$name]
        $line += "   ({0:N2}x the baseline's speed{1})" -f ($was.Seconds / $best), $(if ($was.Check -ne $check) { '; the check value DIFFERS: ' + $was.Check } else { '' })
    }
    $line
    $results += "$name $best $check"
}
if ($Save) {
    [IO.File]::WriteAllLines($baselineFile, $results)
    "saved as the baseline"
}
