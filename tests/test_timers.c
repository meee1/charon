//MIT License
//
//Copyright (c) 2018 tvelliott
//
//Permission is hereby granted, free of charge, to any person obtaining a copy
//of this software and associated documentation files (the "Software"), to deal
//in the Software without restriction, including without limitation the rights
//to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//copies of the Software, and to permit persons to whom the Software is
//furnished to do so, subject to the following conditions:
//
//The above copyright notice and this permission notice shall be included in all
//copies or substantial portions of the Software.
//
//THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
//SOFTWARE.

// Unit tests for timers.c — microsecond-resolution timer functions

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "test_harness.h"

#include "../timers.c"

///////////////////////////////////////////////////////////////////////////////
// Tests
///////////////////////////////////////////////////////////////////////////////

static void test_create_timer(void)
{
    TEST_BEGIN("timer: create_timer returns non-NULL");
    timer_obj *t = create_timer();
    TEST_ASSERT(t != NULL);
    free(t);
    TEST_PASS();
}

static void test_timer_reset_and_elapsed(void)
{
    TEST_BEGIN("timer: elapsed is near zero immediately after reset");
    timer_obj *t = create_timer();
    timer_reset(t);
    long long elapsed = timer_elapsed_usec(t);
    // Should be very small — less than 1 ms (1000 us)
    TEST_ASSERT(elapsed >= 0);
    TEST_ASSERT(elapsed < 1000);
    free(t);
    TEST_PASS();
}

static void test_timer_elapsed_increases(void)
{
    TEST_BEGIN("timer: elapsed increases after usleep");
    timer_obj *t = create_timer();
    timer_reset(t);
    usleep(10000);  // 10 ms
    long long elapsed = timer_elapsed_usec(t);
    // Should be at least 5 ms (5000 us) — allowing scheduler jitter
    TEST_ASSERT_MSG(elapsed >= 5000, "elapsed should be >= 5ms after 10ms sleep");
    // Should not be wildly off — less than 100 ms
    TEST_ASSERT_MSG(elapsed < 100000, "elapsed should be < 100ms after 10ms sleep");
    free(t);
    TEST_PASS();
}

static void test_timer_now_usec(void)
{
    TEST_BEGIN("timer: timer_now_usec returns positive value");
    long long now = timer_now_usec();
    TEST_ASSERT(now > 0);
    TEST_PASS();
}

static void test_timer_now_increases(void)
{
    TEST_BEGIN("timer: timer_now_usec is monotonically increasing");
    long long t1 = timer_now_usec();
    usleep(1000);  // 1 ms
    long long t2 = timer_now_usec();
    TEST_ASSERT(t2 > t1);
    TEST_PASS();
}

static void test_timer_elapsed_since(void)
{
    TEST_BEGIN("timer: timer_elapsed_since computes correct delta");
    timer_obj *t = create_timer();
    timer_reset(t);
    usleep(10000);  // 10 ms
    long long now = timer_now_usec();
    long long elapsed = timer_elapsed_since(t, now);
    TEST_ASSERT(elapsed >= 5000);
    TEST_ASSERT(elapsed < 100000);
    free(t);
    TEST_PASS();
}

static void test_timer_multiple_resets(void)
{
    TEST_BEGIN("timer: reset restarts the clock");
    timer_obj *t = create_timer();
    timer_reset(t);
    usleep(10000);  // 10 ms
    long long e1 = timer_elapsed_usec(t);
    TEST_ASSERT(e1 >= 5000);

    // Reset again
    timer_reset(t);
    long long e2 = timer_elapsed_usec(t);
    // After fresh reset, should be near zero again
    TEST_ASSERT(e2 < 1000);
    free(t);
    TEST_PASS();
}

///////////////////////////////////////////////////////////////////////////////
// main
///////////////////////////////////////////////////////////////////////////////

int main(void)
{
    fprintf(stderr, "=== Timer Tests ===\n");

    RUN_TEST(test_create_timer);
    RUN_TEST(test_timer_reset_and_elapsed);
    RUN_TEST(test_timer_elapsed_increases);
    RUN_TEST(test_timer_now_usec);
    RUN_TEST(test_timer_now_increases);
    RUN_TEST(test_timer_elapsed_since);
    RUN_TEST(test_timer_multiple_resets);

    TEST_SUMMARY();
    return TEST_EXIT_CODE();
}
