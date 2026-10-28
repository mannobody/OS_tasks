#ifndef RUNNER_H
#define RUNNER_H

typedef int (*test_fn_t)(void);

typedef struct {
    const char *name;
    test_fn_t   fn;
    int         timeout_ms;   /* 0 by default */
    int         flags;        /* TEST_* flags below, 0 by default */
} test_case_t;

/* The test ends the whole process itself (mythread_exit or exit from
 * main) instead of returning; only then is TEST_EXIT_MARKER a pass. */
#define TEST_EXITS_PROCESS  (1 << 0)

#define TEST_SUCCESS  0
#define TEST_FAILURE  1
#define TEST_TIMEOUT  2   /* set by the runner, not by tests */
#define TEST_SKIP     3   /* the test does not apply in this build or
                           * environment; counted as skipped, not failed */

/* Special-case marker for tests that terminate the process themselves
 * (e.g. mythread_exit from main): the runner treats exit code 42 as
 * success, but only for tests flagged TEST_EXITS_PROCESS — anywhere
 * else a stray exit(42) is a failure like any other exit code. */
#define TEST_EXIT_MARKER 42

int run_all(const test_case_t *tests, int n);

/* Emulates "return from main": ends the test child as if its main()
 * had returned `value` (a full exit(), so the process semantics with
 * still-running detached threads are exercised). Never returns.
 * Only meaningful inside a test run by the runner. */
void test_return_main(int value);

#endif
