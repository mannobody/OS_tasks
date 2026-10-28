#include "mythread.h"
#include "assert.h"
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>   /* usleep */

/* Shared helpers ------------------------------------------------------ */

/*
 * Exit carrying an argument: whatever create() passed in becomes the
 * join() result. Used for the positive-value tests; the value check in
 * the caller is what detects that exit actually ran.
 */
static void *
worker_exit_arg(void *arg)
{
	mythread_exit(arg);
	return NULL;   /* unreachable if mythread_exit is correct */
}

/*
 * Same, but with a poisoned return path: if mythread_exit fails to stop
 * the thread and control falls through, join() reports the sentinel
 * value and the test fails — even though the expected value is NULL.
 */
static void *
worker_exit_null(void *arg)
{
	(void)arg;
	mythread_exit(NULL);
	return (void *)(intptr_t)1;
}

/*
 * [E1] Tripwire for "code after exit is not executed". If mythread_exit
 * returns to the caller instead of finishing the thread, the deliberate
 * null write raises SIGSEGV and the runner reports a crash — a loud,
 * unambiguous failure.
 */
static void *
worker_touch_after_exit(void *arg)
{
	(void)arg;
	mythread_exit(NULL);
	*(volatile int *)0 = 0xC0FFEE;
	return NULL;
}

/*
 * [E4, E9] Detached thread exiting on its own. Announces "about to
 * leave" in shared memory (the last thing it can do), then calls
 * mythread_exit. Nobody ever joins it; the observable requirements
 * are: the thread really ran, and the library stayed healthy after
 * reclaiming it.
 */
struct spark {
	volatile int fired;
};

static void *
worker_detached_exit(void *arg)
{
	struct spark *s = (struct spark *)arg;

	__atomic_store_n(&s->fired, 1, __ATOMIC_SEQ_CST);
	mythread_exit((void *)(intptr_t)0xD);
	return (void *)(intptr_t)-1;   /* unreachable */
}

/*
 * [E5] Join must block until the target thread exits, then deliver the
 * exit value. Choreography: the target parks; a helper wakes it only
 * after a delay — by then the main thread is already parked inside
 * join. If join returned early (without waiting), the value could not
 * be the one the target passed to mythread_exit.
 */
struct stage {
	volatile int go;
};

static void *
worker_hold(void *arg)
{
	struct stage *s = (struct stage *)arg;

	while (!__atomic_load_n(&s->go, __ATOMIC_SEQ_CST))
		; /* parked until the helper releases us */

	mythread_exit((void *)(intptr_t)0xE5);
	return (void *)(intptr_t)-1;   /* unreachable */
}

static void *
worker_release(void *arg)
{
	struct stage *s = (struct stage *)arg;

	usleep(50 * 1000);   /* let the main thread get stuck in join */
	__atomic_store_n(&s->go, 1, __ATOMIC_SEQ_CST);
	return NULL;
}

/*
 * [E8] Nested lifecycle with exit: the parent thread creates a child;
 * the child terminates via mythread_exit; the parent joins it and
 * relays the child's exit value upward as its own return value. If any
 * stage misbehaves, the sentinel -1/-2 codes flow to the main thread.
 */
struct nest_exit {
	mythread_t child;
};

static void *
worker_grandchild(void *arg)
{
	(void)arg;
	mythread_exit((void *)(intptr_t)0x2A);
	return (void *)(intptr_t)-1;   /* unreachable */
}

static void *
worker_nest_exit(void *arg)
{
	struct nest_exit *n = (struct nest_exit *)arg;

	if (mythread_create(&n->child, NULL, worker_grandchild, NULL) != 0)
		return (void *)(intptr_t)-1;

	void *rv = NULL;
	if (mythread_join(n->child, &rv) != 0)
		return (void *)(intptr_t)-2;

	return rv;   /* relays 0x2A from the grandchild's exit */
}

/* Tests --------------------------------------------------------------- */

/*
 * [E6] Exit with NULL and join receiving exactly NULL. Worker returns a
 * sentinel if control ever falls past mythread_exit, so a broken exit
 * cannot fake this test.
 */
int
test_exit_null(void)
{
	mythread_t t;

	ASSERT_SUCCESS(mythread_create(&t, NULL, worker_exit_null, NULL));

	void *ret = (void *)(intptr_t)0xdeadbeef;
	ASSERT_SUCCESS(mythread_join(t, &ret));
	ASSERT_NULL(ret);
	return TEST_SUCCESS;
}

/*
 * [E2] Exit value reaches the joiner unchanged.
 */
int
test_exit_retval(void)
{
	mythread_t t;

	ASSERT_SUCCESS(mythread_create(&t, NULL, worker_exit_arg,
	                               (void *)(intptr_t)99));

	void *ret = NULL;
	ASSERT_SUCCESS(mythread_join(t, &ret));
	ASSERT_EQ((intptr_t)ret, 99);
	return TEST_SUCCESS;
}

/*
 * [E1] Nothing after mythread_exit() runs.
 */
int
test_exit_stops_thread(void)
{
	mythread_t t;

	ASSERT_SUCCESS(mythread_create(&t, NULL, worker_touch_after_exit,
	                               NULL));
	ASSERT_SUCCESS(mythread_join(t, NULL));
	return TEST_SUCCESS;
}

/*
 * [E4, E9] A detached thread calls mythread_exit: it must terminate
 * normally with no one ever joining it, and the library must reclaim
 * its resources — probed by creating and joining a fresh thread after.
 */
int
test_exit_detached(void)
{
	struct spark s = { .fired = 0 };
	mythread_t t;

	ASSERT_SUCCESS(mythread_create(&t, NULL, worker_detached_exit, &s));
	ASSERT_SUCCESS(mythread_detach(t));

	/* the detached thread must run to its exit without being joined */
	while (!__atomic_load_n(&s.fired, __ATOMIC_SEQ_CST))
		;

	/* resources must be back: the library is still fully usable */
	mythread_t probe;
	ASSERT_SUCCESS(mythread_create(&probe, NULL, worker_exit_arg, NULL));
	ASSERT_SUCCESS(mythread_join(probe, NULL));

	return TEST_SUCCESS;
}

/*
 * [E5] Join blocks until the target exits and returns its value. The
 * helper thread wakes the target only ~50ms in, so the main thread is
 * provably inside join() at that moment; the value can only appear
 * after the target's mythread_exit.
 */
int
test_exit_join_blocks(void)
{
	struct stage st = { .go = 0 };
	mythread_t t_hold, t_release;

	ASSERT_SUCCESS(mythread_create(&t_hold, NULL, worker_hold, &st));
	ASSERT_SUCCESS(mythread_create(&t_release, NULL, worker_release,
	                               &st));

	void *ret = NULL;
	ASSERT_SUCCESS(mythread_join(t_hold, &ret));
	ASSERT_EQ((intptr_t)ret, (intptr_t)0xE5);

	ASSERT_SUCCESS(mythread_join(t_release, NULL));
	return TEST_SUCCESS;
}

/*
 * [E7] Exit immediately after create: no setup, no waiting — the
 * thread may finish before create even returns to the caller on some
 * schedulers. Repeated rounds make any create/exit race likely, and
 * every round must deliver the right value.
 */
int
test_exit_immediate(void)
{
	for (int round = 0; round < 100; ++round) {
		mythread_t t;

		ASSERT_SUCCESS(mythread_create(&t, NULL, worker_exit_arg,
		                               (void *)(intptr_t)7));
		void *ret = NULL;
		ASSERT_SUCCESS(mythread_join(t, &ret));
		ASSERT_EQ((intptr_t)ret, 7);
	}
	return TEST_SUCCESS;
}

/*
 * [E8] Child thread exits, parent joins — nested one level down. The
 * child's exit value travels: parent joins it, then hands it to the
 * main thread as its own return value. Sentinels -1 (create failed)
 * and -2 (join failed) would surface as a wrong retval here.
 */
int
test_exit_nested(void)
{
	struct nest_exit n = { .child = 0 };
	mythread_t parent;

	ASSERT_SUCCESS(mythread_create(&parent, NULL, worker_nest_exit, &n));

	void *ret = NULL;
	ASSERT_SUCCESS(mythread_join(parent, &ret));
	ASSERT_EQ((intptr_t)ret, (intptr_t)0x2A);
	ASSERT_TRUE(n.child != 0);   /* the nested child handle was real */

	return TEST_SUCCESS;
}

/*
 * [E3] mythread_exit() from the main thread when it is the only thread.
 * Nothing else is left to run, so the process must end. POSIX does
 * not pass the retval on as the exit status: with pthreads the last
 * thread exiting gives exit code 0. An implementation that turns it
 * into exit((int)retval) gives 42 instead; the test is flagged
 * TEST_EXITS_PROCESS in main.c, so the runner accepts both.
 *
 * What fails: mythread_exit returning to the caller (TEST_FAILURE,
 * exit code 1), a crash, or a process that never ends (timeout).
 */
int
test_exit_from_main(void)
{
	mythread_exit((void *)(intptr_t)42);
	/* unreachable if mythread_exit is correct */
	return TEST_FAILURE;
}

/*
 * Spinner for E10: raises "up", then spins until the process goes away.
 * Nothing ever releases it, so if it is still running after the exiter
 * is done, exit() did not take the whole thread group down.
 */
static void *
worker_spin_forever(void *arg)
{
	volatile int *up = (volatile int *)arg;

	__atomic_store_n(up, 1, __ATOMIC_SEQ_CST);
	for (;;)
		; /* only process exit ends this */
	return NULL;
}

#define E10_SPINNERS 3

struct e10ctx {
	volatile int up[E10_SPINNERS];
};

/* Waits until every spinner runs, then ends the process from here. */
static void *
worker_exit_process(void *arg)
{
	struct e10ctx *c = (struct e10ctx *)arg;

	for (int i = 0; i < E10_SPINNERS; ++i)
		while (!__atomic_load_n(&c->up[i], __ATOMIC_SEQ_CST))
			;
	exit(TEST_EXIT_MARKER);
}

/*
 * [E10] exit() called from an ordinary (non-main) thread ends the whole
 * process with that status, while main is blocked in join and other
 * threads are busy spinning. Flagged TEST_EXITS_PROCESS in main.c, so
 * exit code 42 is the pass.
 *
 * What fails: exit() ending only the calling thread — join returns and
 * main reports TEST_FAILURE; the spinners outliving exit() — the
 * runner sees no exit and reports a timeout; a crash in the exit path.
 */
int
test_exit_from_thread(void)
{
	static struct e10ctx c;
	static mythread_t spin[E10_SPINNERS];
	static mythread_t exiter;

	for (int i = 0; i < E10_SPINNERS; ++i)
		ASSERT_SUCCESS(mythread_create(&spin[i], NULL, worker_spin_forever,
		                               (void *)&c.up[i]));
	ASSERT_SUCCESS(mythread_create(&exiter, NULL, worker_exit_process, &c));

	(void)mythread_join(exiter, NULL);

	/* unreachable: exit() in the worker must have ended the process */
	fprintf(stderr, "  exit() from a thread did not end the process\n");
	return TEST_FAILURE;
}
