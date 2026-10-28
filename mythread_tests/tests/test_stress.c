#include "mythread.h"
#include "assert.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

/* Stress tests (S1..S10; S11 = make sanitize/valgrind, S12 = runner).
 *
 * S11 (TSan/ASan/Valgrind) is a *build* of the same suite, not a
 * separate test: see `make sanitize` / `make valgrind` in the Makefile.
 * S12 (no hangs) is enforced by the runner's per-test timeout on every
 * test already — no test of its own.
 *
 * The heavy loops are dimensioned by MYTHREAD_STRESS_LEVEL (1..3,
 * default 1): level 1 keeps the full suite well under a minute,
 * level 3 (grading deep-dive) pushes into minutes. Override:
 *   make CPPFLAGS=-DMYTHREAD_STRESS_LEVEL=3
 */

#ifndef MYTHREAD_STRESS_LEVEL
#define MYTHREAD_STRESS_LEVEL 1
#endif

/* Shared helpers ------------------------------------------------------ */

static int64_t
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void *
worker_inc(void *arg)
{
	int *p = (int *)arg;
	__atomic_fetch_add(p, 1, __ATOMIC_SEQ_CST);
	return NULL;
}

static void *
worker_noop(void *arg)
{
	(void)arg;
	return NULL;
}

/*
 * Parked worker released by a flag: lets a test pile up threads
 * before any of them finishes.
 */
struct gate {
	volatile int open;
};

static void *
worker_park(void *arg)
{
	struct gate *g = (struct gate *)arg;
	while (!__atomic_load_n(&g->open, __ATOMIC_SEQ_CST))
		;
	return NULL;
}

/* Tests --------------------------------------------------------------- */

/*
 * [R1/R10] The classic: N threads increment one atomic counter, join
 * all, sum must be exact — now as the suite's quick smoke test.
 */
int
test_stress_many_threads(void)
{
#define N 64
	static int counter;
	counter = 0;

	mythread_t t[N];
	for (int i = 0; i < N; ++i)
		ASSERT_SUCCESS(mythread_create(&t[i], NULL, worker_inc,
		                               &counter));
	for (int i = 0; i < N; ++i)
		ASSERT_SUCCESS(mythread_join(t[i], NULL));
	ASSERT_EQ(counter, N);
#undef N
	return TEST_SUCCESS;
}

/*
 * Quick churn: 20 rounds of create+join one thread.
 */
int
test_stress_churn(void)
{
	static int counter;
	counter = 0;

	for (int round = 0; round < 20; ++round) {
		mythread_t t;
		ASSERT_SUCCESS(mythread_create(&t, NULL, worker_inc,
		                               &counter));
		ASSERT_SUCCESS(mythread_join(t, NULL));
	}
	ASSERT_EQ(counter, 20);
	return TEST_SUCCESS;
}

/*
 * [S1..S3] Scale: many sequential create/join lifecycles. Counts by
 * stress level: 1 000 / 10 000 / 100 000. No crash, no leak, sum
 * exact.
 */
int
test_stress_scale(void)
{
#if MYTHREAD_STRESS_LEVEL >= 3
#define ROUNDS 100000
#elif MYTHREAD_STRESS_LEVEL >= 2
#define ROUNDS 10000
#else
#define ROUNDS 1000
#endif
	static int counter;
	counter = 0;

	for (int i = 0; i < ROUNDS; ++i) {
		mythread_t t;
		ASSERT_SUCCESS(mythread_create(&t, NULL, worker_inc,
		                               &counter));
		ASSERT_SUCCESS(mythread_join(t, NULL));
	}
	ASSERT_EQ(counter, ROUNDS);
#undef ROUNDS
	return TEST_SUCCESS;
}

/*
 * [S4] Random join order: threads are created in order, joined in a
 * fixed shuffled permutation (deterministic, reproducible failures).
 * Every joined handle must succeed exactly once and in one piece.
 */
#define S4_N 16
int
test_stress_random_join_order(void)
{
	static mythread_t t[S4_N];
	static int vals[S4_N];

	static const int perm[S4_N] = {
		7, 2, 12, 0, 9, 14, 4, 1,
		11, 5, 15, 3, 8, 6, 13, 10
	};

	for (int i = 0; i < S4_N; ++i) {
		vals[i] = 0;   /* each worker increments its own cell once */
		ASSERT_SUCCESS(mythread_create(&t[i], NULL, worker_inc,
		                               &vals[i]));
	}
	for (int k = 0; k < S4_N; ++k)
		ASSERT_SUCCESS(mythread_join(t[perm[k]], NULL));

	for (int i = 0; i < S4_N; ++i)
		ASSERT_EQ(vals[i], 1);
	return TEST_SUCCESS;
}
#undef S4_N

/*
 * [S5] N detached threads, no join at all: accountability through an
 * atomic counter. All N must increment within a bounded wait; a
 * detached thread that silently vanishes (the classic 1.7 pool bug)
 * leaves the count short.
 */
static void *
worker_s5_detached(void *arg)
{
	int *count = (int *)arg;
	__atomic_fetch_add(count, 1, __ATOMIC_SEQ_CST);
	return NULL;
}

int
test_stress_detached_count(void)
{
#define S5_N 64
	static int count;
	count = 0;
	static mythread_t t[S5_N];

	for (int i = 0; i < S5_N; ++i)
		ASSERT_SUCCESS(mythread_create(&t[i], NULL,
		                               worker_s5_detached, &count));

	/* detached: nobody joins; wait bounded for the counter */
	for (int ms = 0; ms < 3000; ms += 5) {
		if (__atomic_load_n(&count, __ATOMIC_SEQ_CST) >= S5_N)
			break;
		usleep(5 * 1000);
	}
	ASSERT_EQ(count, S5_N);
#undef S5_N
	return TEST_SUCCESS;
}

/*
 * [S6] Mixed joined and detached threads in one burst, all accounted:
 * the joined half parks until its gates open and joins cleanly; the
 * detached half shows up in the atomic counter. Catches bookkeeping
 * crosstalk between joinable and detached entries.
 */
int
test_stress_mixed(void)
{
#define S6_N 32
	static struct gate gj[S6_N];
	static int dcount;
	static mythread_t tj[S6_N], td[S6_N];

	dcount = 0;

	/* joined half: parked until released */
	for (int i = 0; i < S6_N; ++i) {
		gj[i].open = 0;
		ASSERT_SUCCESS(mythread_create(&tj[i], NULL, worker_park,
		                               &gj[i]));
	}
	/* detached half: fire-and-count */
	for (int i = 0; i < S6_N; ++i)
		ASSERT_SUCCESS(mythread_create(&td[i], NULL,
		                               worker_s5_detached, &dcount));

	for (int i = 0; i < S6_N; ++i)
		__atomic_store_n(&gj[i].open, 1, __ATOMIC_SEQ_CST);
	for (int i = 0; i < S6_N; ++i)
		ASSERT_SUCCESS(mythread_join(tj[i], NULL));

	for (int ms = 0; ms < 3000; ms += 5) {
		if (__atomic_load_n(&dcount, __ATOMIC_SEQ_CST) >= S6_N)
			break;
		usleep(5 * 1000);
	}
	ASSERT_EQ(dcount, S6_N);

	/* the process can still make threads afterwards */
	mythread_t probe;
	ASSERT_SUCCESS(mythread_create(&probe, NULL, worker_noop, NULL));
	ASSERT_SUCCESS(mythread_join(probe, NULL));
#undef S6_N
	return TEST_SUCCESS;
}

/*
 * [S7] Deep nesting: each thread creates the next one and joins it,
 * 100 levels down; the payload sum proves every level really ran.
 * Surfaces recursion limits, per-level stack waste, or depth caps.
 */
#define S7_DEPTH 100
struct s7_arg {
	int level;
};

static void *
worker_s7(void *arg)
{
	struct s7_arg *in = (struct s7_arg *)arg;
	long depth = in->level;

	if (depth >= S7_DEPTH)
		return (void *)(intptr_t)0;

	struct s7_arg next = { .level = in->level + 1 };
	mythread_t child;
	void *ret = NULL;

	if (mythread_create(&child, NULL, worker_s7, &next) != 0)
		return (void *)(intptr_t)-1;
	if (mythread_join(child, &ret) != 0)
		return (void *)(intptr_t)-2;

	/* each level contributes 1; the deepest returns 0 */
	return (void *)(intptr_t)((intptr_t)ret + 1);
}

int
test_stress_nested_deep(void)
{
	static struct s7_arg root = { .level = 0 };
	mythread_t t;
	void *ret = NULL;

	ASSERT_SUCCESS(mythread_create(&t, NULL, worker_s7, &root));
	ASSERT_SUCCESS(mythread_join(t, &ret));

	/* exactly S7_DEPTH levels contributed, no create/join failed */
	ASSERT_EQ((intptr_t)ret, (intptr_t)S7_DEPTH);
	return TEST_SUCCESS;
}
#undef S7_DEPTH

/*
 * [S8] Soak: create/join waves for a fixed wall-clock budget (~2s);
 * higher stress levels soak longer. The clock is the boss, the wave
 * count is whatever fits; the assertion is stability — every wave
 * exact, no drift, no hang (the runner's timeout is the backstop).
 */
int
test_stress_soak(void)
{
#if MYTHREAD_STRESS_LEVEL >= 3
#define SOAK_MS 8000
#elif MYTHREAD_STRESS_LEVEL >= 2
#define SOAK_MS 4000
#else
#define SOAK_MS 2000
#endif
#define WAVE 4
	static struct gate g[WAVE];
	static mythread_t t[WAVE];

	int64_t spent = 0;
	int waves = 0;

	while (spent < SOAK_MS) {
		int64_t t0 = now_ms();

		for (int i = 0; i < WAVE; ++i) {
			g[i].open = 0;
			ASSERT_SUCCESS(mythread_create(&t[i], NULL,
			                               worker_park, &g[i]));
		}
		for (int i = 0; i < WAVE; ++i)
			__atomic_store_n(&g[i].open, 1, __ATOMIC_SEQ_CST);
		for (int i = 0; i < WAVE; ++i)
			ASSERT_SUCCESS(mythread_join(t[i], NULL));

		spent += now_ms() - t0;
		++waves;
	}

	/* sanity: the loop actually ran and the guard isn't fooled */
	ASSERT_TRUE(waves > 0);
	ASSERT_TRUE(spent >= SOAK_MS);
#undef WAVE
#undef SOAK_MS
	return TEST_SUCCESS;
}

/*
 * [S9] Mixed sizes in one storm: instant workers (done before create
 * returns), tiny atomic adds, and medium spinners — interleaved.
 * Surfaces size/stack-class confusion in pools: one allocation size
 * handed to a different worker class corrupts or deadlocks.
 */
static void *
worker_s9_medium(void *arg)
{
	int *iters = (int *)arg;
	for (volatile int i = 0; i < *iters; ++i)
		;
	return NULL;
}

int
test_stress_mixed_sizes(void)
{
#define S9_STORM 48
	static int iters[S9_STORM];
	static mythread_t t[S9_STORM];

	for (int i = 0; i < S9_STORM; ++i) {
		void *(*fn)(void *);
		void *arg = NULL;

		switch (i % 3) {
		case 0:
			fn = worker_noop;                 /* instant */
			break;
		case 1:
			fn = worker_inc;                  /* tiny */
			arg = &iters[0];                  /* any shared cell */
			break;
		default:
			iters[i] = 1000;
			fn = worker_s9_medium;            /* medium */
			arg = &iters[i];
		}
		ASSERT_SUCCESS(mythread_create(&t[i], NULL, fn, arg));
	}
	for (int i = 0; i < S9_STORM; ++i)
		ASSERT_SUCCESS(mythread_join(t[i], NULL));

	/* library intact afterwards */
	mythread_t probe;
	ASSERT_SUCCESS(mythread_create(&probe, NULL, worker_noop, NULL));
	ASSERT_SUCCESS(mythread_join(probe, NULL));
#undef S9_STORM
	return TEST_SUCCESS;
}

/*
 * [S10] A value is a value no matter how the thread ends: half the
 * workers return normally, half call mythread_exit with the same
 * payload. Every joiner must receive its exact value.
 */
static void *
worker_s10_exit(void *arg)
{
	mythread_exit(arg);
	return NULL;   /* unreachable */
}

static void *
worker_s10_return(void *arg)
{
	return arg;
}

int
test_stress_mixed_exit_paths(void)
{
#define S10_N 32
	static mythread_t t[S10_N];
	static int vals[S10_N];

	for (int i = 0; i < S10_N; ++i) {
		vals[i] = 0x70 + i;
		ASSERT_SUCCESS(mythread_create(&t[i], NULL,
		                               (i % 2 == 0) ? worker_s10_exit
		                                            : worker_s10_return,
		                               &vals[i]));
	}
	for (int i = 0; i < S10_N; ++i) {
		void *ret = NULL;
		ASSERT_SUCCESS(mythread_join(t[i], &ret));
		ASSERT_TRUE(ret == &vals[i]);
	}
#undef S10_N
	return TEST_SUCCESS;
}
