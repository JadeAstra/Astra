# The start-up budgets of D145, measured on this machine with a release build:
#   a first run, nothing cached, checked and compiled in under half a second;
#   a second run with nothing changed in under 20 ms on top of astra.exe starting;
#   a run after a one-line change inside a function in under 100 ms.
# The program they are measured on is the standard library, Jade, the examples and the tests
# (some 20,000 lines), each test and example in a folder of its own so that their names do not
# meet, imported by one main file. It is compiled, not run (ASTRA_TIMES=compile).
#
#   powershell -ExecutionPolicy Bypass -File tests\budgets.ps1

# -Keep leaves the program and its cache in the temp folder, to be looked into.
param([string]$Astra = (Join-Path $PSScriptRoot '..\build\astra.exe'), [int]$Runs = 7, [switch]$Keep)

$root = Join-Path ([IO.Path]::GetTempPath()) 'astra-budget'
Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $root | Out-Null
# A cache of the budgets' own, so that the first run can start with nothing in it.
$cache = Join-Path ([IO.Path]::GetTempPath()) 'astra-budget-cache'
$env:ASTRA_CACHE = $cache
function Clear-Cache { Remove-Item -Recurse -Force $cache -ErrorAction SilentlyContinue }

# What the tests and examples import or start beside them.
$siblings = 'modules', 'NsShapes', 'NsText', 'workers'
$sources = @(Get-ChildItem (Join-Path $PSScriptRoot '*.astra') | Where-Object { $_.Name -notlike 'error_*' })
$sources += @(Get-ChildItem (Join-Path $PSScriptRoot '..\examples\*.astra') | Where-Object { $_.Name -notlike 'bounce_worker*' })
$main = @()
$lines = 0
for ($i = 0; $i -lt $sources.Count; $i++) {
    $folder = Join-Path $root ('part{0:d3}' -f $i)
    New-Item -ItemType Directory -Force $folder | Out-Null
    Copy-Item $sources[$i].FullName $folder
    foreach ($s in $siblings) {
        $from = Join-Path $PSScriptRoot $s
        if (Test-Path $from) { Copy-Item -Recurse $from (Join-Path $folder $s) }
    }
    $main += 'import part{0:d3}' -f $i
    $lines += @(Get-Content $sources[$i].FullName).Count
}
$mainFile = Join-Path $root 'main.astra'
[IO.File]::WriteAllText($mainFile, ($main -join "`n") + "`nprint(""ready"")`n")
foreach ($dir in (Join-Path $PSScriptRoot '..\src\std'), (Join-Path $PSScriptRoot '..\lib\Jade')) {
    $lines += @(Get-ChildItem (Join-Path $dir '*.astra') | ForEach-Object { @(Get-Content $_.FullName).Count } | Measure-Object -Sum).Sum
}

function Time-Astra([string[]]$astraArgs, [string]$times = 'compile') {
    $env:ASTRA_TIMES = $times
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $output = & $Astra @astraArgs 2>&1
    $watch.Stop()
    $env:ASTRA_TIMES = $null
    if ($LASTEXITCODE -ne 0) { throw "astra failed: $output" }
    return @{ Ms = $watch.Elapsed.TotalMilliseconds; Text = ($output | Where-Object { "$_" -like 'astra:*' }) -join ' ' }
}
function Best([scriptblock]$run) {
    $best = $null
    for ($r = 0; $r -lt $Runs; $r++) {
        $t = & $run
        if ($null -eq $best -or $t.Ms -lt $best.Ms) { $best = $t }
    }
    return $best
}

# astra.exe starting, and doing nothing.
$start = Best { Time-Astra @('--version') '' }
# A first run: nothing cached.
$first = Best { Clear-Cache; Time-Astra @($mainFile) }
# A second run with nothing changed.
$null = Time-Astra @($mainFile)
$second = Best { Time-Astra @($mainFile) }
# A run after a one-line change inside a function: a test's function body gains a line.
$changed = Join-Path $root 'part000'
$file = @(Get-ChildItem (Join-Path $changed '*.astra'))[0].FullName
$original = [IO.File]::ReadAllText($file)
$edit = Best {
    [IO.File]::WriteAllText($file, $original + "`nfun budgetProbe() : int { val n = $([Environment]::TickCount) `n return n }`n")
    Time-Astra @($mainFile)
}
[IO.File]::WriteAllText($file, $original)

"program: $($sources.Count) tests and examples, the standard library and Jade, $lines lines"
"astra.exe starting:      {0,7:N1} ms" -f $start.Ms
"first run:               {0,7:N1} ms  (budget 500)    {1}" -f $first.Ms, $first.Text
"second run, unchanged:   {0,7:N1} ms  (budget 20 on top of starting: {2:N1})    {1}" -f $second.Ms, $second.Text, ($second.Ms - $start.Ms)
"after a one-line change: {0,7:N1} ms  (budget 100)    {1}" -f $edit.Ms, $edit.Text
$env:ASTRA_CACHE = $null
if ($Keep) { "kept: $root and $cache" } else {
    Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue
    Clear-Cache
}
