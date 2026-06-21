// tests/smoke_test.cpp — the "trivial CI green" gate.
//
// Phase 0 has zero real logic; this test exists only to prove the build tree
// configures, compiles, links (against lockstep_core when A2's header-only
// INTERFACE lib is present), and runs under CTest + every sanitizer. Keep it
// trivial. Real behavior is tested from Phase 1 onward.
//
// No <chrono>/<thread>/<random> here, and none ever in non-provider code
// (cardinal rule 1 / forbidden-call lint).

#include <cassert>
#include <cstdio>

#if __has_include(<lockstep/core/Error.hpp>)
// A2's header-only core is on the include path. Including a core header here
// exercises the lockstep_core integration at compile time without depending on
// any specific symbol (the boundary is still being frozen by A2).
#  include <lockstep/core/Error.hpp>
#  define LOCKSTEP_CORE_PRESENT 1
#else
#  define LOCKSTEP_CORE_PRESENT 0
#endif

int main() {
  // The canonical trivial assertion.
  assert(1 + 1 == 2);

  std::puts(LOCKSTEP_CORE_PRESENT ? "smoke: ok (lockstep_core present)"
                                  : "smoke: ok (lockstep_core absent; A2 pending)");
  return 0;
}
