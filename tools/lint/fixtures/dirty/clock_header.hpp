// DIRTY header fixture: proves .hpp headers are scanned too. Category: std::chrono.
#pragma once
#include <chrono>

namespace lockstep::bad {
inline auto deadline() { return std::chrono::steady_clock::now(); }
}  // namespace lockstep::bad
