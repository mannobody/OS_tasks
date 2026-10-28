#include "runner.h"
#include "tests.h"
#include <stddef.h>

int
main(void)
{
	const int timeout = 5000;
	const test_case_t tests[] =
	{
		{"create_basic",           test_create_basic,           timeout, 0},
		{"create_data_passing",    test_create_data_passing,    timeout, 0},
		{"create_parallel_run",    test_create_parallel_run,    timeout, 0},
		{"create_distinct_stacks", test_create_distinct_stacks, timeout, 0},
		{"create_until_failure",   test_create_until_failure,   timeout,
		                           TEST_SKIP_VALGRIND},
		{"create_nested",          test_create_nested,          timeout, 0},

		{"join_null_retval",     test_join_null_retval,     timeout, 0},
		{"join_retval",          test_join_retval,          timeout, 0},
		{"join_multiple",        test_join_multiple,        timeout, 0},
		{"join_blocks",          test_join_blocks,          timeout, 0},
		{"join_already_exited",  test_join_already_exited,  timeout, 0},
		{"join_detached",        test_join_detached,        timeout, 0},
		{"join_dead",            test_join_dead,            timeout, 0},
		{"join_concurrent",      test_join_concurrent,      timeout, 0},
		{"join_from_thread",     test_join_from_thread,     timeout, 0},

		{"detach_running",       test_detach_running,       timeout, 0},
		{"detach_after_exit",    test_detach_after_exit,    timeout, 0},
		{"detach_double",        test_detach_double,        timeout, 0},
		{"detach_after_join",    test_detach_after_join,    timeout, 0},
		{"detach_invalid",       test_detach_invalid,       timeout, 0},
		{"detach_many",          test_detach_many,          timeout, 0},
		{"detach_race",          test_detach_race,          timeout, 0},

		{"exit_null",            test_exit_null,            timeout, 0},
		{"exit_retval",          test_exit_retval,          timeout, 0},
		{"exit_stops_thread",    test_exit_stops_thread,    timeout, 0},
		{"exit_detached",        test_exit_detached,        timeout, 0},
		{"exit_join_blocks",     test_exit_join_blocks,     timeout, 0},
		{"exit_immediate",       test_exit_immediate,       timeout, 0},
		{"exit_nested",          test_exit_nested,          timeout, 0},
		{"exit_from_main",       test_exit_from_main,       timeout,
		                         TEST_EXITS_PROCESS | TEST_SKIP_SANITIZE},
		{"exit_from_thread",     test_exit_from_thread,     timeout, TEST_EXITS_PROCESS},

		{"stress_many_threads",  test_stress_many_threads,  timeout, 0},
		{"stress_churn",         test_stress_churn,         timeout, 0},
		{"stress_scale",         test_stress_scale,         timeout * 4, 0},
		{"stress_random_join",   test_stress_random_join_order, timeout, 0},
		{"stress_detached_count", test_stress_detached_count, timeout, 0},
		{"stress_mixed",         test_stress_mixed,         timeout, 0},
		{"stress_nested_deep",   test_stress_nested_deep,   timeout, 0},
		{"stress_soak",          test_stress_soak,          timeout * 3, 0},
		{"stress_mixed_sizes",   test_stress_mixed_sizes,   timeout * 2, 0},
		{"stress_mixed_exit",    test_stress_mixed_exit_paths, timeout, 0},

		{"main_exit_detached",   test_main_exit_detached,   timeout, TEST_EXITS_PROCESS},
		{"main_return_detached", test_main_return_detached, timeout, TEST_EXITS_PROCESS},
		{"join_chain",           test_join_chain,           timeout, 0},
		{"lifecycle_waves",      test_lifecycle_waves,      timeout, 0},
		{"handle_reuse",         test_handle_reuse,         timeout, 0},

		{"race_pingpong",        test_race_pingpong,        timeout, 0},
		{"race_producer_consumer", test_race_producer_consumer, timeout, 0},
		{"race_exit_join",       test_race_exit_join,       timeout, 0},
		{"race_exit_detach",     test_race_exit_detach,     timeout, 0},
		{"race_parallel_joins",  test_race_parallel_joins,  timeout, 0},
		{"race_nested_create_join", test_race_nested_create_join, timeout, 0},
		{"race_cross_detach",    test_race_cross_detach,    timeout, 0},

		{"resource_mem_join",    test_resource_mem_join,    timeout * 2,
		                         TEST_SKIP_SANITIZE | TEST_SKIP_VALGRIND},
		{"resource_mem_detach",  test_resource_mem_detach,  timeout * 2,
		                         TEST_SKIP_SANITIZE | TEST_SKIP_VALGRIND},
		{"resource_reclaimed",   test_resource_threads_reclaimed, timeout, TEST_SKIP_SANITIZE},
		{"resource_fds",         test_resource_fds,         timeout, 0},

		{"error_null_routine",   test_error_null_routine,   timeout, 0},
		{"error_join_invalid",   test_error_join_invalid,   timeout, 0},
		{"error_join_self",      test_error_join_self,      timeout, 0},

		{"kernel_errno",         test_kernel_errno_per_thread, timeout, 0},
		{"kernel_tls",           test_kernel_tls_per_thread,   timeout, 0},
		{"kernel_ids",           test_kernel_ids,              timeout, 0},
		{"kernel_fd_shared",     test_kernel_fd_shared,        timeout, 0},
		{"kernel_stack_align",   test_kernel_stack_align,      timeout, 0},
		{"kernel_deep_stack",    test_kernel_deep_stack,       timeout, 0},
		{"kernel_libc_parallel", test_kernel_libc_parallel,    timeout, 0},
	};

	const int n = (int)(sizeof(tests) / sizeof(tests[0]));

	return run_all(tests, n);
}
