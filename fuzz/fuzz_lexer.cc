// libFuzzer target: the lexer must never crash or hang on any bytes.
#include <cstddef>
#include <cstdint>
#include <string>

#include "common/diagnostics.h"
#include "front/lexer.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    ember::SourceFile f("fuzz.em", std::string(reinterpret_cast<const char*>(data), size));
    ember::Diagnostics d;
    ember::Lexer l(f, d);
    auto toks = l.tokenize();
    if (toks.empty()) __builtin_trap();
    d.render(f);
    return 0;
}
