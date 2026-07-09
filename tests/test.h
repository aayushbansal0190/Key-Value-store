// Minimal test support: CHECK macros + a summary. No framework, by design —
// the whole "runner" is: each tests/test_*.cpp is an executable, `make test`
// runs them all, and a non-zero exit fails the make.
#pragma once

#include <cstdio>

static int g_checks_failed = 0;
static int g_checks_run = 0;

// Not assert(): we want to keep running after a failure so one broken case
// reports every check it breaks, and assert() vanishes under -DNDEBUG.
#define CHECK(cond)                                                        \
    do {                                                                   \
        g_checks_run++;                                                    \
        if (!(cond)) {                                                     \
            g_checks_failed++;                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                  \
    } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

// Call at the end of each test main(); its return value is main's exit code.
static inline int test_summary(const char* name) {
    if (g_checks_failed == 0) {
        printf("PASS %s (%d checks)\n", name, g_checks_run);
        return 0;
    }
    fprintf(stderr, "FAIL %s: %d of %d checks failed\n", name, g_checks_failed, g_checks_run);
    return 1;
}
