#include "mythread.h"
#include "assert.h"

/* Negative tests.
 *
 * POSIX marks these situations "undefined behaviour", so implementations
 * legitimately differ: glibc segfaults on pthread_join(0), others return
 * ESRCH/EINVAL. The requirement for a student library therefore is only
 * the weakest one worth having: not crashing the caller and not hanging.
 * Tests here pass if the call returns *any* value; the runner catches a
 * segfault or hang otherwise. Compile with -DMYTHREAD_STRICT_ERRORS to
 * additionally require an explicit error code return. */

static void *worker_noop(void *arg) { (void)arg; return NULL; }

/* NULL start_routine: must be rejected or (weak mode) tolerated. */
int test_error_null_routine(void) {
	mythread_t t;
	int rc = mythread_create(&t, NULL, NULL, NULL);
#ifdef MYTHREAD_STRICT_ERRORS
	ASSERT_FAILURE(rc);
#else
	(void)rc;
	(void)t;
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

/* Joining yourself must be detected (EDEADLK in POSIX), but at minimum
 * must not deadlock the whole suite: the runner timeout catches that. */
struct self_arg { mythread_t self; };

static void *worker_self_join(void *arg) {
	struct self_arg *a = (struct self_arg *)arg;
	/* Wait for the main thread to publish our own handle. Unbounded on
	 * purpose: a bounded spin could expire under load and turn this
	 * into join(0) — real UB in glibc, not the self-join we test. */
	while (__atomic_load_n(&a->self, __ATOMIC_SEQ_CST) == 0)
		;
	(void)mythread_join(a->self, NULL);
	return NULL;
}

int test_error_join_self(void) {
	struct self_arg a = { .self = 0 };
	mythread_t t;
	ASSERT_SUCCESS(mythread_create(&t, NULL, worker_self_join, &a));
	a.self = t;
	ASSERT_SUCCESS(mythread_join(t, NULL));
	return TEST_SUCCESS;
}

/* Silence -Wunused-function in strict mode. */
struct noop_keeper { void *(*fn)(void *); };
__attribute__((used)) static struct noop_keeper keep_noop = { worker_noop };
