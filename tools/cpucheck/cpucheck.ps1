# Runs cpucheck.astra for a while on the chosen cores, restarting it after each failure, and
# says how many times it failed. The program always computes the same thing, so it should never
# fail; every error listed is wrong, whether the cause is Astra or the processor.
#
#   powershell -ExecutionPolicy Bypass -File tools\cpucheck\cpucheck.ps1 [-Seconds 30] [-Cores fast|slow|all] [-Priority Normal|BelowNormal] [-Script file] [-Astra path]
#
# -Cores fast (the default) keeps it on the performance cores, where the errors happen, leaving
# the efficient cores to everything else; slow on the efficient cores, all on every core, to
# compare. -Priority BelowNormal keeps the machine responsive but makes the errors very rare.
# -Script runs another program in place of cpucheck.astra.
param([int]$Seconds = 30, [ValidateSet('fast', 'slow', 'all')][string]$Cores = 'fast', [string]$Astra = '', [string]$Script = '', [ValidateSet('Normal', 'BelowNormal')][string]$Priority = 'Normal')

$repo = Split-Path (Split-Path $PSScriptRoot)
. (Join-Path $repo 'bench\fastcores.ps1')
if (-not $Astra) {
    foreach ($candidate in @('build\clang\astra.exe', 'build\astra.exe')) {
        if (Test-Path (Join-Path $repo $candidate)) { $Astra = Join-Path $repo $candidate; break }
    }
}
$script = if ($Script) { $Script } else { Join-Path $PSScriptRoot 'cpucheck.astra' }
$fast = Get-FastCoreMask
$all = [long]0
for ($i = 0; $i -lt [Environment]::ProcessorCount; $i++) { $all = $all -bor ([long]1 -shl $i) }
$mask = switch ($Cores) { 'fast' { $fast } 'slow' { $all -band -bnot $fast } 'all' { $all } }
if ($mask -eq 0) { $mask = $all }

$cpu = (Get-ItemProperty 'HKLM:\HARDWARE\DESCRIPTION\System\CentralProcessor\0').ProcessorNameString.Trim()
"cpucheck: $cpu"
"cores: $Cores (mask {0:X}), {1} seconds, with {2}" -f $mask, $Seconds, $Astra

$deadline = (Get-Date).AddSeconds($Seconds)
$runs = 0; $failures = 0
while ((Get-Date) -lt $deadline) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Astra
    $psi.Arguments = '"' + $script + '"'
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $process = [System.Diagnostics.Process]::Start($psi)
    $process.ProcessorAffinity = [IntPtr]$mask
    $process.PriorityClass = $Priority
    $out = $process.StandardOutput.ReadToEndAsync()
    $err = $process.StandardError.ReadToEndAsync()
    $left = [int]($deadline - (Get-Date)).TotalMilliseconds
    $ended = $left -gt 0 -and $process.WaitForExit($left)
    if (-not $ended) { $process.Kill(); $process.WaitForExit() }
    $runs++
    if ($ended) {
        $failures++
        $at = [int]($Seconds - ($deadline - (Get-Date)).TotalSeconds)
        $message = (($err.Result -split "`n") | Where-Object { $_.Trim() } | Select-Object -First 1)
        if (-not $message) { $message = 'the program crashed (exit code {0:X})' -f $process.ExitCode }
        "  {0,3} s  ERROR: {1}" -f $at, $message.Trim()
    }
}
""
"{0} errors in {1} seconds ({2} runs of the program)." -f $failures, $Seconds, $runs
if ($failures -eq 0) { "No errors: every result was the same, as it should be." }
else { "The program computes the same thing every time, so none of these errors should happen." }
