#pragma once

// A very small test harness with no dependencies, so the same tests build
// unchanged on macOS (10.13 deployment target), Windows and Linux.
//
//   TEST_CASE("name") { CHECK(x == y); CHECK_NEAR(a, b, tolerance); }
//
// Run all:            RootnoteCoreTests
// Run a subset:       RootnoteCoreTests <substring of the test name> ...
// List the tests:     RootnoteCoreTests --list

#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

namespace testing
{
struct TestCase
{
    const char* name;
    void (*run)();
};

inline std::vector<TestCase>& registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar
{
    Registrar(const char* name, void (*run)()) { registry().push_back({ name, run }); }
};

struct State
{
    int checks = 0;
    int failures = 0;
    int failuresInTest = 0;
};

inline State& state()
{
    static State s;
    return s;
}

struct RequireFailed : std::exception
{
};

inline void report(bool ok, const char* file, int line, const std::string& what)
{
    ++state().checks;
    if (ok)
        return;
    ++state().failures;
    ++state().failuresInTest;
    if (state().failuresInTest <= 12)
        std::printf("    FAIL %s:%d  %s\n", file, line, what.c_str());
    else if (state().failuresInTest == 13)
        std::printf("    ... further failures in this test are not listed\n");
}

inline std::string describe(double a, double b, double tolerance)
{
    char text[200];
    std::snprintf(text, sizeof(text), "got %.9g, expected %.9g +- %.3g (off by %.3g)", a, b, tolerance, a - b);
    return text;
}

inline int runAll(int argc, char** argv)
{
    std::vector<std::string> filters;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--list") == 0)
        {
            for (const auto& test : registry())
                std::printf("%s\n", test.name);
            return 0;
        }
        filters.emplace_back(argv[i]);
    }

    int ran = 0;
    int failedTests = 0;
    for (const auto& test : registry())
    {
        bool selected = filters.empty();
        for (const auto& filter : filters)
            selected = selected || std::string(test.name).find(filter) != std::string::npos;
        if (!selected)
            continue;

        state().failuresInTest = 0;
        std::printf("[ RUN  ] %s\n", test.name);
        std::fflush(stdout);
        try
        {
            test.run();
        }
        catch (const RequireFailed&)
        {
        }
        catch (const std::exception& e)
        {
            report(false, "(exception)", 0, e.what());
        }
        ++ran;
        if (state().failuresInTest != 0)
            ++failedTests;
        std::printf("[ %s ] %s\n", state().failuresInTest == 0 ? " OK " : "FAIL", test.name);
        std::fflush(stdout);
    }

    std::printf("\n%d test(s), %d check(s), %d failed test(s), %d failed check(s)\n",
                ran, state().checks, failedTests, state().failures);
    if (ran == 0)
    {
        std::printf("no test matched the filter\n");
        return 2;
    }
    return failedTests == 0 ? 0 : 1;
}
} // namespace testing

#define TESTING_JOIN2(a, b) a##b
#define TESTING_JOIN(a, b) TESTING_JOIN2(a, b)

#define TEST_CASE(name)                                                                              \
    static void TESTING_JOIN(testBody, __LINE__)();                                                  \
    static const ::testing::Registrar TESTING_JOIN(testRegistrar, __LINE__)(name, &TESTING_JOIN(testBody, __LINE__)); \
    static void TESTING_JOIN(testBody, __LINE__)()

#define CHECK(condition) ::testing::report(static_cast<bool>(condition), __FILE__, __LINE__, #condition)

#define CHECK_MSG(condition, message) \
    ::testing::report(static_cast<bool>(condition), __FILE__, __LINE__, std::string(#condition) + "  [" + (message) + "]")

#define CHECK_NEAR(actual, expected, tolerance)                                                        \
    do                                                                                                 \
    {                                                                                                  \
        const double testingA = static_cast<double>(actual);                                           \
        const double testingB = static_cast<double>(expected);                                         \
        const double testingT = static_cast<double>(tolerance);                                        \
        ::testing::report(std::isfinite(testingA) && std::abs(testingA - testingB) <= testingT, __FILE__, __LINE__, \
                          std::string(#actual " ~ " #expected ": ") + ::testing::describe(testingA, testingB, testingT)); \
    } while (false)

#define CHECK_NEAR_MSG(actual, expected, tolerance, message)                                           \
    do                                                                                                 \
    {                                                                                                  \
        const double testingA = static_cast<double>(actual);                                           \
        const double testingB = static_cast<double>(expected);                                         \
        const double testingT = static_cast<double>(tolerance);                                        \
        ::testing::report(std::isfinite(testingA) && std::abs(testingA - testingB) <= testingT, __FILE__, __LINE__, \
                          std::string(#actual " ~ " #expected ": ") + ::testing::describe(testingA, testingB, testingT) \
                              + "  [" + (message) + "]");                                              \
    } while (false)

#define REQUIRE(condition)                                                              \
    do                                                                                  \
    {                                                                                   \
        const bool testingOk = static_cast<bool>(condition);                            \
        ::testing::report(testingOk, __FILE__, __LINE__, "REQUIRE " #condition);        \
        if (!testingOk)                                                                 \
            throw ::testing::RequireFailed();                                           \
    } while (false)
