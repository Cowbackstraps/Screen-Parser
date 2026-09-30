// Minimal gtest-compatible shim used ONLY for host-side offline verification
// when the real GoogleTest is unavailable. Inside the OpenHarmony tree the real
// <gtest/gtest.h> is used (see test/BUILD.gn); this header is placed on the
// include path ahead of it for the local CMake/g++ build.
//
// Implements the subset used by this project's tests: TEST, EXPECT_*/ASSERT_*,
// EXPECT_THROW, EXPECT_NEAR, ::testing::InitGoogleTest and RUN_ALL_TESTS.

#ifndef SCREENPARSER_TEST_GTEST_SHIM_H
#define SCREENPARSER_TEST_GTEST_SHIM_H

#include <cmath>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace testing {

struct ShimFailure : public std::exception {};

struct Registry {
    struct Case {
        std::string suite;
        std::string name;
        std::function<void()> fn;
    };
    static std::vector<Case> &Cases() {
        static std::vector<Case> cases;
        return cases;
    }
    static bool &CurrentFailed() {
        static bool failed = false;
        return failed;
    }
};

inline void InitGoogleTest(int *argc, char **argv) {
    (void)argc;
    (void)argv;
}

inline void ReportFailure(const char *file, int line, const std::string &msg) {
    Registry::CurrentFailed() = true;
    std::cout << file << ":" << line << ": Failure\n  " << msg << std::endl;
}

}  // namespace testing

struct ShimRegistrar {
    ShimRegistrar(const char *suite, const char *name, std::function<void()> fn) {
        testing::Registry::Cases().push_back({suite, name, std::move(fn)});
    }
};

#define TEST(suite, name)                                                        \
    static void suite##_##name##_body();                                         \
    static ShimRegistrar reg_##suite##_##name(#suite, #name,                     \
                                              suite##_##name##_body);            \
    static void suite##_##name##_body()

#define SHIM_EXPECT(cond, text)                                                  \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ::testing::ReportFailure(__FILE__, __LINE__, text);                  \
        }                                                                        \
    } while (0)

#define SHIM_ASSERT(cond, text)                                                  \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ::testing::ReportFailure(__FILE__, __LINE__, text);                  \
            throw ::testing::ShimFailure();                                      \
        }                                                                        \
    } while (0)

#define EXPECT_TRUE(c) SHIM_EXPECT((c), "expected true: " #c)
#define EXPECT_FALSE(c) SHIM_EXPECT(!(c), "expected false: " #c)
#define ASSERT_TRUE(c) SHIM_ASSERT((c), "assert true: " #c)
#define ASSERT_FALSE(c) SHIM_ASSERT(!(c), "assert false: " #c)

#define EXPECT_EQ(a, b) SHIM_EXPECT((a) == (b), "expected equal: " #a " == " #b)
#define EXPECT_NE(a, b) SHIM_EXPECT((a) != (b), "expected not equal: " #a " != " #b)
#define EXPECT_LT(a, b) SHIM_EXPECT((a) < (b), "expected: " #a " < " #b)
#define EXPECT_LE(a, b) SHIM_EXPECT((a) <= (b), "expected: " #a " <= " #b)
#define EXPECT_GT(a, b) SHIM_EXPECT((a) > (b), "expected: " #a " > " #b)
#define EXPECT_GE(a, b) SHIM_EXPECT((a) >= (b), "expected: " #a " >= " #b)
#define ASSERT_EQ(a, b) SHIM_ASSERT((a) == (b), "assert equal: " #a " == " #b)

#define EXPECT_NEAR(a, b, tol)                                                   \
    SHIM_EXPECT(std::fabs((a) - (b)) <= (tol), "expected near: " #a " ~= " #b)

#define EXPECT_THROW(stmt, exc)                                                  \
    do {                                                                         \
        bool thrown = false;                                                     \
        try {                                                                    \
            stmt;                                                                \
        } catch (const exc &) {                                                  \
            thrown = true;                                                       \
        } catch (...) {                                                          \
        }                                                                        \
        SHIM_EXPECT(thrown, "expected throw " #exc " from: " #stmt);             \
    } while (0)

#define EXPECT_NO_THROW(stmt)                                                    \
    do {                                                                         \
        bool ok = true;                                                          \
        try {                                                                    \
            stmt;                                                                \
        } catch (...) {                                                          \
            ok = false;                                                          \
        }                                                                        \
        SHIM_EXPECT(ok, "expected no throw from: " #stmt);                       \
    } while (0)

inline int RUN_ALL_TESTS() {
    int failed = 0;
    int total = 0;
    for (auto &c : testing::Registry::Cases()) {
        ++total;
        testing::Registry::CurrentFailed() = false;
        std::cout << "[ RUN      ] " << c.suite << "." << c.name << std::endl;
        try {
            c.fn();
        } catch (const testing::ShimFailure &) {
            // assertion aborted this test
        } catch (const std::exception &e) {
            testing::ReportFailure("<test>", 0, std::string("unexpected exception: ") + e.what());
        }
        if (testing::Registry::CurrentFailed()) {
            ++failed;
            std::cout << "[  FAILED  ] " << c.suite << "." << c.name << std::endl;
        } else {
            std::cout << "[       OK ] " << c.suite << "." << c.name << std::endl;
        }
    }
    std::cout << "\n[==========] " << total << " tests, " << failed << " failed." << std::endl;
    return failed == 0 ? 0 : 1;
}

#endif  // SCREENPARSER_TEST_GTEST_SHIM_H
