# Runs every tests\*.astra file three ways: straight from source, compiled to a bytecode
# file, and built into a program of its own. Expected output comes from comments in the test:
#
#   // expect: <line>                  the next line of standard output
#   // expect runtime error: <text>    the program fails (exit code 70) and reports <text>
#   // expect compile error: <text>    compilation fails (exit code 65) and reports <text>
#   // expect error output: <text>     the program finishes normally, having reported <text>
#
# One more check at the end covers the --icon option of `astra build`.

param([string]$Astra = (Join-Path $PSScriptRoot '..\build\astra.exe'))

if (-not (Test-Path $Astra)) {
    Write-Host "astra.exe not found at $Astra - run build.bat first"
    exit 2
}
$Astra = (Resolve-Path $Astra).Path

$tmp = Join-Path ([IO.Path]::GetTempPath()) "astra-tests-$PID"
New-Item -ItemType Directory -Force $tmp | Out-Null
# The compiled programs astra keeps for a script's next run (D145) go in a cache of the tests'
# own, which goes with the rest at the end.
$env:ASTRA_CACHE = Join-Path $tmp 'cache'

# Runs astra, or with -Program a program built by `astra build --exe`.
function Invoke-Astra([string[]]$AstraArgs, [string]$Program = $Astra) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Program
    $psi.Arguments = ($AstraArgs | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    # A test that writes files writes them here, not into the source tree.
    $psi.WorkingDirectory = $tmp
    # Programs write UTF-8, and the expectations in the test files are UTF-8 too.
    $psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
    $psi.StandardErrorEncoding = [System.Text.Encoding]::UTF8

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
    $match = Select-String -Path $path -Pattern $pattern -Encoding UTF8 | Select-Object -First 1
    if ($match) { return $match.Matches[0].Groups[1].Value.TrimEnd() }
    return ''
}

$passed = 0
$failures = @()

foreach ($test in Get-ChildItem (Join-Path $PSScriptRoot '*.astra') | Sort-Object Name) {
    $expected = @(Select-String -Path $test.FullName -Pattern '// expect: ?(.*)$' -Encoding UTF8 |
        ForEach-Object { $_.Matches[0].Groups[1].Value })
    $runtimeError = Get-Marker $test.FullName '// expect runtime error: (.*)$'
    $compileError = Get-Marker $test.FullName '// expect compile error: (.*)$'

    $bytecode = Join-Path $tmp ($test.BaseName + '.astc')
    $results = [ordered]@{}

    if ($compileError) {
        $results['script'] = Compare-Run (Invoke-Astra @($test.FullName)) @() $compileError 65
        $results['compiled'] = Compare-Run (Invoke-Astra @('build', $test.FullName, '-o', $bytecode)) @() $compileError 65
    } else {
        # `// expect error output: <text>`: the program goes on to its end, having reported <text>
        # (an error that ended one of its threads, say).
        $errorText = $runtimeError
        if (-not $errorText) { $errorText = Get-Marker $test.FullName '// expect error output: (.*)$' }
        $exitCode = if ($runtimeError) { 70 } else { 0 }
        $results['script'] = Compare-Run (Invoke-Astra @($test.FullName)) $expected $errorText $exitCode
        # Run again, it starts from the program the first run kept.
        $results['cached'] = Compare-Run (Invoke-Astra @($test.FullName)) $expected $errorText $exitCode

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

# The type checker (M6.5): `astra check` on each tests\check\*.astra. A line ending in
# `// check: <message>` must be reported with that message, on that line; nothing else may
# be reported.
foreach ($test in Get-ChildItem (Join-Path $PSScriptRoot 'check\*.astra') | Sort-Object Name) {
    $wanted = @{}
    $lineNumber = 0
    foreach ($line in [IO.File]::ReadAllLines($test.FullName, [Text.Encoding]::UTF8)) {
        $lineNumber++
        if ($line -match '// check: (.*)$') { $wanted["$($lineNumber): $($Matches[1].TrimEnd())"] = $true }
    }
    $run = Invoke-Astra @('check', $test.FullName)
    $problems = @()
    $found = @{}
    foreach ($line in $run.Lines) {
        if ($line -match '^[^:]+:(\d+):\d+: error: (.*)$') {
            $key = "$($Matches[1]): $($Matches[2].TrimEnd())"
            $found[$key] = $true
            if (-not $wanted.ContainsKey($key)) { $problems += "unexpected: $key" }
        } elseif ($line.Trim()) {
            $problems += "unexpected output: $line"
        }
    }
    foreach ($key in $wanted.Keys) {
        if (-not $found.ContainsKey($key)) { $problems += "not reported: $key" }
    }
    $exitCode = if ($wanted.Count -gt 0) { 65 } else { 0 }
    if ($run.Code -ne $exitCode) { $problems += "exit code $($run.Code), expected $exitCode; stderr: $($run.Error.Trim())" }
    if ($problems.Count -eq 0) {
        $passed++
    } else {
        $failures += "$($test.Name) [check]`n    " + ($problems -join "`n    ")
    }
}

# The shell: each tests\shell\*.shell file is typed into it, line by line. Its `// expect:`
# lines are what the shell prints, and its `// expect error:` lines the errors, each in order.
foreach ($test in Get-ChildItem (Join-Path $PSScriptRoot 'shell\*.shell') | Sort-Object Name) {
    $text = [IO.File]::ReadAllText($test.FullName)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Astra
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $psi.WorkingDirectory = $tmp
    $psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
    $psi.StandardErrorEncoding = [System.Text.Encoding]::UTF8
    $process = [System.Diagnostics.Process]::Start($psi)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $process.StandardInput.Write($text)
    $process.StandardInput.Close()
    $process.WaitForExit()
    $outputs = @($stdout.Result -split "\r?\n" | Where-Object { $_ -ne '' })
    $errors = @($stderr.Result -split "\r?\n" | Where-Object { $_ -ne '' })
    $wantOut = @([regex]::Matches($text, '(?m)^// expect: (.*?)\r?$') | ForEach-Object { $_.Groups[1].Value })
    $wantErr = @([regex]::Matches($text, '(?m)^// expect error: (.*?)\r?$') | ForEach-Object { $_.Groups[1].Value })
    $result = $null
    for ($i = 0; $i -lt [Math]::Max($wantOut.Count, $outputs.Count) -and $null -eq $result; $i++) {
        $want = if ($i -lt $wantOut.Count) { $wantOut[$i] } else { '<no more output>' }
        $got = if ($i -lt $outputs.Count) { $outputs[$i].TrimEnd() } else { '<no more output>' }
        if ($want -cne $got) { $result = "output $($i + 1):`n      expected: $want`n      actual:   $got" }
    }
    for ($i = 0; $i -lt [Math]::Max($wantErr.Count, $errors.Count) -and $null -eq $result; $i++) {
        $want = if ($i -lt $wantErr.Count) { $wantErr[$i] } else { '<no more errors>' }
        $got = if ($i -lt $errors.Count) { $errors[$i].TrimEnd() } else { '<no more errors>' }
        if ($want -cne $got) { $result = "error $($i + 1):`n      expected: $want`n      actual:   $got" }
    }
    if ($null -eq $result) { $passed++ } else { $failures += "$($test.Name) [shell]`n    $result" }
}

# The --icon option of `astra build`: a program built with an icon runs as before, and
# Windows finds the icon in the file. The icon is one 32 pixel picture in a single colour.
$iconColor = 220, 30, 40
$bytes = New-Object IO.MemoryStream
$writer = New-Object IO.BinaryWriter $bytes
$picture = 40 + 32 * 32 * 4 + 32 * 4
foreach ($field in 0, 1, 1) { $writer.Write([int16]$field) }                       # an icon file with one picture
$writer.Write([byte[]](32, 32, 0, 0)); $writer.Write([int16]1); $writer.Write([int16]32)
$writer.Write([int]$picture); $writer.Write([int]22)                               # its size and where it starts
$writer.Write([int]40); $writer.Write([int]32); $writer.Write([int]64); $writer.Write([int16]1); $writer.Write([int16]32)
$writer.Write((New-Object byte[] 24))
for ($i = 0; $i -lt 32 * 32; $i++) { $writer.Write([byte[]]($iconColor[2], $iconColor[1], $iconColor[0], 255)) }
$writer.Write((New-Object byte[] (32 * 4)))                                        # nothing is transparent
$iconFile = Join-Path $tmp 'test.ico'
[IO.File]::WriteAllBytes($iconFile, $bytes.ToArray())

$source = Join-Path $tmp 'icon.astra'
[IO.File]::WriteAllText($source, 'print("has an icon")')
$program = Join-Path $tmp 'icon.exe'
$build = Invoke-Astra @('build', $source, '--icon', $iconFile, '-o', $program)
$result = $null
if ($build.Code -ne 0) {
    $result = "build failed: $($build.Error.Trim())"
} else {
    $result = Compare-Run (Invoke-Astra @() -Program $program) @('has an icon') '' 0
    if ($null -eq $result) {
        Add-Type -AssemblyName System.Drawing
        $pixel = [Drawing.Icon]::ExtractAssociatedIcon($program).ToBitmap().GetPixel(16, 16)
        $found = $pixel.R, $pixel.G, $pixel.B
        if ("$found" -ne "$iconColor") { $result = "the program's icon is coloured ($found), expected ($iconColor)" }
    }
}
if ($null -eq $result) { $passed++ } else { $failures += "--icon option [program]`n    $result" }

Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue

foreach ($failure in $failures) { Write-Host "FAIL $failure" }
Write-Host "$passed passed, $($failures.Count) failed"
if ($failures.Count -gt 0) { exit 1 }
