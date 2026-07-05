#pragma once

// Minimal check/summary macros for host-side tests. No external test
// framework dependency by design (see docs/testing.md).

#include <stdio.h>
#include <string.h>

static int test_failures = 0;
static int test_count = 0;

#define CHECK(cond)                                                        \
    do                                                                     \
    {                                                                      \
        test_count++;                                                     \
        if (!(cond))                                                      \
        {                                                                 \
            test_failures++;                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                  \
    } while (0)

#define CHECK_STREQ(a, b) CHECK(strcmp((a), (b)) == 0)

static int test_summary(const char *suite_name)
{
    printf("[%s] %d/%d checks passed\n", suite_name, test_count - test_failures, test_count);
    return test_failures == 0 ? 0 : 1;
}
