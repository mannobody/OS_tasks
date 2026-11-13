#include "mythread.h"
#include "assert.h"
#include <stdint.h>
static void *worker_exit_value(void *arg)
{
	int *after = arg;
	mythread_exit((void *)(intptr_t)0x17);
	*after = 1;
	return NULL;
}

int test_u17_exit_value(void)
{
	int after = 0;
	mythread_t thread;
	ASSERT_SUCCESS(mythread_create(&thread, NULL, worker_exit_value, &after));

	void *result = NULL;
	ASSERT_SUCCESS(mythread_join(thread, &result));
	ASSERT_EQ((intptr_t)result, 0x17);
	ASSERT_EQ(after, 0);
	return TEST_SUCCESS;
}

static void *worker_exit_null(void *arg)
{
	int *after = arg;
	mythread_exit(NULL);
	*after = 1;
	return (void *)(intptr_t)-1;
}

int test_u17_exit_null(void)
{
	int after = 0;
	mythread_t thread;
	ASSERT_SUCCESS(mythread_create(&thread, NULL, worker_exit_null, &after));
	void *result = (void *)(intptr_t)1;
	ASSERT_SUCCESS(mythread_join(thread, &result));
	ASSERT_NULL(result);
	ASSERT_EQ(after, 0);
	return TEST_SUCCESS;
}
