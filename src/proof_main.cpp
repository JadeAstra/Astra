// The start of the build's proofs (D161): a built program's start, which has no command.

#include "run.h"

int main(int argc, char** argv) { return astra::startExecutable(argc, argv, nullptr); }
