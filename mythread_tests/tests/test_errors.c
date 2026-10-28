#include "mythread.h"
#include "assert.h"

/* Negative tests.
 *
 * POSIX marks these situations "undefined behaviour", so implementations
 * legitimately differ: glibc segfaults on pthread_join(0), others return
 * ESRCH/EINVAL. Making such a call proves nothing about a library that
 * does not promise to validate its input — a thin pthread wrapper would
 * crash for reasons outside the student's control. So the calls are
 * made only when built with -DMYTHREAD_STRICT_ERRORS, which requires an
 * explicit error code return; in weak mode these tests pass trivially. */

static void *worker_noop(void *arg) { (void)arg; return NULL; }

/* NULL start_routine must be rejected. Strict mode only: pthread
 * happily starts a thread that jumps to address 0, and whether that
 * crash lands before the test returns is pure timing. */
int test_error_null_routine(void) {
#ifdef MYTHREAD_STRICT_ERRORS
	mythread_t t;
	ASSERT_FAILURE(mythread_create(&t, NULL, NULL, NULL));
#endif
	return TEST_SUCCESS;
}

/* Joining an invalid handle. Not even glibc does this safely
 * (pthread_join(0) segfaults), so the test only runs in strict mode:
 * a student implementation that validates its handles passes, a thin
 * wrapper over pthreads is simply not penalized. */
int test_error_join_invalid(void) {
#if defined(MYTHREAD_STRICT_ERRORS)
	mythread_t t = (mythread_t)0;
	int rc = mythread_join(t, NULL);
	ASSERT_FAILURE(rc);
#endif
	return TEST_SUCCESS;
}

/* Joining yourself must be detected (EDEADLK in POSIX) instead of
 * deadlocking; a hang is caught by the runner timeout. POSIX leaves
 * self-join undefined, and under TSan the pthread_join interceptor
 * aborts on it, so this too runs only in strict mode. */
#ifdef MYTHREAD_STRICT_ERRORS
struct self_arg { mythread_t self; int rc; };

static void *worker_self_join(void *arg) {
	struct self_arg *a = (struct self_arg *)arg;
	/* Wait for the main thread to publish our own handle. Unbounded on
	 * purpose: a bounded spin could expire under load and turn this
	 * into join(0) — real UB in glibc, not the self-join we test. */
	while (__atomic_load_n(&a->self, __ATOMIC_SEQ_CST) == 0)
		;
	a->rc = mythread_join(a->self, NULL);
	return NULL;
}
#endif

int test_error_join_self(void) {
#ifdef MYTHREAD_STRICT_ERRORS
	struct self_arg a = { .self = 0, .rc = 0 };
	mythread_t t;
	ASSERT_SUCCESS(mythread_create(&t, NULL, worker_self_join, &a));
	__atomic_store_n(&a.self, t, __ATOMIC_SEQ_CST);
	ASSERT_SUCCESS(mythread_join(t, NULL));
	ASSERT_FAILURE(a.rc);
#endif
	return TEST_SUCCESS;
}

/* Every test above is strict-only; keep the helper referenced so weak
 * builds do not warn with -Wunused-function. */
struct noop_keeper { void *(*fn)(void *); };
__attribute__((used)) static struct noop_keeper keep_noop = { worker_noop };
