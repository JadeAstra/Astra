// For the build's proof that a built program without the window's section needs none of it
// (D161): the two entry points of the window that the core names, which it calls only when
// the section is there (run.h, windowCodePresent). Linked into no executable that is shipped.

#include <cstdlib>

#include "vm.h"

namespace astra {

void registerText(VM&) { std::abort(); }
void registerJade(VM&) { std::abort(); }

}  // namespace astra
