#include "tools/version.h"

#include <string>

#include "bc/format.h"

namespace ember {

const char* version_string() {
    static const std::string v = "ember 0.1.0 (bytecode format " + std::to_string(bc::kVersionMajor) + "." +
                                 std::to_string(bc::kVersionMinor) + ")";
    return v.c_str();
}

}  // namespace ember
