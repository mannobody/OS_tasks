#define _XOPEN_SOURCE 700

#include "mythread.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <ucontext.h>

/*
 * 1.7 implementation: one kernel scheduler thread multiplexes an arbitrary
 * number of user contexts.  A user context gives up the scheduler while it
 * joins another user thread; return from the start routine is another
 * scheduling point.  This deliberately implements the non-preemptive model
 * allowed by the assignment.
 */

enum thread_state {
	THREAD_READY,
	THREAD_RUNNING,
	THREAD_BLOCKED,
	THREAD_DONE
};

struct user_thread {
	mythread_t id;
	ucontext_t context;
	void *stack;
	void *(*start)(void *);
	void *arg;
	void *result;
	enum thread_state state;
	int detached;
	int join_claimed;
	int result_taken;
	struct user_thread *waiter;
	struct user_thread *next_all;
	struct user_thread *next_ready;
};

static pthread_once_t init_once = PTHREAD_ONCE_INIT;
static pthread_t scheduler_thread;
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t state_changed = PTHREAD_COND_INITIALIZER;
static struct user_thread *all_threads;
static struct user_thread *ready_head;
static struct user_thread *ready_tail;
static mythread_t next_id = 1;

static ucontext_t scheduler_context;
static _Thread_local struct user_thread *current_thread;

static void queue_ready_locked(struct user_thread *thread)
{
	thread->next_ready = NULL;
	if (ready_tail)
		ready_tail->next_ready = thread;
	else
		ready_head = thread;
	ready_tail = thread;
}

static struct user_thread *dequeue_ready_locked(void)
{
	struct user_thread *thread = ready_head;
	if (!thread)
		return NULL;
	ready_head = thread->next_ready;
	if (!ready_head)
		ready_tail = NULL;
	thread->next_ready = NULL;
	return thread;
}

static struct user_thread *find_thread_locked(mythread_t id)
{
	for (struct user_thread *thread = all_threads; thread; thread = thread->next_all)
		if (thread->id == id)
			return thread;
	return NULL;
}

static void unlink_thread_locked(struct user_thread *target)
{
	struct user_thread **link = &all_threads;
	while (*link && *link != target)
		link = &(*link)->next_all;
	if (*link == target)
		*link = target->next_all;
}

static void reclaim_thread_locked(struct user_thread *thread)
{
	unlink_thread_locked(thread);
	free(thread->stack);
	free(thread);
}

static void reap_finished_locked(void)
{
	struct user_thread *thread = all_threads;
	while (thread) {
		struct user_thread *next = thread->next_all;
		if (thread->state == THREAD_DONE &&
		    (thread->detached || thread->result_taken))
			reclaim_thread_locked(thread);
		thread = next;
	}
}

static void finish_current(void *result)
{
	struct user_thread *thread = current_thread;

	pthread_mutex_lock(&state_lock);
	thread->result = result;
	thread->state = THREAD_DONE;
	if (thread->waiter) {
		thread->waiter->state = THREAD_READY;
		queue_ready_locked(thread->waiter);
		thread->waiter = NULL;
	}
	pthread_cond_broadcast(&state_changed);
	pthread_mutex_unlock(&state_lock);

	/* The scheduler owns reclamation because this stack is still active. */
	current_thread = NULL;
	swapcontext(&thread->context, &scheduler_context);
	abort();
}

static void thread_entry(uintptr_t raw_thread)
{
	struct user_thread *thread = (struct user_thread *)raw_thread;
	current_thread = thread;
	finish_current(thread->start(thread->arg));
}

static void *scheduler_main(void *unused)
{
	(void)unused;

	for (;;) {
		pthread_mutex_lock(&state_lock);
		for (;;) {
			reap_finished_locked();
			if (ready_head)
				break;
			pthread_cond_wait(&state_changed, &state_lock);
		}

		struct user_thread *thread = dequeue_ready_locked();
		if (thread->state != THREAD_READY) {
			pthread_mutex_unlock(&state_lock);
			continue;
		}
		thread->state = THREAD_RUNNING;
		current_thread = thread;
		pthread_mutex_unlock(&state_lock);

		swapcontext(&scheduler_context, &thread->context);
		current_thread = NULL;

		pthread_mutex_lock(&state_lock);
		reap_finished_locked();
		pthread_mutex_unlock(&state_lock);
	}
	return NULL;
}

static void start_scheduler(void)
{
	int rc = pthread_create(&scheduler_thread, NULL, scheduler_main, NULL);
	if (rc != 0)
		abort();
	/* The scheduler is a process-internal service, never joined by clients. */
	pthread_detach(scheduler_thread);
}

static void ensure_scheduler(void)
{
	(void)pthread_once(&init_once, start_scheduler);
}

int mythread_create(mythread_t *thread_handle,
	                   const mythread_attr_t *attr,
	                   void *(*start_routine)(void *),
	                   void *arg)
{
	(void)attr;
	if (!thread_handle || !start_routine)
		return EINVAL;

	ensure_scheduler();

	struct user_thread *thread = calloc(1, sizeof *thread);
	if (!thread)
		return ENOMEM;
	thread->stack = malloc(64 * 1024);
	if (!thread->stack) {
		free(thread);
		return ENOMEM;
	}
	thread->start = start_routine;
	thread->arg = arg;
	thread->state = THREAD_READY;

	if (getcontext(&thread->context) == -1) {
		free(thread->stack);
		free(thread);
		return errno ? errno : EINVAL;
	}
	thread->context.uc_stack.ss_sp = thread->stack;
	thread->context.uc_stack.ss_size = 64 * 1024;
	thread->context.uc_link = &scheduler_context;
	makecontext(&thread->context, (void (*)(void))thread_entry, 1,
	            (uintptr_t)thread);

	pthread_mutex_lock(&state_lock);
	thread->id = next_id++;
	if (thread->id == 0)
		thread->id = next_id++;
	thread->next_all = all_threads;
	all_threads = thread;
	queue_ready_locked(thread);
	*thread_handle = thread->id;
	pthread_cond_signal(&state_changed);
	pthread_mutex_unlock(&state_lock);
	return 0;
}

int mythread_join(mythread_t id, void **retval)
{
	ensure_scheduler();
	struct user_thread *self = current_thread;

	pthread_mutex_lock(&state_lock);
	struct user_thread *target = find_thread_locked(id);
	if (!target || target->detached || target->join_claimed) {
		pthread_mutex_unlock(&state_lock);
		return ESRCH;
	}
	if (self && target == self) {
		pthread_mutex_unlock(&state_lock);
		return EDEADLK;
	}

	target->join_claimed = 1;
	if (self) {
		if (target->state != THREAD_DONE) {
			if (target->waiter) {
				target->join_claimed = 0;
				pthread_mutex_unlock(&state_lock);
				return EBUSY;
			}
			target->waiter = self;
			self->state = THREAD_BLOCKED;
			pthread_mutex_unlock(&state_lock);
			swapcontext(&self->context, &scheduler_context);
			pthread_mutex_lock(&state_lock);
		}
		if (target->state != THREAD_DONE) {
			pthread_mutex_unlock(&state_lock);
			return EFAULT;
		}
	} else {
		while (target->state != THREAD_DONE)
			pthread_cond_wait(&state_changed, &state_lock);
	}

	if (retval)
		*retval = target->result;
	target->result_taken = 1;
	pthread_cond_broadcast(&state_changed);
	pthread_mutex_unlock(&state_lock);
	return 0;
}

int mythread_detach(mythread_t id)
{
	ensure_scheduler();
	pthread_mutex_lock(&state_lock);
	struct user_thread *target = find_thread_locked(id);
	if (!target || target->detached || target->join_claimed) {
		pthread_mutex_unlock(&state_lock);
		return ESRCH;
	}
	target->detached = 1;
	pthread_cond_broadcast(&state_changed);
	pthread_mutex_unlock(&state_lock);
	return 0;
}

void mythread_exit(void *retval)
{
	if (!current_thread)
		exit(42);
	finish_current(retval);
}
