#pragma once

#include <stdio.h>
#include <math.h>

// A deliberately tiny assertion harness.
//
// No test framework is pulled in — adding a dependency needs asking first, and
// nothing here justifies one. Thirty lines of macros give named checks, a
// running count and a non-zero exit code on failure, which is everything a
// build server or a person at a terminal actually needs.

namespace testing {

inline int checks_run = 0;
inline int checks_failed = 0;
inline const char *current_suite = "";

inline void suite(const char *name)
{
    current_suite = name;
    printf("\n--- %s ---\n", name);
}

inline void record(bool passed, const char *expression, const char *file, int line)
{
    checks_run++;
    if (!passed) {
        checks_failed++;
        printf("  FAIL  %s:%d\n        %s\n", file, line, expression);
    }
}

inline void case_passed(const char *description)
{
    printf("  ok    %s\n", description);
}

inline int summary()
{
    printf("\n=========================================\n");
    printf("%d checks, %d failed\n", checks_run, checks_failed);
    printf("%s\n", checks_failed == 0 ? "ALL TESTS PASSED" : "*** TESTS FAILED ***");
    printf("=========================================\n");
    return checks_failed == 0 ? 0 : 1;
}

} // namespace testing

/// Assert a condition, reporting the source expression when it fails.
#define CHECK(cond) \
    ::testing::record((cond), #cond, __FILE__, __LINE__)

/// Assert two integers are equal, printing both values when they are not —
/// "expected 400, got 200" is far more use than "the assertion failed".
#define CHECK_EQ(actual, expected)                                            \
    do {                                                                      \
        const long long a_ = (long long)(actual);                             \
        const long long e_ = (long long)(expected);                           \
        ::testing::checks_run++;                                              \
        if (a_ != e_) {                                                       \
            ::testing::checks_failed++;                                       \
            printf("  FAIL  %s:%d\n        %s\n        expected %lld, got %lld\n", \
                   __FILE__, __LINE__, #actual, e_, a_);                      \
        }                                                                     \
    } while (0)

/// Assert two doubles are equal within a tolerance.
#define CHECK_NEAR(actual, expected, tolerance)                               \
    do {                                                                      \
        const double a_ = (double)(actual);                                   \
        const double e_ = (double)(expected);                                 \
        ::testing::checks_run++;                                              \
        if (fabs(a_ - e_) > (tolerance)) {                                    \
            ::testing::checks_failed++;                                       \
            printf("  FAIL  %s:%d\n        %s\n        expected %.3f +/- %.3f, got %.3f\n", \
                   __FILE__, __LINE__, #actual, e_, (double)(tolerance), a_); \
        }                                                                     \
    } while (0)
