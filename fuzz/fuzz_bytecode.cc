// libFuzzer target: arbitrary bytes as a bytecode file. The decoder must
// reject or accept without crashing; accepted modules go through the
// verifier, and verified modules are executed with tight limits. A verified
// module may fail at run time (type mismatch etc.) but must never crash.
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <vector>

#include "bc/disasm.h"
#include "bc/format.h"
#include "bc/verify.h"
#include "vm/vm.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Fix up the header so that most inputs get past the checksum and
    // exercise the structural decoder: bytes are used as the payload.
    std::vector<uint8_t> file = {'E', 'M', 'B', 'C', 1, 0, 0, 0, 0, 0, 0, 0};
    uint32_t n = static_cast<uint32_t>(size);
    uint32_t crc = ember::bc::crc32(data, size);
    file.insert(file.end(), reinterpret_cast<uint8_t*>(&n), reinterpret_cast<uint8_t*>(&n) + 4);
    file.insert(file.end(), reinterpret_cast<uint8_t*>(&crc), reinterpret_cast<uint8_t*>(&crc) + 4);
    file.insert(file.end(), data, data + size);
    if (size > 0 && data[0] & 1) file[8] = 1;  // debug-info flag
    ember::bc::Module m;
    try {
        m = ember::bc::decode(file);
    } catch (const ember::bc::DecodeError&) {
        return 0;
    }
    ember::bc::disassemble(m);
    if (!ember::bc::verify(m).ok()) return 0;
    ember::vm::VMOptions o;
    o.instruction_budget = 100000;
    o.max_heap_bytes = 8u << 20;
    o.max_call_depth = 200;
    o.max_stack_values = 1u << 14;
    o.max_string_bytes = 1u << 20;
    o.max_array_length = 1u << 14;
    std::ostringstream out;
    std::istringstream in;
    ember::vm::VM vm(m, o);
    vm.set_output(&out);
    vm.set_input(&in);
    vm.run();
    return 0;
}
