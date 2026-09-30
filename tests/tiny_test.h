#pragma once
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

// A hand-rolled test harness, for the same reason src/tiny_json.h is hand-rolled
// (see its header comment): this tree vendors or writes its dependencies and
// hash-pins the two it fetches from the network. A test framework would be the
// first unpinned configure-time download in the build, in exchange for "call
// these functions and report which ones threw".
//
// Registration is a static initialiser per TEST(), so adding a test file to the
// mousiki_tests source list in CMakeLists.txt is all it takes to have its cases
// run -- there is no list of tests to keep in sync.
namespace muisc::test {

struct Case { const char* name; void (*fn)(); };
inline std::vector<Case>& registry() { static std::vector<Case> r; return r; }
struct Registrar { Registrar(const char* n, void (*f)()) { registry().push_back({n, f}); } };

// Thrown by a failed check and caught per-case, so one failure reports and the
// remaining cases still run.
struct Failure { std::string what; };

inline std::string show(const std::string& s) { return "\"" + s + "\""; }
inline std::string show(const char* s) { return show(std::string(s ? s : "(null)")); }
inline std::string show(bool b) { return b ? "true" : "false"; }
template <class T> std::string show(const T& v) { std::ostringstream o; o << v; return o.str(); }

[[noreturn]] inline void fail(const char* file, int line, const std::string& msg) {
    throw Failure{std::string(file) + ":" + std::to_string(line) + ": " + msg};
}

// Reads a recorded response from tests/fixtures. The directory is baked in at
// configure time so a test never depends on the working directory ctest happens
// to launch it from.
inline std::string fixture(const std::string& name) {
    const std::string path = std::string(MOUSIKI_FIXTURE_DIR) + "/" + name;
    std::ifstream in(path, std::ios::binary);
    if (!in) fail(__FILE__, __LINE__, "missing fixture " + path);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

int run_all();

} // namespace muisc::test

#define TEST(name)                                           \
    static void name();                                      \
    static ::muisc::test::Registrar reg_##name(#name, name); \
    static void name()

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) ::muisc::test::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)

#define CHECK_EQ(a, b)                                                        \
    do {                                                                      \
        auto&& a_ = (a);                                                      \
        auto&& b_ = (b);                                                      \
        if (!(a_ == b_))                                                      \
            ::muisc::test::fail(__FILE__, __LINE__,                           \
                                "CHECK_EQ(" #a ", " #b ") -- " +              \
                                    ::muisc::test::show(a_) + " vs " +        \
                                    ::muisc::test::show(b_));                 \
    } while (0)
