# Runs every tests\*.astra file three ways: straight from source, compiled to a bytecode
# file, and built into a program of its own. Expected output comes from comments in the test:
#
#   // expect: <line>                  the next line of standard output
#   // expect runtime error: <text>    the program fails (exit code 70) and reports <text>
#   // expect compile error: <text>    compilation fails (exit code 65) and reports <text>

param([string]$Astra = (Join-Path $PSScriptRoot '..\build\astra.exe'))

if (-not (Test-Path $Astra)) {
    Write-Host "astra.exe not found at $Astra - run build.bat first"
    exit 2
}
$Astra = (Resolve-Path $Astra).Path

$tmp = Join-Path ([IO.Path]::GetTempPath()) "astra-tests-$PID"
New-Item -ItemType Directory -Force $tmp | Out-Null

# Runs astra, or with -Program a program built by `astra build --exe`.
function Invoke-Astra([string[]]$AstraArgs, [string]$Program = $Astra) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Program
    $psi.Arguments = ($AstraArgs | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true

    $process = [System.Diagnostics.Process]::Start($psi)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEnd()
    $process.WaitForExit()

    $lines = @($stdout.Result -split "\r?\n")
    if ($lines.Count -gt 0 -and $lines[-1] -eq '') {
        $lines = @($lines | Select-Object -SkipLast 1)
    }
    return @{ Lines = $lines; Error = $stderr; Code = $process.ExitCode }
}

# Returns $null if the run matches, otherwise a description of the first difference.
function Compare-Run($run, [string[]]$expected, [string]$errorText, [int]$exitCode) {
    $actual = $run.Lines
    for ($i = 0; $i -lt [Math]::Max($expected.Count, $actual.Count); $i++) {
        $want = if ($i -lt $expected.Count) { $expected[$i].TrimEnd() } else { '<no more output>' }
        $got = if ($i -lt $actual.Count) { $actual[$i].TrimEnd() } else { '<no more output>' }
        if ($want -cne $got) {
            return "output line $($i + 1):`n      expected: $want`n      actual:   $got"
        }
    }
    if ($run.Code -ne $exitCode) {
        return "exit code $($run.Code), expected $exitCode`n      stderr: $($run.Error.Trim())"
    }
    if ($errorText -and -not $run.Error.Contains($errorText)) {
        return "stderr does not contain: $errorText`n      stderr: $($run.Error.Trim())"
    }
    if (-not $errorText -and $run.Error.Trim()) {
        return "unexpected stderr: $($run.Error.Trim())"
    }
    return $null
}

function Get-Marker([string]$path, [string]$pattern) {
    $match = Select-String -Path $path -Pattern $pattern | Select-Object -First 1
    if ($match) { return $match.Matches[0].Groups[1].Value.TrimEnd() }
    return ''
}

$passed = 0
$failures = @()

foreach ($test in Get-ChildItem (Join-Path $PSScriptRoot '*.astra') | Sort-Object Name) {
    $expected = @(Select-String -Path $test.FullName -Pattern '// expect: ?(.*)$' |
        ForEach-Object { $_.Matches[0].Groups[1].Value })
    $runtimeError = Get-Marker $test.FullName '// expect runtime error: (.*)$'
    $compileError = Get-Marker $test.FullName '// expect compile error: (.*)$'

    $bytecode = Join-Path $tmp ($test.BaseName + '.astc')
    $results = [ordered]@{}

    if ($compileError) {
        $results['script'] = Compare-Run (Invoke-Astra @($test.FullName)) @() $compileError 65
        $results['compiled'] = Compare-Run (Invoke-Astra @('build', $test.FullName, '-o', $bytecode)) @() $compileError 65
    } else {
        $errorText = $runtimeError
        $exitCode = if ($runtimeError) { 70 } else { 0 }
        $results['script'] = Compare-Run (Invoke-Astra @($test.FullName)) $expected $errorText $exitCode

        $build = Invoke-Astra @('build', $test.FullName, '-o', $bytecode)
        if ($build.Code -ne 0) {
            $results['compiled'] = "build failed: $($build.Error.Trim())"
        } else {
            $results['compiled'] = Compare-Run (Invoke-Astra @($bytecode)) $expected $errorText $exitCode
        }

        $program = Join-Path $tmp ($test.BaseName + '.exe')
        $build = Invoke-Astra @('build', $test.FullName, '--exe', '-o', $program)
        if ($build.Code -ne 0) {
            $results['program'] = "build failed: $($build.Error.Trim())"
        } else {
            $results['program'] = Compare-Run (Invoke-Astra @() -Program $program) $expected $errorText $exitCode
        }
    }

    foreach ($mode in $results.Keys) {
        if ($null -eq $results[$mode]) {
            $passed++
        } else {
            $failures += "$($test.Name) [$mode]`n    $($results[$mode])"
        }
    }
}

Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue

foreach ($failure in $failures) { Write-Host "FAIL $failure" }
Write-Host "$passed passed, $($failures.Count) failed"
if ($failures.Count -gt 0) { exit 1 }
