#include "runner.h"
#include "colors.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

#define DEFAULT_TIMEOUT_MS 5000

/* GCC defines __SANITIZE_*__, clang reports through __has_feature. */
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define SANITIZED 1
#endif
#endif
#ifndef SANITIZED
#define SANITIZED 0
#endif

/* Valgrind is a runtime matter: its client request answers exactly when
 * the header is installed; otherwise its preload library in LD_PRELOAD
 * gives it away. */
#if defined(__has_include)
#if __has_include(<valgrind/valgrind.h>)
#include <valgrind/valgrind.h>
#define HAVE_VALGRIND_H 1
#endif
#endif

static int
under_valgrind(void)
{
#ifdef HAVE_VALGRIND_H
	return RUNNING_ON_VALGRIND != 0;
#else
	const char *pre = getenv("LD_PRELOAD");
	return pre && strstr(pre, "vgpreload") != NULL;
#endif
}

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

/*
 * MYTHREAD_TIMEOUT_SCALE multiplies every test's timeout, for runs that
 * are slow as a whole: valgrind, sanitizers, a loaded CI machine.
 * Anything that is not a positive number is ignored with a warning.
 */
static double
timeout_scale(void)
{
	const char *env = getenv("MYTHREAD_TIMEOUT_SCALE");
	if (!env)
		return 1.0;

	char *end;
	double scale = strtod(env, &end);
	if (end == env || *end != '\0' || !(scale > 0)) {
		fprintf(stderr, "warning: MYTHREAD_TIMEOUT_SCALE=%s is not a "
			"positive number, ignored\n", env);
		return 1.0;
	}
	return scale;
}

/*
 * Each test runs in a process group of its own, and the runner kills
 * the whole group once the test is over. A broken library can leave
 * processes behind — clone() without CLONE_THREAD makes real children
 * that outlive the test — which would keep spinning and hold the
 * runner's stdout open (`./mythread_tests | head` never ends). If the
 * runner itself is interrupted it takes the running group down too, as
 * the group no longer gets the terminal's Ctrl-C.
 */
static volatile sig_atomic_t running_group;

static void
on_fatal_signal(int sig)
{
	if (running_group)
		kill(-running_group, SIGKILL);
	signal(sig, SIG_DFL);
	raise(sig);
}

static void
install_signal_handlers(void)
{
	static const int sigs[] = { SIGINT, SIGTERM, SIGHUP, SIGQUIT };
	struct sigaction sa;

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_fatal_signal;
	sigemptyset(&sa.sa_mask);
	for (unsigned i = 0; i < sizeof sigs / sizeof sigs[0]; ++i)
		sigaction(sigs[i], &sa, NULL);
}

static int
run_one(const test_case_t *t, double scale)
{
	double ms = (t->timeout_ms ? t->timeout_ms : DEFAULT_TIMEOUT_MS) * scale;
	/* at least one poll interval; capped so the wait loop cannot overflow */
	int timeout = ms < 10 ? 10 : ms > INT_MAX / 2 ? INT_MAX / 2 : (int)ms;

	if (SANITIZED && (t->flags & TEST_SKIP_SANITIZE))
		return TEST_SKIP;
	if ((t->flags & TEST_SKIP_VALGRIND) && under_valgrind())
		return TEST_SKIP;

	pid_t pid = fork();
	if (pid < 0) {
		perror("fork");
		return TEST_FAILURE;
	}

	if (pid == 0) {
		/* Child: run the test in its own process group */
		setpgid(0, 0);
		_exit(t->fn());
	}
	setpgid(pid, pid);   /* both sides, so neither order can race */
	running_group = pid;

	/* Parent: wait with timeout. WNOWAIT leaves the child a zombie, so
	 * its pid, and with it the group id, cannot be reused before the
	 * group is killed. */
	int waited = 0;
	int status = 0;
	int exited = 0;

	while (waited < timeout) {
		siginfo_t si;
		si.si_pid = 0;
		if (waitid(P_PID, (id_t)pid, &si,
		           WEXITED | WNOHANG | WNOWAIT) == 0 && si.si_pid == pid) {
			exited = 1;
			break;
		}
		usleep(10 * 1000);
		waited += 10;
	}

	kill(-pid, SIGKILL);   /* the test and anything it left behind */
	waitpid(pid, &status, 0);
	running_group = 0;

	if (!exited)
		return TEST_TIMEOUT;
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
	double scale = timeout_scale();
	install_signal_handlers();

	for (int i = 0; i < n; ++i) {
		const test_case_t *t = &tests[i];

		printf("RUN %s ... ", t->name);
		fflush(stdout);

		int rc = TEST_SUCCESS;
		for (int rep = 0; rep < repeat && rc == TEST_SUCCESS; ++rep) {
			int one = run_one(t, scale);
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
