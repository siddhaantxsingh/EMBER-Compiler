#include "bc/opcodes.h"

namespace ember::bc {

namespace {

constexpr uint8_t format_size(const char* f) {
    uint8_t n = 1;
    for (; *f; ++f) {
        switch (*f) {
            case 'b': n += 1; break;
            case 'h': n += 2; break;
            case 'w': n += 4; break;
            case 'q':
            case 'd': n += 8; break;
            default: break;
        }
    }
    return n;
}

constexpr OpcodeInfo kInfo[] = {
#define X(n, f) {#n, f, format_size(f)},
    EMBER_OPCODES(X)
#undef X
};

static_assert(sizeof(kInfo) / sizeof(kInfo[0]) == kOpcodeCount);

}  // namespace

const OpcodeInfo& opcode_info(Opcode op) { return kInfo[static_cast<size_t>(op)]; }

}  // namespace ember::bc
