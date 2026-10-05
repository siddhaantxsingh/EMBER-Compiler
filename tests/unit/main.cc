#include <cstring>

#include "test.h"

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int passed = 0, failed = 0;
    for (auto& c : t::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        try {
            c.fn();
            ++passed;
        } catch (const t::Failure& f) {
            ++failed;
            std::cout << "FAIL " << c.name << "\n  " << f.msg << "\n";
        } catch (const std::exception& e) {
            ++failed;
            std::cout << "FAIL " << c.name << "\n  unexpected exception: " << e.what() << "\n";
        }
    }
    std::cout << "unit tests: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
