#include "mythread.h"
#include "assert.h"
#include <errno.h>
#include <stdint.h>

static void *worker_increment(void *arg)
{
	(void)arg;
	return NULL;
}
struct nested_job {
	mythread_t child;
	int child_value;
};

static void *worker_child(void *arg)
{
	struct nested_job *job = arg;
	return (void *)(intptr_t)job->child_value;
}

static void *worker_parent(void *arg)
{
	struct nested_job *job = arg;
	if (mythread_create(&job->child, NULL, worker_child, job) != 0)
		return (void *)(intptr_t)-1;
	void *result = NULL;
	if (mythread_join(job->child, &result) != 0)
		return (void *)(intptr_t)-2;
	return (void *)((intptr_t)result + 1);
}

int test_u17_nested_join(void)
{
	struct nested_job job = { .child = 0, .child_value = 41 };
	mythread_t parent;
	ASSERT_SUCCESS(mythread_create(&parent, NULL, worker_parent, &job));

	void *result = NULL;
	ASSERT_SUCCESS(mythread_join(parent, &result));
	ASSERT_EQ((intptr_t)result, 42);
	ASSERT_TRUE(job.child != 0);
	return TEST_SUCCESS;
}
static mythread_t self_handle_test;
static int self_rc_test;
static int self_done_test;

static void *worker_self_join(void *unused)
{
	(void)unused;
	while (!__atomic_load_n(&self_handle_test, __ATOMIC_SEQ_CST))
		;
	self_rc_test = mythread_join(self_handle_test, NULL);
	__atomic_store_n(&self_done_test, 1, __ATOMIC_SEQ_CST);
	return NULL;
}

int test_u17_handles(void)
{
#ifdef MYTHREAD_STRICT_ERRORS
	ASSERT_FAILURE(mythread_create(NULL, NULL, worker_increment, NULL));
	mythread_t invalid;
	ASSERT_FAILURE(mythread_create(&invalid, NULL, NULL, NULL));
#endif

	mythread_t thread;
	ASSERT_SUCCESS(mythread_create(&thread, NULL, worker_increment, NULL));
	ASSERT_SUCCESS(mythread_join(thread, NULL));
#ifdef MYTHREAD_STRICT_ERRORS
	ASSERT_FAILURE(mythread_join(thread, NULL));
	ASSERT_FAILURE(mythread_detach(thread));
	ASSERT_FAILURE(mythread_join((mythread_t)0, NULL));
	ASSERT_FAILURE(mythread_detach((mythread_t)0));
#endif

	/* The self-join error is exercised inside the user context. */
	self_handle_test = 0;
	self_rc_test = 0;
	self_done_test = 0;
	ASSERT_SUCCESS(mythread_create(&self_handle_test, NULL,
	                               worker_self_join, NULL));
	while (!__atomic_load_n(&self_done_test, __ATOMIC_SEQ_CST))
		;
	ASSERT_SUCCESS(mythread_join(self_handle_test, NULL));
	ASSERT_EQ(self_rc_test, EDEADLK);
	return TEST_SUCCESS;
}
