#pragma once
// Minimal assertion harness -- deliberately dependency-free, matching the rest
// of the project (no Catch2/GoogleTest, no vcpkg, nothing to install before
// `ctest` works). Each test executable registers cases with TEST(...) and calls
// runAllTests() from main; the process exit code is the number of failures, so
// CTest picks failures up without any extra glue.
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace testing {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

inline int& failureCount() {
    static int failures = 0;
    return failures;
}

inline bool& currentCaseFailed() {
    static bool failed = false;
    return failed;
}

struct Registrar {
    Registrar(const std::string& name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

inline void reportFailure(const char* file, int line, const std::string& message) {
    std::printf("    FAIL %s:%d\n      %s\n", file, line, message.c_str());
    currentCaseFailed() = true;
}

inline void check(bool condition, const char* file, int line, const std::string& message) {
    if (!condition) reportFailure(file, line, message);
}

inline void checkNear(double actual, double expected, double tolerance, const char* file, int line,
                      const std::string& what) {
    const double diff = std::abs(actual - expected);
    if (!(diff <= tolerance)) {
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "%s: got %.10g, expected %.10g (|diff| = %.4g > tolerance %.4g)",
                      what.c_str(), actual, expected, diff, tolerance);
        reportFailure(file, line, buf);
    }
}

inline void checkBetween(double actual, double low, double high, const char* file, int line,
                         const std::string& what) {
    if (!(actual >= low && actual <= high)) {
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%s: got %.10g, expected within [%.10g, %.10g]",
                      what.c_str(), actual, low, high);
        reportFailure(file, line, buf);
    }
}

inline int runAllTests(const char* suiteName) {
    std::printf("=== %s: %zu test cases ===\n", suiteName, registry().size());
    for (const auto& tc : registry()) {
        currentCaseFailed() = false;
        std::printf("  %s\n", tc.name.c_str());
        tc.fn();
        if (currentCaseFailed()) ++failureCount();
    }
    if (failureCount() == 0) {
        std::printf("=== %s: ALL PASSED ===\n", suiteName);
    } else {
        std::printf("=== %s: %d FAILED ===\n", suiteName, failureCount());
    }
    return failureCount();
}

}  // namespace testing

#define TEST(name)                                                            \
    static void name();                                                       \
    static testing::Registrar registrar_##name(#name, name);                  \
    static void name()

#define CHECK(cond) testing::check((cond), __FILE__, __LINE__, "CHECK(" #cond ") failed")
#define CHECK_MSG(cond, msg) testing::check((cond), __FILE__, __LINE__, (msg))
#define CHECK_NEAR(actual, expected, tol) \
    testing::checkNear((actual), (expected), (tol), __FILE__, __LINE__, #actual)
#define CHECK_BETWEEN(actual, low, high) \
    testing::checkBetween((actual), (low), (high), __FILE__, __LINE__, #actual)
