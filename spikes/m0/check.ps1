# Runs the spike's scripts the way the test suite runs a test: from source, from a compiled
# .astc and as a built program, comparing against their `// expect:` comments.
param([string]$Astra = "$PSScriptRoot\..\..\build\astra.exe")
$Astra = (Resolve-Path $Astra).Path
$work = Join-Path ([IO.Path]::GetTempPath()) "astra-m0-check-$PID"
New-Item -ItemType Directory -Force $work | Out-Null
Copy-Item "$PSScriptRoot\..\..\tests\run.ps1" $work
Copy-Item "$PSScriptRoot\lambdas.astra", "$PSScriptRoot\receivers.astra" $work   # the two with expectations
powershell -NoProfile -ExecutionPolicy Bypass -File "$work\run.ps1" -Astra $Astra
$code = $LASTEXITCODE
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
exit $code
