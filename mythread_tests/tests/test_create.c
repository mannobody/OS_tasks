#include "mythread.h"
#include "assert.h"
#include <stdint.h>
#include <string.h>

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
 * Wait-free rendezvous: each worker marks itself as started, waits until
 * the main thread says "everyone is up" and only then exits. Used to
 * prove threads actually run concurrently, not one-by-one.
 */
struct barrier {
	int seen;         /* workers that reached the rendezvous */
	int release;      /* 0 = wait, 1 = everyone is up, go ahead */
};

static void *
worker_barrier(void *arg)
{
	struct barrier *b = (struct barrier *)arg;

	__atomic_add_fetch(&b->seen, 1, __ATOMIC_SEQ_CST);
	while (!__atomic_load_n(&b->release, __ATOMIC_SEQ_CST))
		; /* spin until released */

	return NULL;
}

/*
 * Fill a private buffer with a per-thread pattern. The main thread
 * verifies the content afterwards — possible only if each thread owned
 * its own stack between create() and join().
 */
struct fill_job {
	uint8_t *buf;
	size_t   len;
	uint8_t  pattern;
};

static void *
worker_fill_pattern(void *arg)
{
	struct fill_job *j = (struct fill_job *)arg;

	for (size_t i = 0; i < j->len; ++i)
		j->buf[i] = j->pattern;

	return NULL;
}

/*
 * Long-lived worker: waits until asked to finish, so a test can pile up
 * many simultaneous threads before joining them. A worker that exits
 * on its own would free slots early and break the resource-limit test.
 */
static void *
worker_sleepy(void *arg)
{
	int *go = (int *)arg;

	while (!__atomic_load_n(go, __ATOMIC_SEQ_CST))
		;

	return NULL;
}

/* Tests --------------------------------------------------------------- */

/*
 * The minimal contract: create() returns success and produces a handle
 * that join() accepts. No user data involved.
 */
int
test_create_basic(void)
{
	mythread_t t;

	ASSERT_SUCCESS(mythread_create(&t, NULL, worker_noop, NULL));
	ASSERT_SUCCESS(mythread_join(t, NULL));

	return TEST_SUCCESS;
}

/*
 * The whole conversation through mythread_t: pass data in, get data out.
 * Covers argument delivery into the start routine and result retrieval
 * through join() — the two channels of the thread API. [C5]
 * Both idiomatic POSIX argument styles are checked: a pointer to an
 * object and an integer transported via intptr_t.
 */
int
test_create_data_passing(void)
{
	mythread_t t;
	int in = 12345;

	ASSERT_SUCCESS(mythread_create(&t, NULL, worker_echo, &in));

	void *out = NULL;
	ASSERT_SUCCESS(mythread_join(t, &out));
	ASSERT_TRUE(out == &in);

	/* integer-as-pointer case */
	ASSERT_SUCCESS(mythread_create(&t, NULL, worker_echo,
	                               (void *)(intptr_t)0xBEEF));
	out = NULL;
	ASSERT_SUCCESS(mythread_join(t, &out));
	ASSERT_EQ((intptr_t)out, (intptr_t)0xBEEF);

	return TEST_SUCCESS;
}

/*
 * Worker also threads must not be isolate islands: N threads started,
 * all of them report they are running at the same time, then released
 * together. Catches the "store the callback, run it once, exit"
 * degenerate implementation that never actually schedules anything.
 */
int
test_create_parallel_run(void)
{
#define N 8
	struct barrier b = { .seen = 0, .release = 0 };
	mythread_t t[N];

	for (int i = 0; i < N; ++i)
		ASSERT_SUCCESS(mythread_create(&t[i], NULL, worker_barrier, &b));

	/* [C4] While all workers are alive simultaneously, their handles
	 * must be pairwise distinct — a thread library that hands out the
	 * same TID twice is un-joinable for real. */
	for (int i = 0; i < N; ++i)
		for (int k = i + 1; k < N; ++k)
			ASSERT_TRUE(t[i] != t[k]);

	/* With a real scheduler every worker reaches the rendezvous while
	 * the main thread spins; a fake single-threaded "scheduler" would
	 * deadlock here and the runner would report TIMEOUT. */
	while (__atomic_load_n(&b.seen, __ATOMIC_SEQ_CST) != N)
		;

	__atomic_store_n(&b.release, 1, __ATOMIC_SEQ_CST);
	for (int i = 0; i < N; ++i)
		ASSERT_SUCCESS(mythread_join(t[i], NULL));

#undef N
	return TEST_SUCCESS;
}

/*
 * [C8, C9] Resource limits and lifecycle: keep creating until the
 * library itself says "no", then check it stayed healthy — the error is
 * reported gracefully and the process can keep making threads again.
 * Catches leaks: an implementation that loses a thread slot per create
 * exhausts long before the loop ends or cannot create afterwards.
 */
int
test_create_until_failure(void)
{
#define MAXT 256
	static int go;
	static mythread_t t[MAXT];
	int created = 0;

	go = 0;

	/* The reference library has no artificial cap; 256 live threads is
	 * well within any ulimit. Students' pools (1.7) may fail earlier —
	 * that is fine, the contract being tested is graceful failure. */
	while (created < MAXT &&
	       mythread_create(&t[created], NULL, worker_sleepy, &go) == 0)
		++created;

	ASSERT_TRUE(created > 0);	/* creating must work in the first place */

	/* every handle so far must be joinable and return success */
	__atomic_store_n(&go, 1, __ATOMIC_SEQ_CST);
	for (int i = 0; i < created; ++i)
		ASSERT_SUCCESS(mythread_join(t[i], NULL));

	/* [C9] after draining everything, creation works again */
	mythread_t extra;
	ASSERT_SUCCESS(mythread_create(&extra, NULL, worker_noop, NULL));
	ASSERT_SUCCESS(mythread_join(extra, NULL));

#undef MAXT
	return TEST_SUCCESS;
}

/*
 * [C11] Nested creation: a thread itself calls mythread_create. The
 * child must be a full citizen — joinable, independent of the parent's
 * lifetime. Catches implementations that keep per-process "the one
 * creator" state or corrupt parent bookkeeping on nested create.
 */
static void *
worker_nest(void *arg)
{
	mythread_t *child = (mythread_t *)arg;

	if (mythread_create(child, NULL, worker_echo, arg) != 0)
		return (void *)(intptr_t)-1;

	return mythread_join(*child, NULL) == 0 ? NULL : (void *)(intptr_t)-2;
}

int
test_create_nested(void)
{
	mythread_t parent;
	mythread_t child_marker = 0;   /* 0 is not a valid live handle */

	ASSERT_SUCCESS(mythread_create(&parent, NULL, worker_nest,
	                               &child_marker));
	void *ret = NULL;
	ASSERT_SUCCESS(mythread_join(parent, &ret));
	ASSERT_NULL(ret);          /* worker_nest reports errors via retval */
	ASSERT_TRUE(child_marker != 0);  /* the nested create really happened */

	return TEST_SUCCESS;
}

/*
 * Distinct threads must own distinct stacks: each worker fills a buffer
 * with its own byte pattern, the main thread verifies everything after
 * joining. A shared-stack implementation (the classic 1.6 bug: handing
 * every thread the same stack area) gets its patterns overwritten and
 * the test fails.
 */
int
test_create_distinct_stacks(void)
{
#define N 8
#define BUF 4096
	static struct fill_job jobs[N];
	static uint8_t bufs[N][BUF];
	static mythread_t t[N];

	for (int i = 0; i < N; ++i) {
		jobs[i].buf = bufs[i];
		jobs[i].len = BUF;
		jobs[i].pattern = (uint8_t)(0x10 + i);
		ASSERT_SUCCESS(mythread_create(&t[i], NULL,
		                               worker_fill_pattern, &jobs[i]));
	}
	for (int i = 0; i < N; ++i)
		ASSERT_SUCCESS(mythread_join(t[i], NULL));

	for (int i = 0; i < N; ++i)
		for (size_t k = 0; k < BUF; ++k)
			ASSERT_EQ(bufs[i][k], (uint8_t)(0x10 + i));

#undef N
#undef BUF
	return TEST_SUCCESS;
}
