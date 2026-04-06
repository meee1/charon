// Minimal unit test harness for Charon
// No external dependencies — just assert + bookkeeping.

#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include <stdio.h>
#include <string.h>

static int _test_pass_count;
static int _test_fail_count;
static const char *_test_current;

#define TEST_BEGIN(name) \
    do { _test_current = (name); } while (0)

#define TEST_ASSERT(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "  FAIL: %s  (%s:%d)\n    assertion: %s\n", \
                    _test_current, __FILE__, __LINE__, #cond); \
            _test_fail_count++; \
            return; \
        } \
    } while (0)

#define TEST_ASSERT_MSG(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "  FAIL: %s  (%s:%d)\n    %s\n", \
                    _test_current, __FILE__, __LINE__, (msg)); \
            _test_fail_count++; \
            return; \
        } \
    } while (0)

#define TEST_PASS() \
    do { \
        fprintf(stderr, "  PASS: %s\n", _test_current); \
        _test_pass_count++; \
    } while (0)

#define RUN_TEST(fn) fn()

#define TEST_SUMMARY() \
    do { \
        fprintf(stderr, "\n%d passed, %d failed\n", _test_pass_count, _test_fail_count); \
    } while (0)

#define TEST_EXIT_CODE() (_test_fail_count ? 1 : 0)

#endif // TEST_HARNESS_H
