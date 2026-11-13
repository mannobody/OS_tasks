#include "mythread.h"
#include "assert.h"
#include <stdint.h>
static void *worker_increment(void *arg)
{
	int *counter = arg;
	if (counter)
		__atomic_fetch_add(counter, 1, __ATOMIC_SEQ_CST);
	return arg;
}

int test_u17_create_join_many(void)
{
#define N 64
	mythread_t handles[N];
	int counter = 0;

	for (int i = 0; i < N; ++i)
		ASSERT_SUCCESS(mythread_create(&handles[i], NULL,
		                               worker_increment, &counter));
	for (int i = 0; i < N; ++i) {
		void *result = NULL;
		ASSERT_SUCCESS(mythread_join(handles[i], &result));
		ASSERT_TRUE(result == &counter);
	}
	ASSERT_EQ(counter, N);
#undef N
	return TEST_SUCCESS;
}

static void *worker_return_arg(void *arg)
{
	return arg;
}

int test_u17_join_value_modes(void)
{
	mythread_t null_thread;
	mythread_t value_thread;
	int value = 17;
	ASSERT_SUCCESS(mythread_create(&null_thread, NULL, worker_return_arg, NULL));
	ASSERT_SUCCESS(mythread_create(&value_thread, NULL, worker_return_arg, &value));
	ASSERT_SUCCESS(mythread_join(null_thread, NULL));
	void *result = NULL;
	ASSERT_SUCCESS(mythread_join(value_thread, &result));
	ASSERT_TRUE(result == &value);
	return TEST_SUCCESS;
}

int test_u17_reverse_join(void)
{
#define N 24
	mythread_t handles[N];
	int values[N];
	for (int i = 0; i < N; ++i) {
		values[i] = i + 1;
		ASSERT_SUCCESS(mythread_create(&handles[i], NULL,
		                               worker_return_arg, &values[i]));
	}
	for (int i = N - 1; i >= 0; --i) {
		void *result = NULL;
		ASSERT_SUCCESS(mythread_join(handles[i], &result));
		ASSERT_TRUE(result == &values[i]);
	}
#undef N
	return TEST_SUCCESS;
}

struct child_batch {
	int count;
};

static void *worker_batch_child(void *arg)
{
	int *value = arg;
	return value;
}

static void *worker_create_from_user(void *arg)
{
#define N 8
	struct child_batch *batch = arg;
	mythread_t children[N];
	int values[N];
	for (int i = 0; i < N; ++i) {
		values[i] = i;
		if (mythread_create(&children[i], NULL, worker_batch_child,
		                   &values[i]) != 0)
			return (void *)(intptr_t)-1;
	}
	for (int i = 0; i < N; ++i) {
		void *result = NULL;
		if (mythread_join(children[i], &result) != 0 || result != &values[i])
			return (void *)(intptr_t)-2;
	}
	batch->count = N;
	return (void *)(intptr_t)N;
#undef N
}

int test_u17_create_from_user(void)
{
	struct child_batch batch = { .count = 0 };
	mythread_t parent;
	ASSERT_SUCCESS(mythread_create(&parent, NULL, worker_create_from_user,
	                               &batch));
	void *result = NULL;
	ASSERT_SUCCESS(mythread_join(parent, &result));
	ASSERT_EQ((intptr_t)result, 8);
	ASSERT_EQ(batch.count, 8);
	return TEST_SUCCESS;
}

struct chain_depth {
	int remaining;
};

static void *worker_chain(void *arg)
{
	struct chain_depth *depth = arg;
	if (depth->remaining == 0)
		return (void *)(intptr_t)0;
	struct chain_depth child_depth = { .remaining = depth->remaining - 1 };
	mythread_t child;
	if (mythread_create(&child, NULL, worker_chain, &child_depth) != 0)
		return (void *)(intptr_t)-1;
	void *result = NULL;
	if (mythread_join(child, &result) != 0)
		return (void *)(intptr_t)-2;
	return (void *)((intptr_t)result + 1);
}

int test_u17_nested_chain(void)
{
	struct chain_depth depth = { .remaining = 24 };
	mythread_t root;
	ASSERT_SUCCESS(mythread_create(&root, NULL, worker_chain, &depth));
	void *result = NULL;
	ASSERT_SUCCESS(mythread_join(root, &result));
	ASSERT_EQ((intptr_t)result, 24);
	return TEST_SUCCESS;
}
struct stack_job {
	uintptr_t address;
	unsigned char value;
};

static void *worker_stack(void *arg)
{
	struct stack_job *job = arg;
	unsigned char local[128];
	for (unsigned i = 0; i < sizeof local; ++i)
		local[i] = job->value;
	job->address = (uintptr_t)&local[0];
	return NULL;
}

int test_u17_stack_isolation(void)
{
#define N 16
	struct stack_job jobs[N];
	mythread_t handles[N];
	for (int i = 0; i < N; ++i) {
		jobs[i].address = 0;
		jobs[i].value = (unsigned char)(i + 1);
		ASSERT_SUCCESS(mythread_create(&handles[i], NULL,
		                               worker_stack, &jobs[i]));
	}
	for (int i = 0; i < N; ++i)
		ASSERT_SUCCESS(mythread_join(handles[i], NULL));
	for (int i = 0; i < N; ++i) {
		ASSERT_TRUE(jobs[i].address != 0);
		for (int j = i + 1; j < N; ++j)
			ASSERT_TRUE(jobs[i].address != jobs[j].address);
	}
#undef N
	return TEST_SUCCESS;
}
