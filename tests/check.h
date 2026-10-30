#ifndef LLSTREAM_TEST_CHECK_H
#define LLSTREAM_TEST_CHECK_H

#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

#endif
