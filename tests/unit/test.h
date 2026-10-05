// Minimal unit-test framework (no external dependencies).
#pragma once

#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace t {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

struct Register {
    Register(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};

struct Failure {
    std::string msg;
};

inline int& check_failures() {
    static int n = 0;
    return n;
}

}  // namespace t

#define TEST_CAT2(a, b) a##b
#define TEST_CAT(a, b) TEST_CAT2(a, b)
#define TEST(name)                                                         \
    static void TEST_CAT(test_fn_, name)();                                \
    static t::Register TEST_CAT(test_reg_, name)(#name, TEST_CAT(test_fn_, name)); \
    static void TEST_CAT(test_fn_, name)()

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::ostringstream os_;                                                       \
            os_ << __FILE__ << ":" << __LINE__ << ": CHECK(" #cond ") failed";            \
            throw t::Failure{os_.str()};                                                  \
        }                                                                                 \
    } while (0)

#define CHECK_EQ(a, b)                                                                    \
    do {                                                                                  \
        auto va_ = (a);                                                                   \
        auto vb_ = (b);                                                                   \
        if (!(va_ == vb_)) {                                                              \
            std::ostringstream os_;                                                       \
            os_ << __FILE__ << ":" << __LINE__ << ": CHECK_EQ(" #a ", " #b ") failed\n    left:  " << va_ \
                << "\n    right: " << vb_;                                                \
            throw t::Failure{os_.str()};                                                  \
        }                                                                                 \
    } while (0)

#define CHECK_CONTAINS(hay, needle)                                                       \
    do {                                                                                  \
        std::string h_ = (hay);                                                           \
        std::string n_ = (needle);                                                        \
        if (h_.find(n_) == std::string::npos) {                                           \
            std::ostringstream os_;                                                       \
            os_ << __FILE__ << ":" << __LINE__ << ": expected to find \"" << n_ << "\" in:\n" << h_; \
            throw t::Failure{os_.str()};                                                  \
        }                                                                                 \
    } while (0)
