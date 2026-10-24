# Runs one program on the fastest kind of core (bench\fastcores.ps1), for a timing taken by hand:
# what it prints is shown as it is. Any timing of Astra on a processor with cores of two kinds is
# taken this way, or by bench\run.ps1; one left to the system may land on a slow core.
#
#   powershell -ExecutionPolicy Bypass -File bench\pinned.ps1 [-Astra build\clang\astra.exe] x.astra [args...]

param([string]$Astra = (Join-Path $PSScriptRoot '..\build\astra.exe'),
      [Parameter(Mandatory = $true, Position = 0)][string]$Script,
      [Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest)

. (Join-Path $PSScriptRoot 'fastcores.ps1')
$mask = Get-FastCoreMask
Write-Host ("on logical processors {0:X}" -f $mask)
Invoke-Pinned $Astra (@($Script) + @($Rest | Where-Object { $_ })) $mask
