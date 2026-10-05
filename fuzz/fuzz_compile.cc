// libFuzzer target: the whole compiler (parse, check, lower, optimize,
// generate, verify) must never crash, and must never report an internal
// error, for any input text.
#include <cstddef>
#include <cstdint>
#include <string>

#include "driver/driver.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 20000) return 0;
    std::string text(reinterpret_cast<const char*>(data), size);
    for (int opt = 0; opt <= 1; ++opt) {
        ember::CompileOptions o;
        o.opt_level = opt;
        auto c = ember::compile("fuzz.em", text, o);
        if (c->internal_error) __builtin_trap();
    }
    return 0;
}
