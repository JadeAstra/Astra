// The astra executable's start (M6.5 part 5): a program built into the executable runs, and
// otherwise this is the astra command. In no section a built program leaves out, and not in
// the build's proofs, which have a start of their own (proof_main.cpp): it names the command.

#include "run.h"

int main(int argc, char** argv) { return astra::startExecutable(argc, argv, astra::astraCommandMain); }
