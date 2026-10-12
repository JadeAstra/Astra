# Shows the spike window for a moment and posts input messages to it, to check that raw events
# reach the script. The window closes by itself.
param([string]$Astra = "$PSScriptRoot\..\..\build\astra.exe", [string]$Out = "$PSScriptRoot\out")
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Win {
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam);
}
'@
New-Item -ItemType Directory -Force $Out | Out-Null
$log = Join-Path $Out 'window_events.txt'
$p = Start-Process $Astra -ArgumentList "`"$PSScriptRoot\window.astra`"", "`"$Out`"", 'show', '2500' -PassThru -NoNewWindow -RedirectStandardOutput $log
$deadline = (Get-Date).AddSeconds(5)
while ($p.MainWindowHandle -eq [IntPtr]::Zero -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 100; $p.Refresh() }
$h = $p.MainWindowHandle
"window found: $($h -ne [IntPtr]::Zero), title '$($p.MainWindowTitle)'"
function Point($x, $y) { [IntPtr](($y -shl 16) -bor $x) }
[Win]::PostMessage($h, 0x0200, [IntPtr]0, (Point 120 90)) | Out-Null     # mouse move
[Win]::PostMessage($h, 0x0201, [IntPtr]1, (Point 120 90)) | Out-Null     # left button down
[Win]::PostMessage($h, 0x0202, [IntPtr]0, (Point 120 90)) | Out-Null     # left button up
[Win]::PostMessage($h, 0x0100, [IntPtr]65, [IntPtr]0) | Out-Null         # key down: A
[Win]::PostMessage($h, 0x0102, [IntPtr]97, [IntPtr]0) | Out-Null         # char: a
[Win]::PostMessage($h, 0x0200, [IntPtr]0, (Point 300 215)) | Out-Null    # mouse move
Start-Sleep -Milliseconds 400
[Win]::PostMessage($h, 0x0100, [IntPtr]27, [IntPtr]0) | Out-Null         # Escape: the script quits
$p.WaitForExit(6000) | Out-Null
"exited: $($p.HasExited), code $($p.ExitCode)"
Get-Content $log
