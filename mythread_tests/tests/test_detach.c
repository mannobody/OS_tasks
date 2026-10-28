#include "mythread.h"
#include "assert.h"
#include <stdint.h>

/* Shared helpers ------------------------------------------------------ */

static void *
worker_noop(void *arg)
{
	(void)arg;
	return NULL;
}

/*
 * Self-describing worker: stores its own handle where the main thread
 * can see it, then parks. Lets a test check that an already-detached
 * thread still runs to completion and its resources come back.
 */
struct park {
	volatile int parked;
	volatile int go;
};

static void *
worker_park(void *arg)
{
	struct park *p = (struct park *)arg;

	__atomic_store_n(&p->parked, 1, __ATOMIC_SEQ_CST);
	while (!__atomic_load_n(&p->go, __ATOMIC_SEQ_CST))
		; /* spin until released */

	return (void *)(intptr_t)7;
}

/*
 * [D1] Basic detach: a running thread is detached, then finishes on its
 * own. The call must succeed, no join is needed, and — the important
 * part — the thread must actually run to completion now that nobody
 * will ever wait for it. If detach had killed it or left it orphaned,
 * the worker would never write its result.
 */
int
test_detach_running(void)
{
	struct park p = { .parked = 0, .go = 0 };
	mythread_t t;

	ASSERT_SUCCESS(mythread_create(&t, NULL, worker_park, &p));
	ASSERT_SUCCESS(mythread_detach(t));

	/* the detached thread must still be alive and scheduled */
	while (!__atomic_load_n(&p.parked, __ATOMIC_SEQ_CST))
		;

	__atomic_store_n(&p.go, 1, __ATOMIC_SEQ_CST);
	return TEST_SUCCESS;
}

/*
 * [D7] Detach an already-exited, never-joined thread. POSIX gives no
 * single answer; student implementations may either report success or
 * an error code. The real requirement is that the library stays
 * healthy afterwards — creation must keep working.
 */
int
test_detach_after_exit(void)
{
	mythread_t t;

	ASSERT_SUCCESS(mythread_create(&t, NULL, worker_noop, NULL));
	/* no join here — the thread is gone, its exit status is pending */

	/* detach may return 0 or an error, both are acceptable [D2] */
	(void)mythread_detach(t);

	/* whatever the outcome, the library must not be poisoned [D7] */
	mythread_t t2;
	ASSERT_SUCCESS(mythread_create(&t2, NULL, worker_noop, NULL));
	ASSERT_SUCCESS(mythread_join(t2, NULL));

	return TEST_SUCCESS;
}

/*
 * [D3] Double detach. The first call succeeds; a second one on the same
 * handle is continuing an already-finished story. POSIX leaves it
 * undefined, and glibc may already have freed the descriptor of the
 * detached thread, so — like join_dead — weak mode does not even make
 * the second call. With -DMYTHREAD_STRICT_ERRORS it must return an
 * explicit error code.
 */
int
test_detach_double(void)
{
	mythread_t t;

	ASSERT_SUCCESS(mythread_create(&t, NULL, worker_noop, NULL));
	ASSERT_SUCCESS(mythread_detach(t));

#ifdef MYTHREAD_STRICT_ERRORS
	ASSERT_FAILURE(mythread_detach(t));
#endif
	return TEST_SUCCESS;
}

/*
 * [D4] Detach after join. A handle whose thread was already joined is
 * no longer valid; using it is undefined (glibc freed the descriptor at
 * join). Same policy as above: the call is made only in strict mode,
 * where it must return an explicit error.
 */
int
test_detach_after_join(void)
{
	mythread_t t;

	ASSERT_SUCCESS(mythread_create(&t, NULL, worker_noop, NULL));
	ASSERT_SUCCESS(mythread_join(t, NULL));

#ifdef MYTHREAD_STRICT_ERRORS
	ASSERT_FAILURE(mythread_detach(t));
#endif
	return TEST_SUCCESS;
}

/*
 * [D5, D10] Invalid handles: zero. POSIX marks detach(0) undefined and
 * even glibc segfaults on it (exactly like pthread_join(0)), so — same
 * policy as error_join_invalid — the weak expectation only holds the
 * runner's safety net: if the library survives the call, fine. The
 * real requirement (explicit error code) is checked only when built
 * with -DMYTHREAD_STRICT_ERRORS.
 */
int
test_detach_invalid(void)
{
	/* Not even run in weak mode: glibc's pthread crashes on it, and a
	 * thin pthread wrapper would be unfairly penalized. */
#if defined(MYTHREAD_STRICT_ERRORS)
	mythread_t zero = (mythread_t)0;
	int rc = mythread_detach(zero);
	ASSERT_FAILURE(rc);
#endif
	return TEST_SUCCESS;
}

/*
 * [D9] Scalability: detach many threads in a row. Every one of them
 * must accept the detach without disturbing its siblings, and the
 * library must stay functional afterwards.
 */
int
test_detach_many(void)
{
#define N 16
	static struct park p[N];
	static mythread_t t[N];

	for (int i = 0; i < N; ++i) {
		p[i].parked = 0;
		p[i].go = 0;
		ASSERT_SUCCESS(mythread_create(&t[i], NULL, worker_park, &p[i]));
		ASSERT_SUCCESS(mythread_detach(t[i]));
	}

	/* all detached workers must still run concurrently */
	for (int i = 0; i < N; ++i)
		while (!__atomic_load_n(&p[i].parked, __ATOMIC_SEQ_CST))
			;

	for (int i = 0; i < N; ++i)
		__atomic_store_n(&p[i].go, 1, __ATOMIC_SEQ_CST);

#undef N
	return TEST_SUCCESS;
}

/*
 * [D8] Detach racing with thread exit. The worker exits almost
 * immediately; wherever detach lands relative to the exit — before,
 * during, after — exactly one valid outcome is possible (the call
 * succeeds or reports the thread is gone), and the library must
 * survive either. Repeated rounds make hitting the race likely.
 */
static void *
worker_instant(void *arg)
{
	(void)arg;
	return NULL;
}

int
test_detach_race(void)
{
	for (int round = 0; round < 64; ++round) {
		mythread_t t;
		ASSERT_SUCCESS(mythread_create(&t, NULL, worker_instant, NULL));
		(void)mythread_detach(t);

		/* library must remain usable after each round [D8] */
		mythread_t probe;
		ASSERT_SUCCESS(mythread_create(&probe, NULL, worker_noop, NULL));
		ASSERT_SUCCESS(mythread_join(probe, NULL));
	}
	return TEST_SUCCESS;
}
