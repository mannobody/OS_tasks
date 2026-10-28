#include "mythread.h"
#include "assert.h"
#include <stdint.h>
#include <stddef.h>

/* Concurrency / race tests (R2, R3, R6..R8, R11, R12).
 *
 * Already covered elsewhere:
 *   R1, R10 -> tests/test_stress.c   stress_many_threads (atomic counter)
 *   R4      -> tests/test_join.c     join_blocks
 *   R5      -> tests/test_detach.c   detach_running
 *   R9      -> tests/test_join.c     join_concurrent
 *
 * Everything here uses plain atomic builtins for inter-thread
 * signalling (no volatile-based "synchronization", which is not
 * synchronization at all). Busy-wait is deliberate: the tests must
 * not require semaphores/condvars a 1.6/1.7 library does not provide.
 */

/* Shared helpers ------------------------------------------------------ */

static void *
worker_noop(void *arg)
{
	(void)arg;
	return NULL;
}

static void *
worker_echo(void *arg)
{
	return arg;
}

struct gate {
	volatile int open;
};

/* Parks on a gate, then returns a pre-agreed value [R8, R12]. */
struct park_ret {
	struct gate g;
	intptr_t val;
};

static void *
worker_park_ret(void *arg)
{
	struct park_ret *p = (struct park_ret *)arg;

	while (!__atomic_load_n(&p->g.open, __ATOMIC_SEQ_CST))
		;
	return (void *)p->val;
}

/* Tests --------------------------------------------------------------- */

/*
 * [R2] Ping-pong: two threads strictly alternate, each advancing a
 * shared step counter only when the parity is theirs. The atomic
 * protocol forces TURNS switches in each direction; any lost or
 * doubled step leaves the final counts wrong.
 */
#define R2_TURNS 200
struct r2_pp {
	volatile int steps;   /* total advanced, atomic */
};

struct r2_player {
	struct r2_pp *pp;
	int parity;           /* this player advances even/odd steps */
	int count;            /* private tally, verified after join */
};

static void *
worker_r2_player(void *arg)
{
	struct r2_player *pl = (struct r2_player *)arg;
	struct r2_pp *pp = pl->pp;

	for (;;) {
		int s = __atomic_load_n(&pp->steps, __ATOMIC_SEQ_CST);
		if (s >= 2 * R2_TURNS)
			break;
		if (s % 2 == pl->parity) {
			__atomic_fetch_add(&pp->steps, 1, __ATOMIC_SEQ_CST);
			++pl->count;
		}
	}
	return NULL;
}

int
test_race_pingpong(void)
{
	static struct r2_pp pp;
	static struct r2_player p[2];
	static mythread_t t[2];

	pp.steps = 0;
	for (int i = 0; i < 2; ++i) {
		p[i].pp = &pp;
		p[i].parity = i;
		p[i].count = 0;
		ASSERT_SUCCESS(mythread_create(&t[i], NULL,
		                               worker_r2_player, &p[i]));
	}
	for (int i = 0; i < 2; ++i)
		ASSERT_SUCCESS(mythread_join(t[i], NULL));

	ASSERT_EQ(pp.steps, 2 * R2_TURNS);
	ASSERT_EQ(p[0].count, R2_TURNS);
	ASSERT_EQ(p[1].count, R2_TURNS);
	return TEST_SUCCESS;
}

/*
 * [R3] Producer-consumer over a single slot guarded by atomic flags.
 * The consumer records every received item; strict handoff means the
 * sequence must arrive complete and in order. Catches lost wakeups /
 * reordered handoff in message-passing over the thread library.
 */
#define R3_ITEMS 2000
struct r3_pc {
	volatile int full;      /* 0 = slot empty, 1 = slot full */
	volatile intptr_t slot;
	int received[R3_ITEMS];
};

static void *
worker_r3_producer(void *arg)
{
	struct r3_pc *pc = (struct r3_pc *)arg;

	for (int i = 0; i < R3_ITEMS; ++i) {
		while (__atomic_load_n(&pc->full, __ATOMIC_SEQ_CST))
			;
		__atomic_store_n(&pc->slot, (intptr_t)(i + 1),
		                 __ATOMIC_SEQ_CST);
		__atomic_store_n(&pc->full, 1, __ATOMIC_SEQ_CST);
	}
	return NULL;
}

static void *
worker_r3_consumer(void *arg)
{
	struct r3_pc *pc = (struct r3_pc *)arg;

	for (int i = 0; i < R3_ITEMS; ++i) {
		while (!__atomic_load_n(&pc->full, __ATOMIC_SEQ_CST))
			;
		pc->received[i] = (int)__atomic_load_n(&pc->slot,
		                                       __ATOMIC_SEQ_CST);
		__atomic_store_n(&pc->full, 0, __ATOMIC_SEQ_CST);
	}
	return NULL;
}

int
test_race_producer_consumer(void)
{
	static struct r3_pc pc;
	mythread_t prod, cons;

	pc.full = 0;
	pc.slot = 0;

	ASSERT_SUCCESS(mythread_create(&prod, NULL, worker_r3_producer,
	                               &pc));
	ASSERT_SUCCESS(mythread_create(&cons, NULL, worker_r3_consumer,
	                               &pc));
	ASSERT_SUCCESS(mythread_join(cons, NULL));
	ASSERT_SUCCESS(mythread_join(prod, NULL));

	for (int i = 0; i < R3_ITEMS; ++i)
		ASSERT_EQ(pc.received[i], i + 1);
	return TEST_SUCCESS;
}

/*
 * [R6] Exit racing with join. The target leaves almost immediately;
 * the joiner calls join at nearly the same moment. POSIX semantics:
 * whichever wins, join returns the exit value (results are kept until
 * reaped). 100 rounds of churn; every round must deliver the value,
 * so an implementation that loses an exit value under the race fails.
 */
#define R6_ROUNDS 100
struct r6_slot {
	mythread_t target;
	volatile int done;
	int rc;
	intptr_t val;
};

static void *
worker_r6_target(void *arg)
{
	(void)arg;
	mythread_exit((void *)(intptr_t)0x6);
	return (void *)(intptr_t)-1;   /* unreachable */
}

static void *
worker_r6_joiner(void *arg)
{
	struct r6_slot *r = (struct r6_slot *)arg;

	void *ret = NULL;
	r->rc = mythread_join(r->target, &ret);
	r->val = (intptr_t)ret;
	__atomic_store_n(&r->done, 1, __ATOMIC_SEQ_CST);
	return NULL;
}

int
test_race_exit_join(void)
{
	static struct r6_slot r;
	static mythread_t joiner;

	for (int round = 0; round < R6_ROUNDS; ++round) {
		r.done = 0;
		r.rc = -1;
		r.val = 0;

		ASSERT_SUCCESS(mythread_create(&r.target, NULL,
		                               worker_r6_target, NULL));
		/* handle is published before the joiner exists — the race
		 * is join-vs-exit, not join-vs-handle-publication */
		ASSERT_SUCCESS(mythread_create(&joiner, NULL,
		                               worker_r6_joiner, &r));

		while (!__atomic_load_n(&r.done, __ATOMIC_SEQ_CST))
			;

		ASSERT_EQ(r.rc, 0);
		ASSERT_EQ(r.val, (intptr_t)0x6);
	}
	return TEST_SUCCESS;
}

/*
 * [R7] Exit racing with detach: the target terminates via
 * mythread_exit while main detaches it in parallel. One valid outcome
 * per call, no crash, and — the real assertion — the library must
 * stay healthy after 100 rounds:资源和 bookkeeping must not leak.
 */
#define R7_ROUNDS 100
static void *
worker_r7_target(void *arg)
{
	(void)arg;
	mythread_exit((void *)(intptr_t)0x7);
	return (void *)(intptr_t)-1;   /* unreachable */
}

int
test_race_exit_detach(void)
{
	for (int round = 0; round < R7_ROUNDS; ++round) {
		mythread_t t;

		ASSERT_SUCCESS(mythread_create(&t, NULL, worker_r7_target,
		                               NULL));
		(void)mythread_detach(t);

		/* library must be intact after each race round */
		mythread_t probe;
		ASSERT_SUCCESS(mythread_create(&probe, NULL, worker_noop,
		                               NULL));
		ASSERT_SUCCESS(mythread_join(probe, NULL));
	}
	return TEST_SUCCESS;
}

/*
 * [R8] Parallel joins of N different threads. All joiner threads
 * enter join() while every target is still parked — so all N joins
 * are blocked at once — then the gates open and each joiner must
 * collect exactly its own target's value. Verifies that the library
 * supports many simultaneous joiners without cross-talk.
 */
#define R8_N 6
struct r8_joiner {
	volatile int entered;
	volatile int done;
	int rc;
	intptr_t val;
	mythread_t target;
};

static void *
worker_r8_joiner(void *arg)
{
	struct r8_joiner *j = (struct r8_joiner *)arg;

	__atomic_store_n(&j->entered, 1, __ATOMIC_SEQ_CST);
	void *ret = NULL;
	j->rc = mythread_join(j->target, &ret);
	j->val = (intptr_t)ret;
	__atomic_store_n(&j->done, 1, __ATOMIC_SEQ_CST);
	return NULL;
}

int
test_race_parallel_joins(void)
{
	static struct park_ret pr[R8_N];
	static struct r8_joiner jr[R8_N];
	static mythread_t targ[R8_N], joiner[R8_N];
	int vals[R8_N];

	for (int i = 0; i < R8_N; ++i) {
		vals[i] = 0x50 + i;
		pr[i].g.open = 0;
		pr[i].val = (intptr_t)0x50 + i;
		ASSERT_SUCCESS(mythread_create(&targ[i], NULL,
		                               worker_park_ret, &pr[i]));
	}
	for (int i = 0; i < R8_N; ++i) {
		jr[i].entered = 0;
		jr[i].done = 0;
		jr[i].rc = -1;
		jr[i].val = 0;
		jr[i].target = targ[i];
		ASSERT_SUCCESS(mythread_create(&joiner[i], NULL,
		                               worker_r8_joiner, &jr[i]));
	}

	/* every joiner has entered join (targets parked → all blocked) */
	for (int i = 0; i < R8_N; ++i)
		while (!__atomic_load_n(&jr[i].entered, __ATOMIC_SEQ_CST))
			;

	/* release all targets; joins must now complete with own values */
	for (int i = 0; i < R8_N; ++i)
		__atomic_store_n(&pr[i].g.open, 1, __ATOMIC_SEQ_CST);

	for (int i = 0; i < R8_N; ++i)
		while (!__atomic_load_n(&jr[i].done, __ATOMIC_SEQ_CST))
			;

	for (int i = 0; i < R8_N; ++i) {
		ASSERT_EQ(jr[i].rc, 0);
		ASSERT_EQ(jr[i].val, vals[i]);
	}
#undef R8_N
	return TEST_SUCCESS;
}

/*
 * [R11] Nested create/join under concurrency: several parents at once
 * each create two children and join them. Catches races in shared
 * creator/allocator state — the classic failure is two simultaneous
 * mythread_create calls corrupting the thread table.
 */
#define R11_P 4
struct r11_ctx {
	int id;
	volatile int done;
	int rc_a, rc_b;
	intptr_t val_a, val_b;
};

static void *
worker_r11_parent(void *arg)
{
	struct r11_ctx *n = (struct r11_ctx *)arg;
	mythread_t ca, cb;
	void *ret = NULL;

	n->rc_a = mythread_create(&ca, NULL, worker_echo,
	                          (void *)(intptr_t)(0x10 + n->id));
	if (n->rc_a == 0)
		n->rc_a = mythread_join(ca, &ret);
	n->val_a = n->rc_a == 0 ? (intptr_t)ret : -1;

	n->rc_b = mythread_create(&cb, NULL, worker_echo,
	                          (void *)(intptr_t)(0x20 + n->id));
	if (n->rc_b == 0)
		n->rc_b = mythread_join(cb, &ret);
	n->val_b = n->rc_b == 0 ? (intptr_t)ret : -1;

	__atomic_store_n(&n->done, 1, __ATOMIC_SEQ_CST);
	return NULL;
}

int
test_race_nested_create_join(void)
{
	static struct r11_ctx ctx[R11_P];
	static mythread_t parent[R11_P];

	for (int i = 0; i < R11_P; ++i) {
		ctx[i].id = i;
		ctx[i].done = 0;
		ctx[i].rc_a = ctx[i].rc_b = -1;
		ctx[i].val_a = ctx[i].val_b = -1;
		ASSERT_SUCCESS(mythread_create(&parent[i], NULL,
		                               worker_r11_parent, &ctx[i]));
	}
	for (int i = 0; i < R11_P; ++i) {
		while (!__atomic_load_n(&ctx[i].done, __ATOMIC_SEQ_CST))
			;
	}

	for (int i = 0; i < R11_P; ++i) {
		ASSERT_SUCCESS(mythread_join(parent[i], NULL));
		ASSERT_EQ(ctx[i].rc_a, 0);
		ASSERT_EQ(ctx[i].rc_b, 0);
		ASSERT_EQ(ctx[i].val_a, (intptr_t)(0x10 + i));
		ASSERT_EQ(ctx[i].val_b, (intptr_t)(0x20 + i));
	}
#undef R11_P
	return TEST_SUCCESS;
}

/*
 * [R12] Cross-thread detach: thread A detaches thread B (both are
 * work threads, main only watches). Detaching a *live* thread from
 * another thread is well-defined POSIX — A must get 0. B then lives
 * out its detached life (nobody joins it), and the library stays
 * healthy. Strict mode additionally checks B has really become
 * unjoinable.
 */
struct r12_ctx {
	mythread_t b;
	volatile int rc_det;
	volatile int done;
	struct park_ret *pb;
};

static void *
worker_r12_detacher(void *arg)
{
	struct r12_ctx *c = (struct r12_ctx *)arg;

	__atomic_store_n(&c->rc_det, mythread_detach(c->b),
	                 __ATOMIC_SEQ_CST);
	__atomic_store_n(&c->done, 1, __ATOMIC_SEQ_CST);
	return NULL;
}

int
test_race_cross_detach(void)
{
	static struct park_ret pb;
	static struct r12_ctx c;
	static mythread_t b, a;

	pb.g.open = 0;
	pb.val = (intptr_t)0xB2;

	c.rc_det = -1;
	c.done = 0;
	c.pb = &pb;
	c.b = 0;

	ASSERT_SUCCESS(mythread_create(&b, NULL, worker_park_ret, &pb));
	c.b = b;   /* publish the handle before A exists */
	ASSERT_SUCCESS(mythread_create(&a, NULL, worker_r12_detacher,
	                               &c));

	while (!__atomic_load_n(&c.done, __ATOMIC_SEQ_CST))
		;
	/* cross-thread detach of a live thread is well-defined: 0 */
	ASSERT_EQ(c.rc_det, 0);

#ifdef MYTHREAD_STRICT_ERRORS
	/* B is alive but detached — joining it must be an explicit error,
	 * not a silent success, and it must not hang on the parked B. */
	ASSERT_FAILURE(mythread_join(b, NULL));
#endif

	/* B runs its detached life to the end; nobody ever joins it */
	__atomic_store_n(&pb.g.open, 1, __ATOMIC_SEQ_CST);

	/* the library must be intact */
	mythread_t probe;
	ASSERT_SUCCESS(mythread_create(&probe, NULL, worker_noop, NULL));
	ASSERT_SUCCESS(mythread_join(probe, NULL));
	return TEST_SUCCESS;
}
