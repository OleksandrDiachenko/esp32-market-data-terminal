#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks;

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        g_checks++;                                                                                                    \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);                            \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

#define CHECK_STREQ(actual, expected) CHECK(strcmp((actual), (expected)) == 0)
