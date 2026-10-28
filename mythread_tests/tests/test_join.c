#include "mythread.h"
#include "assert.h"
#include <stdint.h>
#include <unistd.h>   /* usleep */

/* Coverage map (J1..J14):
 *
 *   J1  join_blocks           blocks until exit, value correct
 *   J2  join_already_exited   terminated-but-unjoined: immediate, value ok
 *   J3  -> tests/test_errors.c  error_join_invalid (strict mode: glibc
 *                             segfaults on pthread_join(0))
 *   J4  join_detached         join of a detached thread (POSIX: UB)
 *   J5  join_dead             second join of the same handle (UB)
 *   J6  -> tests/test_errors.c  error_join_self (EDEADLK / no deadlock)
 *   J7  join_null_retval      NULL retval pointer is fine
 *   J8  join_multiple         reverse-order joins all succeed
 *   J9  join_concurrent       many threads join the same target (UB):
 *                             >=1 gets the value, winners are consistent
 *   J10 join_detached         join after detach — same call shape as J4
 *   J11 -> tests/test_exit.c   exit_retval (value comes from exit)
 *   J12 join_dead (strict)    stale handle must be rejected explicitly
 *   J13 join_from_thread      one thread joins another thread
 *   J14 N/A                   timed join is not part of the 1.6/1.7 API
 *
 * Default build keeps POSIX-UB cases weak (no crash, no hang — the
 * runner catches both); -DMYTHREAD_STRICT_ERRORS additionally demands
 * explicit error codes where the call must fail.
 */

/* Shared helpers ------------------------------------------------------ */

static void *
worker_noop(void *arg)
{
	(void)arg;
	return NULL;
}

static void *
worker_echo(void *arg)
{
	return arg;
}

/*
 * For [J1]: a parked worker released by a helper after a delay. The
 * main thread is provably inside join() when the release happens, so
 * a join that "returns early" cannot deliver the correct value.
 */
struct stage {
	volatile int go;
};

static void *
worker_hold(void *arg)
{
	struct stage *s = (struct stage *)arg;

	while (!__atomic_load_n(&s->go, __ATOMIC_SEQ_CST))
		; /* parked until released */

	return (void *)(intptr_t)0x51;
}

static void *
worker_release(void *arg)
{
	struct stage *s = (struct stage *)arg;

	usleep(50 * 1000);
	__atomic_store_n(&s->go, 1, __ATOMIC_SEQ_CST);
	return NULL;
}

/*
 * For [J4, J10]: a self-reporting parked worker, detached while alive.
 * Keeping it running (descriptor stays valid) makes even glibc answer
 * the join attempt deterministically instead of tripping on freed
 * memory.
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
		; /* parked until released */

	return NULL;
}

/* Tests --------------------------------------------------------------- */

/*
 * [J7] retval == NULL must be accepted: the caller is not interested
 * in the result.
 */
int
test_join_null_retval(void)
{
	mythread_t t;

	ASSERT_SUCCESS(mythread_create(&t, NULL, worker_noop, NULL));
	ASSERT_SUCCESS(mythread_join(t, NULL));
	return TEST_SUCCESS;
}

/*
 * Basic value transport: the pointer handed to create() is exactly
 * what join() hands back. Part of the [J1]/[J2] contract.
 */
int
test_join_retval(void)
{
	mythread_t t;
	int x = 7;

	ASSERT_SUCCESS(mythread_create(&t, NULL, worker_echo, &x));

	void *ret = NULL;
	ASSERT_SUCCESS(mythread_join(t, &ret));
	ASSERT_TRUE(ret == &x);
	return TEST_SUCCESS;
}

/*
 * [J8] Join order must not matter: N threads created forward, joined
 * in reverse — every value must still land with its own owner.
 * Catches implementations that overwrite a shared "last result" slot.
 */
int
test_join_multiple(void)
{
#define N 8
	mythread_t t[N];
	int vals[N];

	for (int i = 0; i < N; ++i) {
		vals[i] = (int)(i + 1);
		ASSERT_SUCCESS(mythread_create(&t[i], NULL, worker_echo,
		                               &vals[i]));
	}
	for (int i = N - 1; i >= 0; --i) {
		void *ret = NULL;
		ASSERT_SUCCESS(mythread_join(t[i], &ret));
		ASSERT_TRUE(ret == &vals[i]);
	}
#undef N
	return TEST_SUCCESS;
}

/*
 * [J1] Join of a *running* thread must block until it exits, then
 * deliver its value. The helper wakes the worker only 50ms in — by
 * then the main thread is already parked inside join, so the value
 * could not have been produced without waiting.
 */
int
test_join_blocks(void)
{
	struct stage st = { .go = 0 };
	mythread_t t_hold, t_release;

	ASSERT_SUCCESS(mythread_create(&t_hold, NULL, worker_hold, &st));
	ASSERT_SUCCESS(mythread_create(&t_release, NULL, worker_release,
	                               &st));

	void *ret = NULL;
	ASSERT_SUCCESS(mythread_join(t_hold, &ret));
	ASSERT_EQ((intptr_t)ret, (intptr_t)0x51);

	ASSERT_SUCCESS(mythread_join(t_release, NULL));
	return TEST_SUCCESS;
}

/*
 * [J2] Join an already-exited, never-joined thread. POSIX guarantees
 * this works: the result is kept until someone reaps it. The 100ms
 * delay makes "already exited" certain on any scheduler; the test
 * fails the implementation that loses the result or hangs.
 */
int
test_join_already_exited(void)
{
	mythread_t t;
	int x = 77;

	ASSERT_SUCCESS(mythread_create(&t, NULL, worker_echo, &x));
	usleep(100 * 1000);   /* the thread is definitely finished now */

	void *ret = NULL;
	ASSERT_SUCCESS(mythread_join(t, &ret));
	ASSERT_TRUE(ret == &x);
	return TEST_SUCCESS;
}

/*
 * [J4, J10] Join of a detached thread. POSIX calls this undefined;
 * real implementations answer with EINVAL/ESRCH or, badly, hang, and
 * the TSan pthread_join interceptor aborts. So weak mode does not make the
 * call and only demands that detaching a parked thread leaves the
 * library usable (the worker is released, a probe create/join works).
 * Strict mode requires an explicit error code.
 */
int
test_join_detached(void)
{
	struct park p = { .parked = 0, .go = 0 };
	mythread_t t;

	ASSERT_SUCCESS(mythread_create(&t, NULL, worker_park, &p));
	ASSERT_SUCCESS(mythread_detach(t));

#ifdef MYTHREAD_STRICT_ERRORS
	ASSERT_FAILURE(mythread_join(t, NULL));
#endif

	/* let the detached thread finish; nobody joins it */
	__atomic_store_n(&p.go, 1, __ATOMIC_SEQ_CST);

	/* the library must be unharmed by the bogus join */
	mythread_t probe;
	ASSERT_SUCCESS(mythread_create(&probe, NULL, worker_noop, NULL));
	ASSERT_SUCCESS(mythread_join(probe, NULL));
	return TEST_SUCCESS;
}

/*
 * [J5, J12] State machine: once joined, the handle is spent. Weak mode
 * verifies only the first join succeeds — the second is *not even
 * attempted*, because glibc frees the thread descriptor at first join
 * and rejoining is memory-unsafe UB (sth like join(0)). Strict mode
 * demands the second call return an explicit error instead.
 */
int
test_join_dead(void)
{
	mythread_t t;

	ASSERT_SUCCESS(mythread_create(&t, NULL, worker_noop, NULL));
	ASSERT_SUCCESS(mythread_join(t, NULL));

#ifdef MYTHREAD_STRICT_ERRORS
	/* [J12] the stale handle must be rejected, not re-served */
	ASSERT_FAILURE(mythread_join(t, NULL));
#endif
	return TEST_SUCCESS;
}

/*
 * [J9] Concurrent joins on the same target. POSIX declares it UB, and
 * on glibc it really is (losers may "succeed" without a retval, TSan
 * aborts), so the test runs only in strict mode. There the library
 * must serialize duplicate joins: at least one joiner gets the correct
 * value, every joiner that reports success gets the *same correct*
 * value, and every joiner returns — a loser left waiting forever hangs
 * the final joins and trips the runner timeout.
 */
#ifdef MYTHREAD_STRICT_ERRORS
#define JOINERS 4
struct jslot {
	int done;
	int rc;
	intptr_t val;
};

struct jrace {
	mythread_t target;
	int next;               /* slot handout for starting joiners */
	struct jslot slot[JOINERS];
};

static void *
worker_race_target(void *arg)
{
	(void)arg;
	/* linger briefly so the joiners pile up while it is alive */
	for (volatile int i = 0; i < 500000; ++i)
		;
	return (void *)(intptr_t)0x1234;
}

static void *
worker_joiner(void *arg)
{
	struct jrace *r = (struct jrace *)arg;
	int i = __atomic_fetch_add(&r->next, 1, __ATOMIC_SEQ_CST);

	void *ret = NULL;
	r->slot[i].rc = mythread_join(r->target, &ret);
	r->slot[i].val = (intptr_t)ret;
	__atomic_store_n(&r->slot[i].done, 1, __ATOMIC_SEQ_CST);
	return NULL;
}
#endif

int
test_join_concurrent(void)
{
#ifdef MYTHREAD_STRICT_ERRORS
	static struct jrace r;
	r.target = 0;
	r.next = 0;
	for (int i = 0; i < JOINERS; ++i) {
		r.slot[i].done = 0;
		r.slot[i].rc = -1;
		r.slot[i].val = 0;
	}

	ASSERT_SUCCESS(mythread_create(&r.target, NULL, worker_race_target,
	                               NULL));

	/* joiner handles go to scratch storage — r.target must stay the
	 * join race target, not be clobbered by joiner handles */
	mythread_t joiners[JOINERS];
	for (int i = 0; i < JOINERS; ++i)
		ASSERT_SUCCESS(mythread_create(&joiners[i], NULL,
		                               worker_joiner, &r));

	/* wait up to 3s for the first successful joiner */
	int successes = 0;
	for (int ms = 0; ms < 3000; ms += 5) {
		successes = 0;
		for (int i = 0; i < JOINERS; ++i)
			if (__atomic_load_n(&r.slot[i].done,
			                    __ATOMIC_SEQ_CST) &&
			    r.slot[i].rc == 0)
				++successes;
		if (successes > 0)
			break;
		usleep(5 * 1000);
	}

	ASSERT_TRUE(successes >= 1);   /* someone must have got the value */

	/* every joiner must come back, with a value or with an error */
	for (int i = 0; i < JOINERS; ++i)
		ASSERT_SUCCESS(mythread_join(joiners[i], NULL));

	for (int i = 0; i < JOINERS; ++i)
		if (r.slot[i].rc == 0)
			ASSERT_EQ(r.slot[i].val, (intptr_t)0x1234);
#undef JOINERS
#endif
	return TEST_SUCCESS;
}

/*
 * [J13] Join performed *from* a thread, not from main: B joins C and
 * relays C's value to the main thread. Both joins must work from
 * inside a normal worker.
 */
struct relay {
	mythread_t c;
};

static void *
worker_b_joins_c(void *arg)
{
	struct relay *r = (struct relay *)arg;

	void *ret = NULL;
	if (mythread_create(&r->c, NULL, worker_echo,
	                    (void *)(intptr_t)0xC3) != 0)
		return (void *)(intptr_t)-1;

	if (mythread_join(r->c, &ret) != 0)
		return (void *)(intptr_t)-2;

	return ret;   /* 0xC3 from C */
}

int
test_join_from_thread(void)
{
	struct relay rl = { .c = 0 };
	mythread_t b;

	ASSERT_SUCCESS(mythread_create(&b, NULL, worker_b_joins_c, &rl));

	void *ret = NULL;
	ASSERT_SUCCESS(mythread_join(b, &ret));
	ASSERT_EQ((intptr_t)ret, (intptr_t)0xC3);
	ASSERT_TRUE(rl.c != 0);
	return TEST_SUCCESS;
}
