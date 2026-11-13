#include "mythread.h"
#include "assert.h"
#include <stdint.h>

static void *worker_return_arg(void *arg)
{
	return arg;
}
static void *worker_detached(void *arg)
{
	int *counter = arg;
	__atomic_fetch_add(counter, 1, __ATOMIC_SEQ_CST);
	return NULL;
}

int test_u17_detach_queue(void)
{
#define N 48
	mythread_t detached[N];
	int counter = 0;
	for (int i = 0; i < N; ++i) {
		ASSERT_SUCCESS(mythread_create(&detached[i], NULL,
		                               worker_detached, &counter));
		ASSERT_SUCCESS(mythread_detach(detached[i]));
	}

	/* The probe is queued after all detached jobs, so joining it also
	 * proves that the scheduler drained the detached work. */
	mythread_t probe;
	ASSERT_SUCCESS(mythread_create(&probe, NULL, worker_detached, &counter));
	ASSERT_SUCCESS(mythread_join(probe, NULL));
	ASSERT_EQ(counter, N + 1);
#undef N
	return TEST_SUCCESS;
}

struct detach_job {
	int *counter;
};

static void *worker_detach_child(void *arg)
{
	struct detach_job *job = arg;
	__atomic_fetch_add(job->counter, 1, __ATOMIC_SEQ_CST);
	return NULL;
}

static void *worker_detach_from_user(void *arg)
{
	struct detach_job *job = arg;
	mythread_t child;
	if (mythread_create(&child, NULL, worker_detach_child, job) != 0)
		return (void *)(intptr_t)-1;
	if (mythread_detach(child) != 0)
		return (void *)(intptr_t)-2;
	return NULL;
}

int test_u17_detach_from_user(void)
{
	int counter = 0;
	struct detach_job job = { .counter = &counter };
	mythread_t parent;
	ASSERT_SUCCESS(mythread_create(&parent, NULL, worker_detach_from_user, &job));
	ASSERT_SUCCESS(mythread_join(parent, NULL));
	mythread_t probe;
	ASSERT_SUCCESS(mythread_create(&probe, NULL, worker_detach_child, &job));
	ASSERT_SUCCESS(mythread_join(probe, NULL));
	ASSERT_EQ(counter, 2);
	return TEST_SUCCESS;
}

int test_u17_detach_double(void)
{
	mythread_t thread;
	ASSERT_SUCCESS(mythread_create(&thread, NULL, worker_return_arg, NULL));
	ASSERT_SUCCESS(mythread_detach(thread));
#ifdef MYTHREAD_STRICT_ERRORS
	ASSERT_FAILURE(mythread_detach(thread));
#endif
	mythread_t probe;
	ASSERT_SUCCESS(mythread_create(&probe, NULL, worker_return_arg, NULL));
	ASSERT_SUCCESS(mythread_join(probe, NULL));
	return TEST_SUCCESS;
}
