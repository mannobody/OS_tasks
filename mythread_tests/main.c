#include "runner.h"
#include "tests.h"
#include <stddef.h>

int
main(void)
{
	const int timeout = 5000;
	const test_case_t tests[] =
	{
		{"create_basic",           test_create_basic,           timeout},
		{"create_data_passing",    test_create_data_passing,    timeout},
		{"create_parallel_run",    test_create_parallel_run,    timeout},
		{"create_distinct_stacks", test_create_distinct_stacks, timeout},
		{"create_until_failure",   test_create_until_failure,   timeout},
		{"create_nested",          test_create_nested,          timeout},

		{"join_null_retval",     test_join_null_retval,     timeout},
		{"join_retval",          test_join_retval,          timeout},
		{"join_multiple",        test_join_multiple,        timeout},
		{"join_blocks",          test_join_blocks,          timeout},
		{"join_already_exited",  test_join_already_exited,  timeout},
		{"join_detached",        test_join_detached,        timeout},
		{"join_dead",            test_join_dead,            timeout},
		{"join_concurrent",      test_join_concurrent,      timeout},
		{"join_from_thread",     test_join_from_thread,     timeout},

		{"detach_running",       test_detach_running,       timeout},
		{"detach_after_exit",    test_detach_after_exit,    timeout},
		{"detach_double",        test_detach_double,        timeout},
		{"detach_after_join",    test_detach_after_join,    timeout},
		{"detach_invalid",       test_detach_invalid,       timeout},
		{"detach_many",          test_detach_many,          timeout},
		{"detach_race",          test_detach_race,          timeout},

		{"exit_null",            test_exit_null,            timeout},
		{"exit_retval",          test_exit_retval,          timeout},
		{"exit_stops_thread",    test_exit_stops_thread,    timeout},
		{"exit_detached",        test_exit_detached,        timeout},
		{"exit_join_blocks",     test_exit_join_blocks,     timeout},
		{"exit_immediate",       test_exit_immediate,       timeout},
		{"exit_nested",          test_exit_nested,          timeout},
		{"exit_from_main",       test_exit_from_main,       timeout},

		{"stress_many_threads",  test_stress_many_threads,  timeout},
		{"stress_churn",         test_stress_churn,         timeout},
		{"stress_scale",         test_stress_scale,         timeout * 4},
		{"stress_random_join",   test_stress_random_join_order, timeout},
		{"stress_detached_count", test_stress_detached_count, timeout},
		{"stress_mixed",         test_stress_mixed,         timeout},
		{"stress_nested_deep",   test_stress_nested_deep,   timeout},
		{"stress_soak",          test_stress_soak,          timeout * 3},
		{"stress_mixed_sizes",   test_stress_mixed_sizes,   timeout * 2},
		{"stress_mixed_exit",    test_stress_mixed_exit_paths, timeout},

		{"main_exit_detached",   test_main_exit_detached,   timeout},
		{"main_return_detached", test_main_return_detached, timeout},
		{"join_chain",           test_join_chain,           timeout},
		{"lifecycle_waves",      test_lifecycle_waves,      timeout},
		{"handle_reuse",         test_handle_reuse,         timeout},

		{"race_pingpong",        test_race_pingpong,        timeout},
		{"race_producer_consumer", test_race_producer_consumer, timeout},
		{"race_exit_join",       test_race_exit_join,       timeout},
		{"race_exit_detach",     test_race_exit_detach,     timeout},
		{"race_parallel_joins",  test_race_parallel_joins,  timeout},
		{"race_nested_create_join", test_race_nested_create_join, timeout},
		{"race_cross_detach",    test_race_cross_detach,    timeout},

		{"resource_mem_join",    test_resource_mem_join,    timeout * 2},
		{"resource_mem_detach",  test_resource_mem_detach,  timeout * 2},
		{"resource_reclaimed",   test_resource_threads_reclaimed, timeout},
		{"resource_fds",         test_resource_fds,         timeout},

		{"error_null_routine",   test_error_null_routine,   timeout},
		{"error_join_invalid",   test_error_join_invalid,   timeout},
		{"error_join_self",      test_error_join_self,      timeout},

		{"kernel_errno",         test_kernel_errno_per_thread, timeout},
		{"kernel_tls",           test_kernel_tls_per_thread,   timeout},
		{"kernel_ids",           test_kernel_ids,              timeout},
		{"kernel_fd_shared",     test_kernel_fd_shared,        timeout},
		{"kernel_stack_align",   test_kernel_stack_align,      timeout},
		{"kernel_deep_stack",    test_kernel_deep_stack,       timeout},
		{"kernel_libc_parallel", test_kernel_libc_parallel,    timeout},
	};

	const int n = (int)(sizeof(tests) / sizeof(tests[0]));

	return run_all(tests, n);
}
