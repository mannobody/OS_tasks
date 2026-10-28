#include "mythread.h"
#include "assert.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

/* Kernel-thread semantics (K1..): what a clone()-based 1.6 library must
 * get right beyond create/join/detach bookkeeping.
 *
 * Tests with many workers keep them all alive at the same time behind a
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

/*
 * [K4] The fd table is shared (CLONE_FILES). Main opens a pipe; the
 * worker opens a pipe of its own, hands the numbers back and closes
 * the read end of main's pipe. Main must see both effects: its read end is gone,
 * and the worker's pipe works from main. With a private copy of the
 * table the close stays local and the new numbers are unknown in main.
 */
struct k4_job {
	int close_fd;      /* main's fd the worker closes */
	int fds[2];        /* pipe opened by the worker */
};

static void *
worker_k4_fds(void *arg)
{
	struct k4_job *j = (struct k4_job *)arg;

	/* pipe first: after the close, pipe() would reuse close_fd's number */
	if (pipe(j->fds) != 0)
		return (void *)(intptr_t)-1;
	if (close(j->close_fd) != 0)
		return (void *)(intptr_t)-2;
	return NULL;
}

int
test_kernel_fd_shared(void)
{
	int mine[2];
	struct k4_job j = { .fds = { -1, -1 } };
	mythread_t t;

	ASSERT_SUCCESS(pipe(mine));
	j.close_fd = mine[0];
	ASSERT_SUCCESS(mythread_create(&t, NULL, worker_k4_fds, &j));

	void *ret = (void *)(intptr_t)-3;
	ASSERT_SUCCESS(mythread_join(t, &ret));
	ASSERT_NULL(ret);

	/* the close made in the worker is visible here */
	ASSERT_TRUE(fcntl(mine[0], F_GETFD) == -1);
	close(mine[1]);

	/* the worker's pipe is usable from main */
	char c = 'k';
	ASSERT_EQ(write(j.fds[1], &c, 1), 1);
	c = 0;
	ASSERT_EQ(read(j.fds[0], &c, 1), 1);
	ASSERT_EQ(c, 'k');
	close(j.fds[0]);
	close(j.fds[1]);
	return TEST_SUCCESS;
}

/*
 * [K5] Every thread starts on a properly aligned stack: the x86-64 and
 * AArch64 ABIs require 16 bytes at a call, and the compiler places
 * aligned locals relative to the stack pointer trusting that. A thread
 * entered with a misaligned stack (e.g. a hand-written trampoline that
 * pushes one word too many) gets a _Alignas(16) local at an address
 * that is off by 8, and libc code using aligned SSE stores — printf's
 * floating-point path among them — dies with SIGSEGV. Each worker of
 * K_N (different stacks) checks both.
 */
static void *
worker_k5_align(void *arg)
{
	(void)arg;
	_Alignas(16) char probe[16];

	/* through a volatile so the compiler cannot fold the check away */
	volatile uintptr_t addr = (uintptr_t)probe;
	if (addr % 16 != 0)
		return (void *)(intptr_t)-1;

	char buf[32];
	snprintf(buf, sizeof buf, "%.2f", 3.25);
	return strcmp(buf, "3.25") == 0 ? NULL : (void *)(intptr_t)-2;
}

int
test_kernel_stack_align(void)
{
	mythread_t t[K_N];

	for (int i = 0; i < K_N; ++i)
		ASSERT_SUCCESS(mythread_create(&t[i], NULL, worker_k5_align,
		                               NULL));
	for (int i = 0; i < K_N; ++i) {
		void *ret = (void *)(intptr_t)-3;
		ASSERT_SUCCESS(mythread_join(t[i], &ret));
		ASSERT_NULL(ret);   /* aligned stack, printf("%f") works */
	}
	return TEST_SUCCESS;
}

/*
 * [K6] A thread's stack is big enough for ordinary deep frames. Each of
 * K_N workers puts a MYTHREAD_DEEP_STACK_KIB array on its stack (128 KiB
 * by default: far below the 8 MiB pthread default, above toy 64 KiB
 * stacks), fills it with its own byte, waits until all are full, then
 * checks it. A stack that is too small either runs into a guard page
 * (SIGSEGV) or, without one, into a neighbour's stack or the heap, and
 * the overwritten bytes show. A library that documents a different
 * stack size can be graded with
 *   make CPPFLAGS=-DMYTHREAD_DEEP_STACK_KIB=<n>
 */
#ifndef MYTHREAD_DEEP_STACK_KIB
#define MYTHREAD_DEEP_STACK_KIB 128
#endif
#define K6_DEPTH (MYTHREAD_DEEP_STACK_KIB * 1024)

struct k6_job {
	uint8_t fill;
	struct k_gate *g;
};

static void *
worker_k6_deep(void *arg)
{
	struct k6_job *j = (struct k6_job *)arg;
	volatile uint8_t big[K6_DEPTH];   /* volatile: really on the stack */

	for (int i = 0; i < K6_DEPTH; ++i)
		big[i] = j->fill;
	k_arrive_and_wait(j->g);
	for (int i = 0; i < K6_DEPTH; ++i)
		if (big[i] != j->fill)
			return (void *)(intptr_t)-1;
	return NULL;
}

int
test_kernel_deep_stack(void)
{
	static struct k6_job jobs[K_N];
	static struct k_gate g;
	static mythread_t t[K_N];

	g.arrived = 0;
	g.open = 0;
	for (int i = 0; i < K_N; ++i) {
		jobs[i].fill = (uint8_t)(0xA0 + i);
		jobs[i].g = &g;
		ASSERT_SUCCESS(mythread_create(&t[i], NULL, worker_k6_deep,
		                               &jobs[i]));
	}
	k_wait_all_arrived(&g, K_N);
	__atomic_store_n(&g.open, 1, __ATOMIC_SEQ_CST);

	for (int i = 0; i < K_N; ++i) {
		void *ret = (void *)(intptr_t)-2;
		ASSERT_SUCCESS(mythread_join(t[i], &ret));
		ASSERT_NULL(ret);   /* the whole array survived */
	}
	return TEST_SUCCESS;
}
