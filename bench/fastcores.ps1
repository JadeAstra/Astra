# The logical processors of the fastest kind, as an affinity mask, and a way to run a program on
# them. A processor with cores of two kinds (Intel's performance and efficient cores) runs the
# same code at very different speeds on each, and a benchmark the system puts on a slow core
# measures the core, not the code: on an i9-14900KF the interpreter takes 1.85 times as long on
# an efficient core. Windows says which kind each core is (its efficiency class, higher for the
# faster); on a processor with one kind, every logical processor is in the mask.
#
#   . bench\fastcores.ps1; Invoke-Pinned 'astra.exe' @('x.astra')     (on the fastest cores)
#   powershell -ExecutionPolicy Bypass -File bench\pinned.ps1 x.astra  (one program, from a shell)

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

function Get-FastCoreMask {
    $mask = [CoreKinds]::FastMask()
    if ($mask -le 0) { throw "the fastest cores could not be found (GetLogicalProcessorInformationEx failed): benchmarks are not run on cores of unknown kind" }
    return $mask
}

# Runs a program on the logical processors of `$Mask` and gives its standard output's lines. The
# program starts on them: a process takes its affinity from the one that starts it, so this one
# takes the mask for the moment of the start (and the program is given it again after, in case).
# There is no way to run unpinned here: a timing taken on whatever core the system picked
# measures the core.
function Invoke-Pinned([string]$Program, [string[]]$Arguments, [long]$Mask = (Get-FastCoreMask)) {
    if ($Mask -le 0) { throw "Invoke-Pinned needs a mask of logical processors" }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Program
    $psi.Arguments = ($Arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $self = [System.Diagnostics.Process]::GetCurrentProcess()
    $own = $self.ProcessorAffinity
    $self.ProcessorAffinity = [IntPtr]$Mask
    try {
        $process = [System.Diagnostics.Process]::Start($psi)
    } finally {
        $self.ProcessorAffinity = $own
    }
    try { $process.ProcessorAffinity = [IntPtr]$Mask } catch { }  # it may have ended already
    $out = $process.StandardOutput.ReadToEndAsync()
    $err = $process.StandardError.ReadToEnd()
    $process.WaitForExit()
    # A run that fails is said, not taken for a time.
    if ($process.ExitCode -ne 0) { Write-Host "  ($Program $Arguments ended with $($process.ExitCode): $($err.Trim()))" }
    return @($out.Result -split "\r?\n" | Where-Object { $_ -ne '' })
}
