#include "mythread.h"
#include "assert.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>

/* Resource / leak tests (M1..M3, M5, M8).
 *
 * Already covered elsewhere:
 *   M4 -> test_join.c join_dead + test_detach.c (strict mode rejects
 *         stale/detached/invalid handles)
 *   M6 -> test_create.c create_until_failure (graceful resource limit)
 *   M7 -> Makefile sanitize / sanitize-mem / valgrind targets
 *
 * Measurement notes:
 *   - RSS is read from /proc/self/statm (page granularity, cheap);
 *     a loop must return to (approximately) its starting footprint.
 *     "Approximately": glibc's malloc keeps freed arenas cached, so
 *     the threshold is deliberately loose — this test catches real
 *     leaks (linear growth), not allocator behaviour.
 *   - Task count comes from /proc/self/task — works for kernel
 *     threads (1.6). A userspace scheduler (1.7) keeps its workers
 *     hidden there; for those runs this check degenerates to a
 *     self-check and still passes, which is honest: the framework
 *     cannot see inside the student's scheduler.
 */

/* Shared helpers ------------------------------------------------------ */

static void *
worker_noop(void *arg)
{
	(void)arg;
	return NULL;
}

static long
rss_pages(void)
{
	FILE *f = fopen("/proc/self/statm", "r");
	if (!f)
		return -1;
	long total = 0, resident = 0;
	if (fscanf(f, "%ld %ld", &total, &resident) != 2)
		resident = -1;
	fclose(f);
	return resident;
}

static int
task_count(void)
{
	DIR *d = opendir("/proc/self/task");
	if (!d)
		return -1;
	int n = 0;
	while (readdir(d) != NULL)
		++n;
	closedir(d);
	/* "." and ".." were counted as entries */
	return n > 2 ? n - 2 : 0;
}

static int
fd_count(void)
{
	DIR *d = opendir("/proc/self/fd");
	if (!d)
		return -1;
	int n = 0;
	while (readdir(d) != NULL)
		++n;
	/* subtract "." ".." and the directory handle itself */
	closedir(d);
	return n > 3 ? n - 3 : 0;
}

/* Tests --------------------------------------------------------------- */

/*
 * [M1] Memory stability across a create/join loop: footprint after the
 * loop must be back near the footprint before it. Loose threshold (pages
 * can be cached by the allocator), but linear leak growth trips it.
 */
int
test_resource_mem_join(void)
{
#define ROUNDS 2000
	long before = rss_pages();
	ASSERT_TRUE(before > 0);

	for (int i = 0; i < ROUNDS; ++i) {
		mythread_t t;
		ASSERT_SUCCESS(mythread_create(&t, NULL, worker_noop, NULL));
		ASSERT_SUCCESS(mythread_join(t, NULL));
	}

	long after = rss_pages();
	ASSERT_TRUE(after > 0);
	/* A thread whose TCB+stack are leaked costs >=~16 pages with
	 * default 64 KiB stacks; 256 pages (1 MiB) slack per aggregate
	 * run absorbs allocator noise while still catching a leak. */
	ASSERT_TRUE(after - before < 256);
#undef ROUNDS
	return TEST_SUCCESS;
}

/*
 * [M2] Same for a detach loop: detached threads must be reclaimed by
 * the library itself, nobody ever joins them.
 */
int
test_resource_mem_detach(void)
{
#define ROUNDS 2000
	long before = rss_pages();
	ASSERT_TRUE(before > 0);
	int base = task_count();
	ASSERT_TRUE(base > 0);

	for (int i = 0; i < ROUNDS; ++i) {
		mythread_t t;
		ASSERT_SUCCESS(mythread_create(&t, NULL, worker_noop, NULL));
		ASSERT_SUCCESS(mythread_detach(t));
	}

	/* detached threads die whenever: wait (bounded) until the kernel
	 * has reaped them all instead of guessing with a busy loop */
	for (int ms = 0; ms < 3000 && task_count() > base; ms += 5)
		usleep(5 * 1000);
	ASSERT_EQ(task_count(), base);

	long after = rss_pages();
	ASSERT_TRUE(after > 0);
	ASSERT_TRUE(after - before < 256);
#undef ROUNDS
	return TEST_SUCCESS;
}

/*
 * [M3, M5] Kernel-visible thread reclamation: after joining every
 * thread it created, the process must be back to exactly one task
 * (itself). No zombies, no stale TIDs, no orphaned workers. A 1.7
 * userspace scheduler hides its workers from /proc — there this check
 * trivially holds (1 task), which is expected and not a failure.
 */
int
test_resource_threads_reclaimed(void)
{
#define N 32
	static mythread_t t[N];

	int base = task_count();
	ASSERT_TRUE(base > 0);

	for (int i = 0; i < N; ++i)
		ASSERT_SUCCESS(mythread_create(&t[i], NULL, worker_noop,
		                               NULL));
	for (int i = 0; i < N; ++i)
		ASSERT_SUCCESS(mythread_join(t[i], NULL));

	int after = task_count();
	ASSERT_TRUE(after > 0);
	ASSERT_EQ(after, base);
#undef N
	return TEST_SUCCESS;
}

/*
 * [M8] File-descriptor hygiene: create/join and detach loops must not
 * open descriptors. Kernel-thread backends (clone-based 1.6) open
 * nothing per thread; a backend that leaks an fd per thread (e.g. an
 * eventfd per worker that is never closed) grows the count linearly.
 */
int
test_resource_fds(void)
{
#define ROUNDS 256
	int before = fd_count();
	ASSERT_TRUE(before >= 0);

	for (int i = 0; i < ROUNDS; ++i) {
		mythread_t t;
		ASSERT_SUCCESS(mythread_create(&t, NULL, worker_noop, NULL));
		ASSERT_SUCCESS(mythread_join(t, NULL));
	}

	int after = fd_count();
	ASSERT_TRUE(after >= 0);
	/* allow a couple for lazy runtime init; a leak shows as growth
	 * of ROUNDS scale */
	ASSERT_TRUE(after - before < 4);
#undef ROUNDS
	return TEST_SUCCESS;
}
