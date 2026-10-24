What this is
------------
A check of whether a computer runs a small program reliably.

test.astra is a small program, run by astra.exe on one thread: it builds the string "string"
from four pieces 60 million times and compares it with "string". The two are always equal, so a
working computer never prints a WRONG line, and the program always ends with "done".

burst.exe is a separate program that loads the processor in bursts: 32 threads wake every 50 ms,
compute for 20 ms, and go back to sleep. The two programs share nothing.

run.bat runs run.astra, which runs test.astra over and over for 3 minutes on its own, then 3
minutes beside burst.exe, and says how many runs failed in each phase: a run fails when it
prints a WRONG line, stops with an error, or ends without "done". It uses all logical
processors; on a processor with two kinds of core (Intel 12th gen and later), give the fast ones
as a hexadecimal mask: run.bat --mask FFFF (the 16 fast ones of an i9-14900KF). It takes
about 6 minutes; the machine is busy meanwhile. The results are written to results.txt.

On the computer where this was found (an Intel Core i9-14900KF), the runs beside burst.exe fail
often (about half of them) and the runs alone never do.

How to run
----------
Copy the whole folder anywhere and double-click run.bat (or run: astra.exe run.astra
[--seconds 180] [--mask HEX]). Nothing is installed; astra.exe and burst.exe need only Windows
itself.

Files
-----
astra.exe        the Astra interpreter (Windows, 64-bit)
test.astra       the program that is checked
burst.exe        the load; burst.cpp is its source, build-burst.bat builds it again
run.astra        the check, written in Astra
run.bat          runs it

The C++ check (no Astra)
------------------------
cpudetect.exe does the same kind of test with no Astra in it: a small interpreter and a
garbage-collected heap written in C++ (cpudetect.cpp), building the string "string" from four
pieces and comparing it, 60 million times per run. It starts its own burst load as a second
process. Double-click run-cpudetect.bat: 3 minutes without load, then 3 minutes with it (or run
cpudetect.exe [--seconds N] [--cores all|p|e] [--no-bursts]). A run fails if a comparison comes out
wrong, the interpreter's stack ends at the wrong depth, an operation sees a value of the wrong
type, or the program crashes (the crash is reported with its exception code). On the
i9-14900KF: no failures in 130 runs without load; beside the load, 5 of 6 three-minute sessions
failed (runs ending with the stack one value short, crashes, Windows stopping it for heap
corruption).

In the repository
-----------------
The .exe files are not kept in git. Build burst.exe and cpudetect.exe with build-burst.bat and
build-cpudetect.bat, and copy astra.exe from build\clang (or build) to make the folder
complete; zipping the folder gives cpucheck-other-pc.zip.
