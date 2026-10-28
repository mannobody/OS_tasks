#ifndef TESTS_H
#define TESTS_H

/* All test entries for main.c's registry. Each returns TEST_SUCCESS or
 * TEST_FAILURE (runner.h) and is run in a forked child by the runner. */

/* create */
int test_create_basic(void);
int test_create_data_passing(void);
int test_create_parallel_run(void);
int test_create_distinct_stacks(void);
int test_create_until_failure(void);
int test_create_nested(void);

/* join */
int test_join_null_retval(void);
int test_join_retval(void);
int test_join_multiple(void);
int test_join_blocks(void);
int test_join_already_exited(void);
int test_join_detached(void);
int test_join_dead(void);
int test_join_concurrent(void);
int test_join_from_thread(void);

/* detach */
int test_detach_running(void);
int test_detach_after_exit(void);
int test_detach_double(void);
int test_detach_after_join(void);
int test_detach_invalid(void);
int test_detach_many(void);
int test_detach_race(void);

/* exit */
int test_exit_null(void);
int test_exit_retval(void);
int test_exit_stops_thread(void);
int test_exit_detached(void);
int test_exit_join_blocks(void);
int test_exit_immediate(void);
int test_exit_nested(void);
int test_exit_from_main(void);    /* never returns: exits the process */

/* stress */
int test_stress_many_threads(void);
int test_stress_churn(void);
int test_stress_scale(void);
int test_stress_random_join_order(void);
int test_stress_detached_count(void);
int test_stress_mixed(void);
int test_stress_nested_deep(void);
int test_stress_soak(void);
int test_stress_mixed_sizes(void);
int test_stress_mixed_exit_paths(void);

/* cross-function lifecycle */
int test_main_exit_detached(void);   /* never returns: exits the process */
int test_main_return_detached(void); /* never returns: exits the process */
int test_join_chain(void);
int test_lifecycle_waves(void);
int test_handle_reuse(void);

/* concurrency / races */
int test_race_pingpong(void);
int test_race_producer_consumer(void);
int test_race_exit_join(void);
int test_race_exit_detach(void);
int test_race_parallel_joins(void);
int test_race_nested_create_join(void);
int test_race_cross_detach(void);

/* resources / leaks */
int test_resource_mem_join(void);
int test_resource_mem_detach(void);
int test_resource_threads_reclaimed(void);
int test_resource_fds(void);

/* error handling */
int test_error_null_routine(void);
int test_error_join_invalid(void);
int test_error_join_self(void);

/* kernel-thread semantics */
int test_kernel_errno_per_thread(void);
int test_kernel_tls_per_thread(void);
int test_kernel_ids(void);

#endif
