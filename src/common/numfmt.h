// Number formatting shared by the VM and the reference interpreter so that
// both print exactly the same text.
#pragma once

#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ember {

// Shortest round-trip decimal with at least one digit after the point
// ("2.0", "0.1", "1e+300"); "nan", "inf", "-inf".
inline std::string format_float(double d) {
    if (std::isnan(d)) return "nan";
    if (std::isinf(d)) return d < 0 ? "-inf" : "inf";
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof buf, d);
    std::string s(buf, res.ptr);
    if (s.find_first_of(".en") == std::string::npos) s += ".0";
    return s;
}

// Decimal integer: optional '-', then one or more digits; no spaces, no '+'.
inline std::optional<int64_t> parse_int(std::string_view s) {
    if (s.empty()) return std::nullopt;
    size_t i = s[0] == '-' ? 1 : 0;
    if (i == s.size()) return std::nullopt;
    for (size_t k = i; k < s.size(); ++k)
        if (s[k] < '0' || s[k] > '9') return std::nullopt;
    int64_t v = 0;
    auto res = std::from_chars(s.data(), s.data() + s.size(), v);
    if (res.ec != std::errc() || res.ptr != s.data() + s.size()) return std::nullopt;
    return v;
}

}  // namespace ember
