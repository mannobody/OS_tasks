#include "mythread.h"
#include "assert.h"
#include <dirent.h>
#include <stdint.h>

static void *worker_increment(void *arg)
{
	int *counter = arg;
	if (counter)
		__atomic_fetch_add(counter, 1, __ATOMIC_SEQ_CST);
	return arg;
}

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
int test_u17_handle_uniqueness(void)
{
#define N 32
	mythread_t handles[N];
	for (int i = 0; i < N; ++i)
		ASSERT_SUCCESS(mythread_create(&handles[i], NULL, worker_return_arg, NULL));
	for (int i = 0; i < N; ++i) {
		ASSERT_TRUE(handles[i] != 0);
		for (int j = i + 1; j < N; ++j)
			ASSERT_TRUE(handles[i] != handles[j]);
	}
	for (int i = 0; i < N; ++i)
		ASSERT_SUCCESS(mythread_join(handles[i], NULL));
#undef N
	return TEST_SUCCESS;
}

int test_u17_reuse_cycles(void)
{
#define JOIN_ROUNDS 256
#define DETACH_ROUNDS 256
	int joined = 0;
	for (int i = 0; i < JOIN_ROUNDS; ++i) {
		mythread_t thread;
		ASSERT_SUCCESS(mythread_create(&thread, NULL, worker_increment, &joined));
		ASSERT_SUCCESS(mythread_join(thread, NULL));
	}
	int detached = 0;
	for (int i = 0; i < DETACH_ROUNDS; ++i) {
		mythread_t thread;
		ASSERT_SUCCESS(mythread_create(&thread, NULL, worker_detached, &detached));
		ASSERT_SUCCESS(mythread_detach(thread));
	}
	mythread_t probe;
	ASSERT_SUCCESS(mythread_create(&probe, NULL, worker_detached, &detached));
	ASSERT_SUCCESS(mythread_join(probe, NULL));
	ASSERT_EQ(joined, JOIN_ROUNDS);
	ASSERT_EQ(detached, DETACH_ROUNDS + 1);
#undef JOIN_ROUNDS
#undef DETACH_ROUNDS
	return TEST_SUCCESS;
}
static int task_count(void)
{
	DIR *dir = opendir("/proc/self/task");
	if (!dir)
		return -1;
	int count = 0;
	while (readdir(dir))
		++count;
	closedir(dir);
	return count - 2;
}

int test_u17_kernel_thread_bound(void)
{
	int before = task_count();
	ASSERT_TRUE(before > 0);
	for (int i = 0; i < 128; ++i) {
		mythread_t thread;
		ASSERT_SUCCESS(mythread_create(&thread, NULL, worker_increment, NULL));
		ASSERT_SUCCESS(mythread_join(thread, NULL));
	}
	int after = task_count();
	ASSERT_TRUE(after > 0);
	/* A 1:m runtime keeps one scheduler worker, independent of user count. */
	ASSERT_TRUE(after <= before + 1);
	return TEST_SUCCESS;
}
