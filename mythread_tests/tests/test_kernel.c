#include "mythread.h"
#include "assert.h"
#include <errno.h>
#include <stdint.h>

/* Kernel-thread semantics (K1..): what a clone()-based 1.6 library must
 * get right beyond create/join/detach bookkeeping.
 *
 * Every test keeps all its workers alive at the same time behind a
 * gate, so state that is shared by mistake (one errno, one TLS block,
 * one stack) is overwritten by a sibling before it is checked. Workers
 * report through their return value (NULL = fine), so the verdict does
 * not depend on assertions running inside a worker. */

/* Shared helpers ------------------------------------------------------ */

#define K_N 8

/* All workers check in, then wait until main opens the gate. */
struct k_gate {
	int arrived;
	int open;
};

static void
k_arrive_and_wait(struct k_gate *g)
{
	__atomic_fetch_add(&g->arrived, 1, __ATOMIC_SEQ_CST);
	while (!__atomic_load_n(&g->open, __ATOMIC_SEQ_CST))
		;
}

static void
k_wait_all_arrived(struct k_gate *g, int n)
{
	while (__atomic_load_n(&g->arrived, __ATOMIC_SEQ_CST) < n)
		;
}

/* Tests --------------------------------------------------------------- */

/*
 * [K1] errno is per thread. glibc's errno lives in thread-local storage,
 * so a clone() without CLONE_SETTLS (or with one TLS block reused for
 * every thread) makes all threads share main's errno: whichever writes
 * last wins and every other thread reads a foreign error code. Each
 * worker stores its own value, everyone waits until all have written,
 * then each checks its value survived. Main writes its own value last,
 * so a shared errno is clobbered for every worker.
 */
struct k1_job {
	int value;
	struct k_gate *g;
};

static void *
worker_k1_errno(void *arg)
{
	struct k1_job *j = (struct k1_job *)arg;

	errno = j->value;
	k_arrive_and_wait(j->g);
	return errno == j->value ? NULL : (void *)(intptr_t)-1;
}

int
test_kernel_errno_per_thread(void)
{
	static struct k1_job jobs[K_N];
	static struct k_gate g;
	static mythread_t t[K_N];

	g.arrived = 0;
	g.open = 0;
	for (int i = 0; i < K_N; ++i) {
		jobs[i].value = 1000 + i;
		jobs[i].g = &g;
		ASSERT_SUCCESS(mythread_create(&t[i], NULL, worker_k1_errno,
		                               &jobs[i]));
	}
	k_wait_all_arrived(&g, K_N);

	/* main writes last: with a shared errno this clobbers every worker */
	errno = 999;
	__atomic_store_n(&g.open, 1, __ATOMIC_SEQ_CST);

	for (int i = 0; i < K_N; ++i) {
		void *ret = (void *)(intptr_t)-2;
		ASSERT_SUCCESS(mythread_join(t[i], &ret));
		ASSERT_NULL(ret);   /* the worker saw its own errno */
	}
	/* main's errno is not checked afterwards: create and join may set
	 * it even on success (C allows that, futex waits often do). */
	return TEST_SUCCESS;
}
