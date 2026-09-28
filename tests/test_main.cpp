#include "tiny_test.h"

namespace muisc::test {

int run_all() {
    int failed = 0;
    for (const auto& c : registry()) {
        try {
            c.fn();
            std::printf("ok    %s\n", c.name);
        } catch (const Failure& f) {
            std::printf("FAIL  %s\n      %s\n", c.name, f.what.c_str());
            ++failed;
        } catch (const std::exception& e) {
            std::printf("FAIL  %s\n      threw std::exception: %s\n", c.name, e.what());
            ++failed;
        } catch (...) {
            std::printf("FAIL  %s\n      threw a non-exception\n", c.name);
            ++failed;
        }
    }
    const int total = static_cast<int>(registry().size());
    std::printf("\n%d/%d passed", total - failed, total);
    if (failed) std::printf(", %d FAILED", failed);
    std::printf("\n");
    return failed;  // non-zero exit is what ctest reads
}

} // namespace muisc::test

int main() { return muisc::test::run_all(); }
