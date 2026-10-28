#include "mythread.h"
#include "assert.h"
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/syscall.h>

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

/*
 * [K2] Thread-local variables are per thread. Same mechanism as K1, but
 * on a _Thread_local of our own: every thread must start from the
 * variable's initial value (its own copy of the TLS image, not a
 * sibling's leftovers), keep what it wrote, and see it at an address
 * of its own. Main checks the addresses are pairwise distinct.
 */
static _Thread_local int k2_tls = 7;

struct k2_job {
	int value;
	uintptr_t addr;     /* &k2_tls as seen by the worker */
	struct k_gate *g;
};

static void *
worker_k2_tls(void *arg)
{
	struct k2_job *j = (struct k2_job *)arg;

	int fresh = k2_tls == 7;
	k2_tls = j->value;
	__atomic_store_n(&j->addr, (uintptr_t)&k2_tls, __ATOMIC_SEQ_CST);
	k_arrive_and_wait(j->g);

	if (!fresh)
		return (void *)(intptr_t)-1;   /* inherited a used TLS block */
	return k2_tls == j->value ? NULL : (void *)(intptr_t)-2;
}

int
test_kernel_tls_per_thread(void)
{
	static struct k2_job jobs[K_N];
	static struct k_gate g;
	static mythread_t t[K_N];

	g.arrived = 0;
	g.open = 0;
	k2_tls = -1;   /* main's copy is used: a shared block starts dirty */
	for (int i = 0; i < K_N; ++i) {
		jobs[i].value = 2000 + i;
		jobs[i].addr = 0;
		jobs[i].g = &g;
		ASSERT_SUCCESS(mythread_create(&t[i], NULL, worker_k2_tls,
		                               &jobs[i]));
	}
	k_wait_all_arrived(&g, K_N);

	uintptr_t mine = (uintptr_t)&k2_tls;
	for (int i = 0; i < K_N; ++i) {
		ASSERT_TRUE(jobs[i].addr != mine);
		for (int k = i + 1; k < K_N; ++k)
			ASSERT_TRUE(jobs[i].addr != jobs[k].addr);
	}

	__atomic_store_n(&g.open, 1, __ATOMIC_SEQ_CST);
	for (int i = 0; i < K_N; ++i) {
		void *ret = (void *)(intptr_t)-3;
		ASSERT_SUCCESS(mythread_join(t[i], &ret));
		ASSERT_NULL(ret);   /* fresh TLS copy, value kept */
	}
	ASSERT_EQ(k2_tls, -1);   /* nobody wrote into main's copy */
	return TEST_SUCCESS;
}

/*
 * [K3] Threads, not processes: CLONE_THREAD and CLONE_VM. While all
 * workers are alive at once, each one must report main's pid (same
 * thread group) and a tid of its own (distinct from each other and
 * from main — tids are only reusable after a thread is gone, hence the
 * gate). Each also writes a global that main must then see: with a
 * fork()-like copy of the address space the write stays invisible.
 */
static int k3_seen[K_N];

struct k3_job {
	int idx;
	pid_t pid, tid;
	struct k_gate *g;
};

static void *
worker_k3_ids(void *arg)
{
	struct k3_job *j = (struct k3_job *)arg;

	j->pid = getpid();
	j->tid = (pid_t)syscall(SYS_gettid);
	__atomic_store_n(&k3_seen[j->idx], 0x300 + j->idx, __ATOMIC_SEQ_CST);
	k_arrive_and_wait(j->g);
	return NULL;
}

int
test_kernel_ids(void)
{
	static struct k3_job jobs[K_N];
	static struct k_gate g;
	static mythread_t t[K_N];

	g.arrived = 0;
	g.open = 0;
	for (int i = 0; i < K_N; ++i) {
		k3_seen[i] = 0;
		jobs[i].idx = i;
		jobs[i].pid = jobs[i].tid = 0;
		jobs[i].g = &g;
		ASSERT_SUCCESS(mythread_create(&t[i], NULL, worker_k3_ids,
		                               &jobs[i]));
	}
	k_wait_all_arrived(&g, K_N);

	pid_t pid = getpid();
	pid_t tid = (pid_t)syscall(SYS_gettid);
	for (int i = 0; i < K_N; ++i) {
		/* the write made in the worker is visible here (CLONE_VM) */
		ASSERT_EQ(__atomic_load_n(&k3_seen[i], __ATOMIC_SEQ_CST),
		          0x300 + i);
		ASSERT_EQ(jobs[i].pid, pid);   /* same process (CLONE_THREAD) */
		ASSERT_TRUE(jobs[i].tid > 0);
		ASSERT_TRUE(jobs[i].tid != tid);
		for (int k = i + 1; k < K_N; ++k)
			ASSERT_TRUE(jobs[i].tid != jobs[k].tid);
	}

	__atomic_store_n(&g.open, 1, __ATOMIC_SEQ_CST);
	for (int i = 0; i < K_N; ++i)
		ASSERT_SUCCESS(mythread_join(t[i], NULL));
	return TEST_SUCCESS;
}
