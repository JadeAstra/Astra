# The logical processors of the fastest kind, as an affinity mask, and a way to run a program on
# them. A processor with cores of two kinds (Intel's performance and efficient cores) runs the
# same code at very different speeds on each, and a benchmark the system puts on a slow core
# measures the core, not the code: on an i9-14900KF the interpreter takes 1.85 times as long on
# an efficient core. Windows says which kind each core is (its efficiency class, higher for the
# faster); on a processor with one kind, every logical processor is in the mask.
#
#   . bench\fastcores.ps1; $mask = Get-FastCoreMask; Invoke-Pinned 'astra.exe' @('x.astra') $mask

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class CoreKinds {
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetLogicalProcessorInformationEx(int relation, IntPtr buffer, ref int length);

    // Each processor core (RelationProcessorCore = 0): its efficiency class and the logical
    // processors it holds (group 0 only, which is all of them up to 64).
    public static long FastMask() {
        int length = 0;
        GetLogicalProcessorInformationEx(0, IntPtr.Zero, ref length);
        IntPtr buffer = Marshal.AllocHGlobal(length);
        try {
            if (!GetLogicalProcessorInformationEx(0, buffer, ref length)) return -1;
            int best = -1;
            long mask = 0;
            for (int at = 0; at < length; ) {
                IntPtr entry = IntPtr.Add(buffer, at);
                int size = Marshal.ReadInt32(entry, 4);
                // PROCESSOR_RELATIONSHIP: Flags (byte at 8), EfficiencyClass (byte at 9),
                // then GroupCount (ushort at 30) and the GROUP_AFFINITY array at 32 (Mask first).
                int efficiency = Marshal.ReadByte(entry, 9);
                long cores = Marshal.ReadInt64(entry, 32);
                if (efficiency > best) { best = efficiency; mask = cores; }
                else if (efficiency == best) { mask |= cores; }
                at += size;
            }
            return mask;
        } finally {
            Marshal.FreeHGlobal(buffer);
        }
    }
}
'@

function Get-FastCoreMask { return [CoreKinds]::FastMask() }

# Runs a program on the logical processors of `$mask` and gives its standard output's lines.
function Invoke-Pinned([string]$Program, [string[]]$Arguments, [long]$Mask) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Program
    $psi.Arguments = ($Arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $process = [System.Diagnostics.Process]::Start($psi)
    if ($Mask -gt 0) { $process.ProcessorAffinity = [IntPtr]$Mask }
    $out = $process.StandardOutput.ReadToEndAsync()
    $err = $process.StandardError.ReadToEnd()
    $process.WaitForExit()
    # A run that fails is said, not taken for a time.
    if ($process.ExitCode -ne 0) { Write-Host "  ($Program $Arguments ended with $($process.ExitCode): $($err.Trim()))" }
    return @($out.Result -split "\r?\n" | Where-Object { $_ -ne '' })
}
