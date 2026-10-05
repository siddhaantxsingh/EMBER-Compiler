// Random generator of well-typed Ember programs, for differential testing
// (compiler+VM vs reference interpreter, -O0 vs -O1, GC stress vs normal).
// Programs always terminate: loops have constant bounds or counters,
// functions only call functions defined before them (plus one bounded
// recursive function). Some programs end with a runtime error (division by
// zero, overflow, index out of bounds ...) on purpose: both
// implementations must report the same error.
#pragma once

#include <cstdint>
#include <string>

namespace ember {

std::string generate_program(uint64_t seed);

}  // namespace ember
