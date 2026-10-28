#include "runner.h"
#include "colors.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

#define DEFAULT_TIMEOUT_MS 5000

/*
 * Stand-in for "return from main()". A plain return inside our test
 * harness would only unwind the test function, not the process, so we
 * spell the process-level semantics out: like a real return from main,
 * this must end the process even with detached threads still running.
 */
void
test_return_main(int value)
{
	exit(value);
}

static int
run_one(const test_case_t *t)
{
	int timeout = t->timeout_ms ? t->timeout_ms : DEFAULT_TIMEOUT_MS;

	pid_t pid = fork();
	if (pid < 0) {
		perror("fork");
		return TEST_FAILURE;
	}

	if (pid == 0) {
		/* Child: run the test */
		_exit(t->fn());
	}

	/* Parent: wait with timeout */
	int waited = 0;
	int status = 0;

	while (waited < timeout) {
		pid_t r = waitpid(pid, &status, WNOHANG);
		if (r == pid) {
			if (WIFEXITED(status)) {
				int code = WEXITSTATUS(status);
				/* A test that terminated the whole process via
				 * mythread_exit reports through exit code 42. */
				if (code == TEST_EXIT_MARKER &&
				    (t->flags & TEST_EXITS_PROCESS))
					return TEST_SUCCESS;
				return code;
			}
			/* died from signal */
			fprintf(stderr, "\n  child killed by signal %d "
				"(crash / abort)\n", WTERMSIG(status));
			return TEST_FAILURE;
		}
		usleep(10 * 1000);
		waited += 10;
	}

	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);
	return TEST_TIMEOUT;
}

int
run_all(const test_case_t *tests, int n)
{
	int passed = 0, failed = 0, skipped = 0;

	/* [H3] MYTHREAD_REPEAT re-runs every test in a fresh process,
	 * catching nondeterministic failures (races in particular). 1 is
	 * the default; MYTHREAD_REPEAT=20 makes a solid race-hunt run. */
	const char *rep_env = getenv("MYTHREAD_REPEAT");
	int repeat = rep_env ? atoi(rep_env) : 1;
	if (repeat < 1)
		repeat = 1;

	for (int i = 0; i < n; ++i) {
		const test_case_t *t = &tests[i];

		printf("RUN %s ... ", t->name);
		fflush(stdout);

		int rc = TEST_SUCCESS;
		for (int rep = 0; rep < repeat && rc == TEST_SUCCESS; ++rep) {
			int one = run_one(t);
			if (one == TEST_SKIP) {
				/* does not apply: no point repeating it */
				rc = one;
			} else if (one != TEST_SUCCESS) {
				if (repeat > 1)
					fprintf(stderr,
					    "\n  (failed on repetition "
					    "%d/%d)\n", rep + 1, repeat);
				rc = one;
			}
		}

		if (rc == TEST_SUCCESS) {
			printf(COLOR_GREEN "OK" COLOR_RESET "\n");
			passed++;
		} else if (rc == TEST_SKIP) {
			printf(COLOR_YELLOW "SKIP" COLOR_RESET "\n");
			skipped++;
		} else if (rc == TEST_TIMEOUT) {
			printf(COLOR_RED "TIMEOUT" COLOR_RESET "\n");
			failed++;
		} else {
			printf(COLOR_RED "FAIL" COLOR_RESET "\n");
			failed++;
		}
	}

	printf("\npassed: %d, failed: %d, skipped: %d\n",
		   passed, failed, skipped);
	return failed == 0 ? 0 : 1;
}
