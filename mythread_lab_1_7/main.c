#include "runner.h"
#include "tests.h"

int main(int argc, char **argv)
{
	const test_case_t tests[] = {
		{"u17_create_join_many",  test_u17_create_join_many,  5000, 0},
		{"u17_join_value_modes",  test_u17_join_value_modes,  5000, 0},
		{"u17_reverse_join",      test_u17_reverse_join,      5000, 0},
		{"u17_create_from_user",  test_u17_create_from_user,  5000, 0},
		{"u17_nested_chain",      test_u17_nested_chain,      5000, 0},
		{"u17_nested_join",       test_u17_nested_join,       5000, 0},
		{"u17_exit_value",        test_u17_exit_value,        5000, 0},
		{"u17_exit_null",         test_u17_exit_null,         5000, 0},
		{"u17_detach_queue",      test_u17_detach_queue,      5000, 0},
		{"u17_detach_from_user",  test_u17_detach_from_user,  5000, 0},
		{"u17_detach_double",     test_u17_detach_double,     5000, 0},
		{"u17_stack_isolation",   test_u17_stack_isolation,   5000, 0},
		{"u17_handles",           test_u17_handles,           5000, 0},
		{"u17_handle_uniqueness", test_u17_handle_uniqueness, 5000, 0},
		{"u17_reuse_cycles",      test_u17_reuse_cycles,      5000, 0},
		{"u17_kernel_thread_bound", test_u17_kernel_thread_bound, 5000,
			TEST_SKIP_SANITIZE | TEST_SKIP_VALGRIND},
	};
	return run_selected(tests, (int)(sizeof tests / sizeof tests[0]),
	                    (const char *const *)argv + 1, argc - 1);
}
