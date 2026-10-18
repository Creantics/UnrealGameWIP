#pragma once

#include <cstdio>
#include <cstdlib>

// Always-on invariant check: prints the failed condition and aborts. ftcore builds without
// exceptions, so a broken invariant stops the program instead of throwing.
#define FTCORE_ASSERT(cond)                                                                   \
    do {                                                                                      \
        if (!(cond)) {                                                                        \
            std::fprintf(stderr, "ftcore assertion failed: %s (%s:%d)\n", #cond, __FILE__,    \
                         __LINE__);                                                           \
            std::abort();                                                                     \
        }                                                                                     \
    } while (0)
